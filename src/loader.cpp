#include "bgd.hpp"

#include <windows.h>

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace bgd::loader {
namespace {

constexpr const char* kVersion = "0.1.0";
constexpr int kBudgetMs = 50;  // max time a single script call may run

using Clock = std::chrono::steady_clock;

struct Timer {
    int id;
    double due;
    double interval;  // <= 0 means one-shot
    int ref;
};

struct Mod {
    std::string id, name, version, author;
    fs::path dir;
    lua_State* L = nullptr;
    std::unordered_map<std::string, std::vector<int>> handlers;
    std::unordered_map<std::string, int> modules;  // require() cache
    std::vector<Timer> timers;
    int nextTimer = 1;
    double clock = 0;
};

std::vector<std::unique_ptr<Mod>> g_mods;
Clock::time_point g_deadline;

Mod* modOf(lua_State* L) { return static_cast<Mod*>(lua_callbacks(L)->userdata); }

void arm() { g_deadline = Clock::now() + std::chrono::milliseconds(kBudgetMs); }

// Stops runaway scripts (while(true) etc.) instead of freezing the game.
void interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;
    if (Clock::now() > g_deadline) luaL_error(L, "script exceeded its %d ms time budget", kBudgetMs);
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Relative paths only: no "..", no drive letters, no absolute paths.
bool safeRel(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    if (s.find("..") != std::string::npos) return false;
    if (s.find_first_of(":\\") != std::string::npos) return false;
    return s[0] != '/';
}

// Compiles Luau source and loads it as a function on top of the stack.
// On failure the error message is left on top of the stack.
bool loadChunk(lua_State* L, const std::string& src, const std::string& chunkName) {
    size_t size = 0;
    char* bytecode = luau_compile(src.data(), src.size(), nullptr, &size);
    int rc = luau_load(L, chunkName.c_str(), bytecode, size, 0);
    free(bytecode);
    return rc == 0;
}

const char* errText(lua_State* L) {
    const char* e = lua_tostring(L, -1);
    return e ? e : "(non-string error)";
}

// ---------------------------------------------------------------- logging

std::string joinArgs(lua_State* L) {
    int n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) out += '\t';
        out.append(s, len);
        lua_pop(L, 1);
    }
    return out;
}

int logWith(lua_State* L, Level level) {
    write_log(level, "[" + modOf(L)->id + "] " + joinArgs(L));
    return 0;
}
int l_info(lua_State* L) { return logWith(L, Level::Info); }
int l_warn(lua_State* L) { return logWith(L, Level::Warn); }
int l_error(lua_State* L) { return logWith(L, Level::Error); }

// ---------------------------------------------------------------- events & timers

int l_on(lua_State* L) {
    std::string ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ev != "ready" && ev != "frame") luaL_error(L, "unknown event '%s' (expected 'ready' or 'frame')", ev.c_str());
    lua_pushvalue(L, 2);
    int ref = lua_ref(L, -1);  // Luau's lua_ref does not pop
    lua_pop(L, 1);
    modOf(L)->handlers[ev].push_back(ref);
    return 0;
}

int addTimer(lua_State* L, bool repeat) {
    double secs = std::max(0.0, luaL_checknumber(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    Mod* m = modOf(L);
    int id = m->nextTimer++;
    m->timers.push_back({id, m->clock + secs, repeat ? std::max(secs, 0.001) : -1.0, ref});
    lua_pushinteger(L, id);
    return 1;
}
int l_after(lua_State* L) { return addTimer(L, false); }
int l_every(lua_State* L) { return addTimer(L, true); }

int l_cancel(lua_State* L) {
    int id = luaL_checkinteger(L, 1);
    Mod* m = modOf(L);
    auto it = std::find_if(m->timers.begin(), m->timers.end(), [&](const Timer& t) { return t.id == id; });
    if (it != m->timers.end()) {
        lua_unref(L, it->ref);
        m->timers.erase(it);
    }
    return 0;
}

bool callRef(Mod& m, int ref, const double* arg) {
    lua_State* L = m.L;
    lua_getref(L, ref);
    int nargs = 0;
    if (arg) {
        lua_pushnumber(L, *arg);
        nargs = 1;
    }
    arm();
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        write_log(Level::Error, "[" + m.id + "] " + errText(L));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void emit(Mod& m, const char* ev, const double* arg = nullptr) {
    auto it = m.handlers.find(ev);
    if (it == m.handlers.end()) return;
    std::vector<int> refs = it->second;  // copy: handlers may be added while we iterate
    for (int ref : refs) {
        if (callRef(m, ref, arg)) continue;
        // A handler that throws gets disabled so it can't spam the log every frame.
        auto& list = m.handlers[ev];
        list.erase(std::remove(list.begin(), list.end(), ref), list.end());
        lua_unref(m.L, ref);
        write_log(Level::Warn, "[" + m.id + "] handler for '" + ev + "' disabled after an error");
    }
}

void runTimers(Mod& m, double dt) {
    m.clock += dt;
    std::vector<int> due;
    for (auto& t : m.timers)
        if (t.due <= m.clock) due.push_back(t.id);

    for (int id : due) {
        auto it = std::find_if(m.timers.begin(), m.timers.end(), [&](const Timer& t) { return t.id == id; });
        if (it == m.timers.end()) continue;  // cancelled by an earlier callback
        int ref = it->ref;
        bool repeat = it->interval > 0;
        if (repeat) it->due = m.clock + it->interval;
        else m.timers.erase(it);
        callRef(m, ref, nullptr);
        if (!repeat) lua_unref(m.L, ref);
    }
}

// ---------------------------------------------------------------- require

int l_require(lua_State* L) {
    Mod* m = modOf(L);
    std::string name = luaL_checkstring(L, 1);
    if (!safeRel(name)) luaL_error(L, "invalid module name '%s'", name.c_str());

    if (auto it = m->modules.find(name); it != m->modules.end()) {
        lua_getref(L, it->second);
        return 1;
    }

    std::string src;
    if (!readFile(m->dir / (name + ".luau"), src)) luaL_error(L, "module '%s' not found", name.c_str());
    if (!loadChunk(L, src, "@" + m->id + "/" + name + ".luau")) lua_error(L);

    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    m->modules[name] = lua_ref(L, -1);
    return 1;
}

// ---------------------------------------------------------------- memory / process access
// Mods run with the full power of native code through this API; only install mods you trust.

bool safeCopy(void* dst, const void* src, size_t n) {
    // ReadProcessMemory on our own process fails cleanly on bad addresses instead of
    // raising an access violation, and works on every compiler (unlike __try).
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &done) && done == n;
}

bool safeWrite(uintptr_t addr, const void* src, size_t n) {
    void* target = reinterpret_cast<void*>(addr);
    DWORD old = 0;
    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    SIZE_T done = 0;
    bool ok = WriteProcessMemory(GetCurrentProcess(), target, src, n, &done) && done == n;
    VirtualProtect(target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    return ok;
}

uintptr_t checkAddr(lua_State* L, int i) {
    double d = luaL_checknumber(L, i);
    if (d < 0 || d > 18446744073709551615.0) luaL_error(L, "address out of range");
    return static_cast<uintptr_t>(d);
}

template <class T>
int l_read(lua_State* L) {
    uintptr_t addr = checkAddr(L, 1);
    T v{};
    if (!safeCopy(&v, reinterpret_cast<const void*>(addr), sizeof(T)))
        luaL_error(L, "unreadable memory at 0x%llX", static_cast<unsigned long long>(addr));
    lua_pushnumber(L, static_cast<double>(v));
    return 1;
}

template <class T>
int l_write(lua_State* L) {
    uintptr_t addr = checkAddr(L, 1);
    double x = luaL_checknumber(L, 2);
    T v;
    if constexpr (std::is_floating_point_v<T>) v = static_cast<T>(x);
    else v = static_cast<T>(static_cast<long long>(x));
    if (!safeWrite(addr, &v, sizeof(T)))
        luaL_error(L, "unwritable memory at 0x%llX", static_cast<unsigned long long>(addr));
    return 0;
}

int l_readBytes(lua_State* L) {
    uintptr_t addr = checkAddr(L, 1);
    int len = luaL_checkinteger(L, 2);
    if (len < 0 || len > (1 << 20)) luaL_error(L, "length must be between 0 and 1048576");
    std::string buf(static_cast<size_t>(len), '\0');
    if (!safeCopy(buf.data(), reinterpret_cast<const void*>(addr), buf.size()))
        luaL_error(L, "unreadable memory at 0x%llX", static_cast<unsigned long long>(addr));
    lua_pushlstring(L, buf.data(), buf.size());
    return 1;
}

int l_writeBytes(lua_State* L) {
    uintptr_t addr = checkAddr(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    if (!safeWrite(addr, data, len)) luaL_error(L, "unwritable memory at 0x%llX", static_cast<unsigned long long>(addr));
    return 0;
}

int l_base(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    return 1;
}

int l_module(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    HMODULE h = GetModuleHandleA(name);
    if (h) lua_pushnumber(L, static_cast<double>(reinterpret_cast<uintptr_t>(h)));
    else lua_pushnil(L);
    return 1;
}

int l_export(lua_State* L) {
    const char* mod = luaL_checkstring(L, 1);
    const char* sym = luaL_checkstring(L, 2);
    HMODULE h = GetModuleHandleA(mod);
    FARPROC p = h ? GetProcAddress(h, sym) : nullptr;
    if (p) lua_pushnumber(L, static_cast<double>(reinterpret_cast<uintptr_t>(p)));
    else lua_pushnil(L);
    return 1;
}

int l_keyDown(lua_State* L) {
    int vk = luaL_checkinteger(L, 1);
    lua_pushboolean(L, game_focused() && (GetAsyncKeyState(vk) & 0x8000) != 0);
    return 1;
}

// ---------------------------------------------------------------- state setup

void setFn(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

void setStr(lua_State* L, const char* key, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
}

void openApi(lua_State* L, Mod& m) {
    luaL_openlibs(L);

    lua_pushcfunction(L, l_info, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, l_require, "require");
    lua_setglobal(L, "require");

    lua_newtable(L);  // bgd
    setStr(L, "version", kVersion);
    setFn(L, "on", l_on);
    setFn(L, "after", l_after);
    setFn(L, "every", l_every);
    setFn(L, "cancel", l_cancel);
    setFn(L, "key_down", l_keyDown);
    setFn(L, "base", l_base);
    setFn(L, "module", l_module);
    setFn(L, "export", l_export);

    lua_newtable(L);  // bgd.log
    setFn(L, "info", l_info);
    setFn(L, "warn", l_warn);
    setFn(L, "error", l_error);
    lua_setfield(L, -2, "log");

    lua_newtable(L);  // bgd.mem
    setFn(L, "read_u8", l_read<uint8_t>);
    setFn(L, "read_u16", l_read<uint16_t>);
    setFn(L, "read_u32", l_read<uint32_t>);
    setFn(L, "read_i32", l_read<int32_t>);
    setFn(L, "read_f32", l_read<float>);
    setFn(L, "read_f64", l_read<double>);
    setFn(L, "read_bytes", l_readBytes);
    setFn(L, "write_u8", l_write<uint8_t>);
    setFn(L, "write_u16", l_write<uint16_t>);
    setFn(L, "write_u32", l_write<uint32_t>);
    setFn(L, "write_i32", l_write<int32_t>);
    setFn(L, "write_f32", l_write<float>);
    setFn(L, "write_f64", l_write<double>);
    setFn(L, "write_bytes", l_writeBytes);
    lua_setfield(L, -2, "mem");

    lua_newtable(L);  // bgd.mod
    setStr(L, "id", m.id);
    setStr(L, "name", m.name);
    setStr(L, "version", m.version);
    setStr(L, "author", m.author);
    setStr(L, "dir", m.dir.string());
    lua_setfield(L, -2, "mod");

    lua_setglobal(L, "bgd");

    // Lock down the standard libraries; each mod still gets its own writable globals.
    luaL_sandbox(L);
    luaL_sandboxthread(L);
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '.' || c == '_' || c == '-';
    });
}

void loadMod(const fs::path& dir) {
    std::string raw;
    if (!readFile(dir / "mod.json", raw)) return;  // not a mod folder

    auto m = std::make_unique<Mod>();
    m->dir = dir;
    std::string mainFile;
    try {
        json j = json::parse(raw);
        m->id = j.at("id").get<std::string>();
        m->name = j.value("name", m->id);
        m->version = j.value("version", "0.0.0");
        m->author = j.value("author", "unknown");
        mainFile = j.value("main", "main.luau");
    } catch (const std::exception& e) {
        write_log(Level::Error, "Bad mod.json in " + dir.string() + ": " + e.what());
        return;
    }

    if (!validId(m->id) || !safeRel(mainFile)) {
        write_log(Level::Error, "Invalid id or main file in " + dir.string());
        return;
    }
    for (auto& other : g_mods) {
        if (other->id == m->id) {
            write_log(Level::Error, "Duplicate mod id '" + m->id + "', skipping " + dir.string());
            return;
        }
    }

    std::string src;
    if (!readFile(dir / mainFile, src)) {
        write_log(Level::Error, "[" + m->id + "] cannot read " + mainFile);
        return;
    }

    lua_State* L = luaL_newstate();
    m->L = L;
    lua_callbacks(L)->userdata = m.get();
    lua_callbacks(L)->interrupt = interrupt;
    openApi(L, *m);

    if (!loadChunk(L, src, "@" + m->id + "/" + mainFile)) {
        write_log(Level::Error, "[" + m->id + "] " + errText(L));
        lua_close(L);
        return;
    }
    arm();
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        write_log(Level::Error, "[" + m->id + "] " + errText(L));
        lua_close(L);
        return;
    }

    write_log(Level::Info, "Loaded " + m->name + " v" + m->version + " (" + m->id + ")");
    g_mods.push_back(std::move(m));
}

void hotReloadCheck() {
    static bool wasDown = false;
    bool down = game_focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                (GetAsyncKeyState('R') & 0x8000);
    if (down && !wasDown) {
        write_log(Level::Info, "Ctrl+Shift+R: reloading mods");
        unload_all();
        load_all();
    }
    wasDown = down;
}

}  // namespace

void load_all() {
    fs::path modsDir = rootDir() / "mods";
    std::error_code ec;
    fs::create_directories(modsDir, ec);

    std::vector<fs::path> dirs;
    for (auto& entry : fs::directory_iterator(modsDir, ec))
        if (entry.is_directory()) dirs.push_back(entry.path());
    std::sort(dirs.begin(), dirs.end());

    for (auto& dir : dirs) loadMod(dir);
    write_log(Level::Info, "Loaded " + std::to_string(g_mods.size()) + " mod(s)");

    for (auto& m : g_mods) emit(*m, "ready");
}

void unload_all() {
    for (auto& m : g_mods)
        if (m->L) lua_close(m->L);
    g_mods.clear();
}

void on_frame(double dt) {
    hotReloadCheck();
    for (auto& m : g_mods) {
        runTimers(*m, dt);
        emit(*m, "frame", &dt);
    }
}

}  // namespace bgd::loader
