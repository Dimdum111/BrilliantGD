// BrilliantGd native UI (bgd.cui) and scene detection (bgd.scene).
//
// Unlike bgd.ui (a Dear ImGui overlay), bgd.cui creates real cocos2d nodes (sprites, labels
// and menu buttons) and adds them to the running scene, so they live inside the game: they
// follow its z-order, its scene changes and its touch handling.
//
// How it works without Geometry Dash bindings:
//   * Every cocos2d function is looked up BY NAME in libcocos2d.dll (it exports them).
//   * Scene names (MenuLayer, PlayLayer, ...) come from the game's built-in C++ runtime type
//     information (RTTI), read from each scene child's vtable.
//   * A mod's element is a *description*. Real nodes only exist while a matching scene is
//     running; they are rebuilt each time one appears, so a handle stays valid across scenes.
//
// Everything here runs on the game thread (from loader::on_frame, inside the drawScene hook).
// Button clicks are queued and handed to Lua on the next frame, never from inside a cocos
// touch handler, so mods can safely remove elements from their own callbacks.
//
// If libcocos2d.dll lacks a function we need, one clear error is logged and every bgd.cui
// call becomes a harmless no-op.

#include "bgd.hpp"

#include <windows.h>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bgd::cui {
namespace {

// ====================================================================== cocos2d functions

// All of these are plain C-style calls: on x64 Windows a C++ member function takes `this`
// as its first (RCX) argument, so `void f(void* self, ...)` matches it exactly.
struct KindFns {
    void (*setPosition)(void*, float, float) = nullptr;
    void (*setAnchor)(void*, const float*) = nullptr;  // takes a CCPoint const& (two floats)
    void (*setScale)(void*, float) = nullptr;
    void (*setRotation)(void*, float) = nullptr;
    void (*setVisible)(void*, bool) = nullptr;
    void (*setOpacity)(void*, uint8_t) = nullptr;
    void (*setColor)(void*, const uint8_t*) = nullptr;  // takes a ccColor3B const& (three bytes)
};

struct Fns {
    void* (*sharedDirector)() = nullptr;
    void* (*getRunningScene)(void*) = nullptr;
    void (*retain)(void*) = nullptr;
    void (*release)(void*) = nullptr;
    void* (*nodeCreate)() = nullptr;
    void (*addChild)(void*, void*, int) = nullptr;
    void (*removeFromParent)(void*, bool) = nullptr;
    void (*reorderChild)(void*, void*, int) = nullptr;  // parent->reorderChild(child, z)
    void* (*getChildren)(void*) = nullptr;
    void* (*getParent)(void*) = nullptr;
    const float* (*getContentSize)(void*) = nullptr;  // returns CCSize const&
    uint32_t (*arrayCount)(void*) = nullptr;
    void* (*arrayAt)(void*, uint32_t) = nullptr;

    KindFns node, sprite, label;

    void* (*spriteCreate)(const char*) = nullptr;
    void* (*spriteFrameCreate)(const char*) = nullptr;
    void* (*frameCache)() = nullptr;                     // optional
    void* (*frameByName)(void*, const char*) = nullptr;  // optional
    void* (*labelCreate)(const char*, const char*) = nullptr;
    void (*labelSetString)(void*, const char*) = nullptr;
    void* (*menuCreate)() = nullptr;
    void (*menuSetPriority)(void*, int) = nullptr;
    void* (*itemCreate)(void*, void*, void*, void*) = nullptr;
    void (*itemSetEnabled)(void*, bool) = nullptr;
} F;

int g_fnState = 0;  // 0 = not tried, 1 = ready, -1 = unavailable

template <class T>
T sym(HMODULE m, const char* name, std::vector<std::string>* missing) {
    void* p = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!p && missing) missing->push_back(name);
    return reinterpret_cast<T>(p);
}

bool initFns() {
    if (g_fnState != 0) return g_fnState > 0;

    HMODULE m = GetModuleHandleA("libcocos2d.dll");
    if (!m) {
        g_fnState = -1;
        write_log(Level::Error, "bgd.cui: libcocos2d.dll is not loaded, so bgd.cui and bgd.scene are unavailable");
        return false;
    }

    std::vector<std::string> miss;
    auto* req = &miss;

    F.sharedDirector = sym<void* (*)()>(m, "?sharedDirector@CCDirector@cocos2d@@SAPEAV12@XZ", req);
    F.getRunningScene = sym<void* (*)(void*)>(m, "?getRunningScene@CCDirector@cocos2d@@QEAAPEAVCCScene@2@XZ", req);
    F.retain = sym<void (*)(void*)>(m, "?retain@CCObject@cocos2d@@QEAAXXZ", req);
    F.release = sym<void (*)(void*)>(m, "?release@CCObject@cocos2d@@QEAAXXZ", req);
    F.nodeCreate = sym<void* (*)()>(m, "?create@CCNode@cocos2d@@SAPEAV12@XZ", req);
    F.addChild = sym<void (*)(void*, void*, int)>(m, "?addChild@CCNode@cocos2d@@UEAAXPEAV12@H@Z", req);
    F.removeFromParent = sym<void (*)(void*, bool)>(m, "?removeFromParentAndCleanup@CCNode@cocos2d@@UEAAX_N@Z", req);
    F.reorderChild = sym<void (*)(void*, void*, int)>(m, "?reorderChild@CCNode@cocos2d@@UEAAXPEAV12@H@Z", req);
    F.getChildren = sym<void* (*)(void*)>(m, "?getChildren@CCNode@cocos2d@@UEAAPEAVCCArray@2@XZ", req);
    F.getParent = sym<void* (*)(void*)>(m, "?getParent@CCNode@cocos2d@@UEAAPEAV12@XZ", req);
    F.getContentSize = sym<const float* (*)(void*)>(m, "?getContentSize@CCNode@cocos2d@@UEBAAEBVCCSize@2@XZ", req);
    F.arrayCount = sym<uint32_t (*)(void*)>(m, "?count@CCArray@cocos2d@@QEBAIXZ", req);
    F.arrayAt = sym<void* (*)(void*, uint32_t)>(m, "?objectAtIndex@CCArray@cocos2d@@QEAAPEAVCCObject@2@I@Z", req);

    F.node.setPosition = sym<void (*)(void*, float, float)>(m, "?setPosition@CCNode@cocos2d@@UEAAXMM@Z", req);
    F.node.setAnchor = sym<void (*)(void*, const float*)>(m, "?setAnchorPoint@CCNode@cocos2d@@UEAAXAEBVCCPoint@2@@Z", req);
    F.node.setScale = sym<void (*)(void*, float)>(m, "?setScale@CCNode@cocos2d@@UEAAXM@Z", req);
    F.node.setRotation = sym<void (*)(void*, float)>(m, "?setRotation@CCNode@cocos2d@@UEAAXM@Z", req);
    F.node.setVisible = sym<void (*)(void*, bool)>(m, "?setVisible@CCNode@cocos2d@@UEAAX_N@Z", req);

    F.spriteCreate = sym<void* (*)(const char*)>(m, "?create@CCSprite@cocos2d@@SAPEAV12@PEBD@Z", req);
    F.spriteFrameCreate = sym<void* (*)(const char*)>(m, "?createWithSpriteFrameName@CCSprite@cocos2d@@SAPEAV12@PEBD@Z", req);
    F.labelCreate = sym<void* (*)(const char*, const char*)>(m, "?create@CCLabelBMFont@cocos2d@@SAPEAV12@PEBD0@Z", req);
    F.labelSetString = sym<void (*)(void*, const char*)>(m, "?setString@CCLabelBMFont@cocos2d@@UEAAXPEBD@Z", req);
    F.menuCreate = sym<void* (*)()>(m, "?create@CCMenu@cocos2d@@SAPEAV12@XZ", req);
    F.menuSetPriority = sym<void (*)(void*, int)>(m, "?setHandlerPriority@CCMenu@cocos2d@@QEAAXH@Z", req);
    F.itemCreate = sym<void* (*)(void*, void*, void*, void*)>(
        m, "?create@CCMenuItemSprite@cocos2d@@SAPEAV12@PEAVCCNode@2@0PEAVCCObject@2@P842@EAAX1@Z@Z", req);
    F.itemSetEnabled = sym<void (*)(void*, bool)>(m, "?setEnabled@CCMenuItem@cocos2d@@UEAAX_N@Z", req);

    // Optional: class-specific versions of virtual functions (so a sprite's own override runs),
    // and the sprite-frame lookup that guards against a crash on unknown frame names.
    F.sprite = F.node;
    F.sprite.setScale = sym<void (*)(void*, float)>(m, "?setScale@CCSprite@cocos2d@@UEAAXM@Z", nullptr);
    F.sprite.setRotation = sym<void (*)(void*, float)>(m, "?setRotation@CCSprite@cocos2d@@UEAAXM@Z", nullptr);
    F.sprite.setVisible = sym<void (*)(void*, bool)>(m, "?setVisible@CCSprite@cocos2d@@UEAAX_N@Z", nullptr);
    F.sprite.setOpacity = sym<void (*)(void*, uint8_t)>(m, "?setOpacity@CCSprite@cocos2d@@UEAAXE@Z", nullptr);
    F.sprite.setColor = sym<void (*)(void*, const uint8_t*)>(m, "?setColor@CCSprite@cocos2d@@UEAAXAEBU_ccColor3B@2@@Z", nullptr);
    F.label = F.node;
    F.label.setAnchor = sym<void (*)(void*, const float*)>(m, "?setAnchorPoint@CCLabelBMFont@cocos2d@@UEAAXAEBVCCPoint@2@@Z", nullptr);
    F.label.setScale = sym<void (*)(void*, float)>(m, "?setScale@CCLabelBMFont@cocos2d@@UEAAXM@Z", nullptr);
    F.label.setOpacity = sym<void (*)(void*, uint8_t)>(m, "?setOpacity@CCLabelBMFont@cocos2d@@UEAAXE@Z", nullptr);
    F.label.setColor = sym<void (*)(void*, const uint8_t*)>(m, "?setColor@CCLabelBMFont@cocos2d@@UEAAXAEBU_ccColor3B@2@@Z", nullptr);
    // (a null override above would wipe the CCNode fallback copied from F.node; restore it)
    auto keep = [](auto& dst, auto fallback) { if (!dst) dst = fallback; };
    keep(F.sprite.setScale, F.node.setScale);
    keep(F.sprite.setRotation, F.node.setRotation);
    keep(F.sprite.setVisible, F.node.setVisible);
    keep(F.label.setAnchor, F.node.setAnchor);
    keep(F.label.setScale, F.node.setScale);

    F.frameCache = sym<void* (*)()>(m, "?sharedSpriteFrameCache@CCSpriteFrameCache@cocos2d@@SAPEAV12@XZ", nullptr);
    F.frameByName = sym<void* (*)(void*, const char*)>(
        m, "?spriteFrameByName@CCSpriteFrameCache@cocos2d@@QEAAPEAVCCSpriteFrame@2@PEBD@Z", nullptr);

    if (!miss.empty()) {
        std::string list;
        for (auto& s : miss) list += "\n  " + s;
        g_fnState = -1;
        write_log(Level::Error, "bgd.cui: this libcocos2d.dll does not export functions bgd.cui needs, so bgd.cui and "
                                "bgd.scene are disabled. Missing:" + list);
        return false;
    }
    g_fnState = 1;
    write_log(Level::Info, "bgd.cui: cocos2d functions found");
    return true;
}

// ====================================================================== safe memory reads + RTTI

bool readMem(const void* src, void* dst, size_t n) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &done) && done == n;
}

template <class T>
bool rd(uintptr_t addr, T& out) {
    return readMem(reinterpret_cast<const void*>(addr), &out, sizeof(T));
}

// Reads a C string of up to 127 characters, tolerating the end of a mapped page.
std::string readCString(uintptr_t addr) {
    char buf[128] = {};
    for (size_t n : {size_t(127), size_t(64), size_t(32), size_t(16), size_t(8)}) {
        std::memset(buf, 0, sizeof buf);
        if (readMem(reinterpret_cast<const void*>(addr), buf, n)) return std::string(buf);
    }
    return {};
}

// MSVC RTTI: object -> vtable -> (vtable[-1]) complete object locator -> type descriptor -> name
// such as ".?AVMenuLayer@@". Returns "MenuLayer", or "" if the object has no readable RTTI.
std::string readRttiName(uintptr_t vtable) {
    uintptr_t col = 0;
    if (!rd(vtable - sizeof(void*), col) || !col) return {};
    uint32_t signature = 0;
    if (!rd(col, signature)) return {};

    uintptr_t descriptor = 0;
    if (signature == 1) {  // 64-bit: 32-bit offsets from the module's base address
        uint32_t descRva = 0, selfRva = 0;
        if (!rd(col + 12, descRva) || !rd(col + 20, selfRva)) return {};
        descriptor = (col - selfRva) + descRva;
    } else if (signature == 0) {  // 32-bit: absolute pointer
        uint32_t p = 0;
        if (!rd(col + 12, p)) return {};
        descriptor = p;
    } else {
        return {};
    }

    std::string raw = readCString(descriptor + 2 * sizeof(void*));
    if (raw.size() < 5 || raw[0] != '.' || raw[1] != '?' || raw[2] != 'A') return {};
    std::string name = raw.substr(4);       // drop ".?AV" / ".?AU"
    return name.substr(0, name.find('@'));  // drop namespaces and the trailing "@@"
}

std::unordered_map<uintptr_t, std::string> g_rttiCache;

std::string rttiName(const void* object) {
    if (!object) return {};
    uintptr_t vtable = 0;
    if (!rd(reinterpret_cast<uintptr_t>(object), vtable) || !vtable) return {};
    auto it = g_rttiCache.find(vtable);
    if (it != g_rttiCache.end()) return it->second;
    std::string name = readRttiName(vtable);
    g_rttiCache[vtable] = name;
    return name;
}

// ====================================================================== scenes

struct SceneInfo {
    std::string sceneClass, primary;
    std::vector<std::string> names;  // class names of the scene's children (not counting our own nodes)
    void* firstChild = nullptr;
    bool transition = false;
};

struct SceneState {
    void* scene = nullptr;
    void* firstChild = nullptr;
    std::string primary;
    std::vector<std::string> names;
} g_scene;

std::unordered_set<void*> g_ourRoots;  // nodes we added, so scene detection can ignore them
bool g_changePending = false;
std::string g_changeName, g_changePrev;
float g_winW = 0, g_winH = 0;

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

SceneInfo inspect(void* scene) {
    SceneInfo si;
    si.sceneClass = rttiName(scene);
    if (startsWith(si.sceneClass, "CCTransition")) {  // a transition wraps the real scene; wait for it to finish
        si.transition = true;
        return si;
    }
    void* children = F.getChildren(scene);
    if (children) {
        uint32_t count = std::min<uint32_t>(F.arrayCount(children), 64);
        for (uint32_t i = 0; i < count; i++) {
            void* child = F.arrayAt(children, i);
            if (!child || g_ourRoots.count(child)) continue;
            if (!si.firstChild) si.firstChild = child;
            std::string name = rttiName(child);
            if (name.empty()) continue;
            if (std::find(si.names.begin(), si.names.end(), name) == si.names.end()) si.names.push_back(name);
            // The "main" layer is the first child that is not a plain cocos2d class (CCLayer, CCMenu, ...).
            if (si.primary.empty() && !startsWith(name, "CC")) si.primary = name;
        }
    }
    if (si.primary.empty()) si.primary = si.names.empty() ? si.sceneClass : si.names[0];
    return si;
}

void readWinSize(void* scene) {
    const float* p = F.getContentSize(scene);
    float size[2] = {0, 0};
    if (p && readMem(p, size, sizeof size) && size[0] > 1 && size[1] > 1) {
        g_winW = size[0];
        g_winH = size[1];
    }
}

// ====================================================================== element model

enum class Kind { Sprite, Label, Button };

struct Elem {
    int id = 0;
    Kind kind = Kind::Sprite;

    bool texIsGame = false;
    std::string texPath;  // game texture name, or an absolute path to a file in the mod folder
    std::string text, font = "bigFont.fnt";

    float x = 0, y = 0;
    std::string anchor = "bottomleft";  // which point of the screen x and y are measured from
    float px = 0.5f, py = 0.5f;         // which point of the element sits there (0..1)
    float scale = 1, rotation = 0, opacity = 1;
    uint8_t color[3] = {255, 255, 255};
    int z = 0;
    bool visible = true, enabled = true;
    std::vector<std::string> scenes;  // empty = every scene
    int cb = LUA_NOREF;

    // Live nodes (only while a matching scene is running)
    void* root = nullptr;  // what we added to the scene
    void* body = nullptr;  // what we move/scale (the sprite, label, or menu item)
    void* normal = nullptr;
    void* selected = nullptr;  // button images
    bool failed = false;       // creating the nodes failed; don't retry until the texture/text changes
};

struct CuiState {
    lua_State* L = nullptr;
    std::string modId;
    fs::path dir;
    std::map<int, std::unique_ptr<Elem>> elems;
    int nextId = 1;
};

std::unordered_map<lua_State*, std::unique_ptr<CuiState>> g_states;

struct Click {
    lua_State* L;
    int id;
};
std::vector<Click> g_clicks;

void logOnce(const std::string& key, const std::string& message) {
    static std::unordered_set<std::string> seen;
    if (seen.insert(key).second) write_log(Level::Error, message);
}

bool anchorFraction(const std::string& name, float& fx, float& fy) {
    static const struct {
        const char* name;
        float x, y;
    } table[] = {{"bottomleft", 0, 0}, {"bottom", .5f, 0}, {"bottomright", 1, 0}, {"left", 0, .5f}, {"center", .5f, .5f},
                 {"right", 1, .5f},    {"topleft", 0, 1},   {"top", .5f, 1},       {"topright", 1, 1}};
    for (auto& t : table) {
        if (name == t.name) {
            fx = t.x;
            fy = t.y;
            return true;
        }
    }
    return false;
}

const KindFns& bodyFns(const Elem& e) {
    return e.kind == Kind::Sprite ? F.sprite : (e.kind == Kind::Label ? F.label : F.node);
}

uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// ====================================================================== live nodes

void applyVisible(Elem& e) {
    if (!e.root) return;
    const KindFns& k = e.kind == Kind::Sprite ? F.sprite : F.node;
    k.setVisible(e.root, e.visible);
}

void applyLook(Elem& e) {
    if (!e.root) return;
    uint8_t opacity = toByte(e.opacity);
    if (e.kind == Kind::Button) {
        uint8_t dim[3] = {static_cast<uint8_t>(e.color[0] * 160 / 255), static_cast<uint8_t>(e.color[1] * 160 / 255),
                          static_cast<uint8_t>(e.color[2] * 160 / 255)};
        for (void* n : {e.normal, e.selected}) {
            if (!n) continue;
            if (F.sprite.setOpacity) F.sprite.setOpacity(n, opacity);
            if (F.sprite.setColor) F.sprite.setColor(n, n == e.selected ? dim : e.color);
        }
        return;
    }
    const KindFns& k = bodyFns(e);
    if (k.setOpacity) k.setOpacity(e.body, opacity);
    if (k.setColor) k.setColor(e.body, e.color);
}

void applyPosition(Elem& e) {
    if (!e.body) return;
    float fx = 0, fy = 0;
    anchorFraction(e.anchor, fx, fy);
    float w = g_winW > 1 ? g_winW : 569.0f, h = g_winH > 1 ? g_winH : 320.0f;
    bodyFns(e).setPosition(e.body, fx * w + e.x, fy * h + e.y);
}

void applyAll(Elem& e) {
    if (!e.root) return;
    const KindFns& k = bodyFns(e);
    applyPosition(e);
    float pivot[2] = {e.px, e.py};
    k.setAnchor(e.body, pivot);
    k.setScale(e.body, e.scale);
    k.setRotation(e.body, e.rotation);
    applyLook(e);
    applyVisible(e);
}

int touchPriority(int z) { return std::clamp(-128 - z, -100000, 100000); }  // higher z gets touches first

void* targetNode() {  // any CCObject works as a menu item's "target"; the clicked item is passed to our function
    static void* target = nullptr;
    if (!target) {
        target = F.nodeCreate();
        if (target) F.retain(target);
    }
    return target;
}

void onButton(void* /*self*/, void* sender);

void* makeSprite(const Elem& e) {
    if (e.texIsGame && F.frameCache && F.frameByName) {
        void* cache = F.frameCache();
        // createWithSpriteFrameName crashes on an unknown name, so check the cache first.
        if (cache && F.frameByName(cache, e.texPath.c_str())) return F.spriteFrameCreate(e.texPath.c_str());
    }
    return F.spriteCreate(e.texPath.c_str());  // a loose file; returns null if it can't be loaded
}

void destroyLive(Elem& e) {
    if (!e.root) return;
    if (F.getParent(e.root)) F.removeFromParent(e.root, true);  // only if the scene is still around
    F.release(e.root);
    g_ourRoots.erase(e.root);
    e.root = e.body = e.normal = e.selected = nullptr;
}

bool build(CuiState& st, Elem& e, void* scene) {
    void* root = nullptr;
    void* body = nullptr;
    void* normal = nullptr;
    void* selected = nullptr;

    if (e.kind == Kind::Sprite) {
        root = body = makeSprite(e);
    } else if (e.kind == Kind::Label) {
        root = body = F.labelCreate(e.text.c_str(), e.font.c_str());
    } else {
        normal = makeSprite(e);
        selected = makeSprite(e);
        void* target = targetNode();
        if (normal && selected && target) {
            body = F.itemCreate(normal, selected, target, reinterpret_cast<void*>(&onButton));
            if (body) {
                root = F.menuCreate();
                if (root) {
                    F.node.setPosition(root, 0, 0);  // the item is positioned in screen coordinates
                    F.addChild(root, body, 0);
                }
            }
        }
    }

    if (!root || !body) {
        e.failed = true;
        logOnce(st.modId + "#" + std::to_string(e.id),
                "[" + st.modId + "] bgd.cui could not create " +
                    (e.kind == Kind::Label ? "label \"" + e.text + "\"" : "an image from \"" + e.texPath + "\"") +
                    (e.kind == Kind::Label ? " (is the font name right?)" : " (is the texture name or file right?)"));
        return false;
    }

    e.root = root;
    e.body = body;
    e.normal = normal;
    e.selected = selected;
    F.retain(root);  // keep it alive until we release it, even if the scene is destroyed first
    g_ourRoots.insert(root);

    applyAll(e);
    F.addChild(scene, root, e.z);
    if (e.kind == Kind::Button) {
        F.menuSetPriority(root, touchPriority(e.z));
        F.itemSetEnabled(body, e.enabled);
    }
    return true;
}

bool matches(const Elem& e) {
    if (e.scenes.empty()) return true;
    for (auto& wanted : e.scenes)
        for (auto& name : g_scene.names)
            if (wanted == name) return true;
    return false;
}

void sync(CuiState& st, Elem& e, void* scene) {
    bool want = matches(e);
    if (want && !e.root && !e.failed) build(st, e, scene);
    else if (!want && e.root) destroyLive(e);
}

void onButton(void* /*self*/, void* sender) {
    for (auto& [L, st] : g_states)
        for (auto& [id, e] : st->elems)
            if (e->kind == Kind::Button && e->body && e->body == sender) {
                g_clicks.push_back({L, id});
                return;
            }
}

void dispatchClicks() {
    if (g_clicks.empty()) return;
    std::vector<Click> clicks;
    clicks.swap(g_clicks);
    for (const Click& c : clicks) {
        auto si = g_states.find(c.L);
        if (si == g_states.end()) continue;
        auto ei = si->second->elems.find(c.id);
        if (ei == si->second->elems.end()) continue;  // removed by an earlier callback
        int ref = ei->second->cb;
        if (ref == LUA_NOREF) continue;
        lua_State* L = c.L;
        lua_getref(L, ref);
        loader::arm_budget();
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            write_log(Level::Error, "[" + si->second->modId + "] " + (err ? err : "(non-string error)"));
            lua_pop(L, 1);
        }
    }
}

// ====================================================================== Lua helpers

CuiState& cstate(lua_State* L) {
    auto it = g_states.find(lua_mainthread(L));
    if (it == g_states.end()) luaL_error(L, "bgd.cui is not available in this state");
    return *it->second;
}

Elem* elemAt(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "__id");
    int id = lua_tointeger(L, -1);
    lua_pop(L, 1);
    CuiState& st = cstate(L);
    auto it = st.elems.find(id);
    if (it == st.elems.end()) luaL_error(L, "this element was removed");
    return it->second.get();
}

void setFn(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

bool safeRel(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    if (s.find("..") != std::string::npos) return false;
    if (s.find_first_of(":\\") != std::string::npos) return false;
    return s[0] != '/';
}

// "game:NAME" is one of the game's own textures; anything else is a file inside the mod's folder.
void resolveTexture(lua_State* L, CuiState& st, const std::string& spec, Elem& e) {
    e.failed = false;
    if (spec.rfind("game:", 0) == 0) {
        e.texIsGame = true;
        e.texPath = spec.substr(5);
        if (e.texPath.empty()) luaL_error(L, "empty game texture name");
        return;
    }
    if (!safeRel(spec)) luaL_error(L, "invalid texture path '%s' (use a path inside your mod folder)", spec.c_str());
    fs::path file = st.dir / spec;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) luaL_error(L, "texture file not found: %s", file.string().c_str());
    e.texIsGame = false;
    e.texPath = file.generic_string();
}

bool optNumber(lua_State* L, int idx, const char* key, float& out) {
    lua_getfield(L, idx, key);
    bool ok = lua_type(L, -1) == LUA_TNUMBER;
    if (ok) out = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return ok;
}

bool optBool(lua_State* L, int idx, const char* key, bool& out) {
    lua_getfield(L, idx, key);
    bool ok = lua_type(L, -1) == LUA_TBOOLEAN;
    if (ok) out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return ok;
}

void checkAnchorName(lua_State* L, const std::string& name) {
    float fx, fy;
    if (!anchorFraction(name, fx, fy))
        luaL_error(L, "unknown anchor '%s' (use bottomleft, bottom, bottomright, left, center, right, topleft, top or topright)",
                   name.c_str());
}

void checkFontName(lua_State* L, const std::string& font) {
    if (font != "bigFont.fnt" && font != "goldFont.fnt" && font != "chatFont.fnt")
        luaL_error(L, "unknown font '%s' (use bigFont.fnt, goldFont.fnt or chatFont.fnt)", font.c_str());
}

void readColorTable(lua_State* L, int idx, uint8_t out[3]) {
    luaL_checktype(L, idx, LUA_TTABLE);
    static const char* keys[3] = {"r", "g", "b"};
    for (int k = 0; k < 3; k++) {
        lua_rawgeti(L, idx, k + 1);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, idx, keys[k]);
        }
        if (lua_isnumber(L, -1)) out[k] = toByte(static_cast<float>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
}

std::vector<std::string> readSceneList(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    std::vector<std::string> out;
    int n = lua_objlen(L, idx);
    for (int k = 1; k <= n; k++) {
        lua_rawgeti(L, idx, k);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        if (s) out.emplace_back(s, len);
        lua_pop(L, 1);
    }
    return out;
}

// Reads the options table shared by every element type (it may be absent).
void readOptions(lua_State* L, int idx, Elem& e) {
    if (lua_isnoneornil(L, idx)) return;
    luaL_checktype(L, idx, LUA_TTABLE);

    optNumber(L, idx, "x", e.x);
    optNumber(L, idx, "y", e.y);
    lua_getfield(L, idx, "anchor");
    if (lua_type(L, -1) == LUA_TSTRING) {
        e.anchor = lua_tostring(L, -1);
        checkAnchorName(L, e.anchor);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "pivot");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        if (lua_isnumber(L, -1)) e.px = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 2);
        if (lua_isnumber(L, -1)) e.py = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    optNumber(L, idx, "scale", e.scale);
    optNumber(L, idx, "rotation", e.rotation);
    float opacity = e.opacity;
    if (optNumber(L, idx, "opacity", opacity)) e.opacity = std::clamp(opacity, 0.0f, 1.0f);
    float z = 0;
    if (optNumber(L, idx, "z", z)) e.z = static_cast<int>(z);
    optBool(L, idx, "visible", e.visible);
    optBool(L, idx, "enabled", e.enabled);

    lua_getfield(L, idx, "color");
    if (lua_istable(L, -1)) readColorTable(L, lua_gettop(L), e.color);
    lua_pop(L, 1);

    lua_getfield(L, idx, "scenes");
    if (lua_istable(L, -1)) e.scenes = readSceneList(L, lua_gettop(L));
    lua_pop(L, 1);

    lua_getfield(L, idx, "font");
    if (lua_type(L, -1) == LUA_TSTRING) {
        e.font = lua_tostring(L, -1);
        checkFontName(L, e.font);
    }
    lua_pop(L, 1);
}

int addElement(lua_State* L, CuiState& st, std::unique_ptr<Elem> e) {
    e->id = st.nextId++;
    int id = e->id;
    st.elems[id] = std::move(e);

    lua_newtable(L);
    lua_pushinteger(L, id);
    lua_setfield(L, -2, "__id");
    luaL_getmetatable(L, "bgd.cui.element");
    lua_setmetatable(L, -2);
    return 1;
}

int takeFn(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return LUA_NOREF;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_pushvalue(L, idx);
    int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return ref;
}

void dropFn(lua_State* L, int& ref) {
    if (ref != LUA_NOREF) lua_unref(L, ref);
    ref = LUA_NOREF;
}

// ====================================================================== Lua: bgd.cui functions

int c_sprite(lua_State* L) {  // bgd.cui.sprite(texture, options?)
    CuiState& st = cstate(L);
    std::string texture = luaL_checkstring(L, 1);
    auto e = std::make_unique<Elem>();
    e->kind = Kind::Sprite;
    resolveTexture(L, st, texture, *e);
    readOptions(L, 2, *e);
    return addElement(L, st, std::move(e));
}

int c_label(lua_State* L) {  // bgd.cui.label(text, options?)
    CuiState& st = cstate(L);
    std::string text = luaL_checkstring(L, 1);
    auto e = std::make_unique<Elem>();
    e->kind = Kind::Label;
    e->text = text;
    readOptions(L, 2, *e);
    return addElement(L, st, std::move(e));
}

int c_button(lua_State* L) {  // bgd.cui.button(texture, options?, callback?)
    CuiState& st = cstate(L);
    std::string texture = luaL_checkstring(L, 1);
    auto e = std::make_unique<Elem>();
    e->kind = Kind::Button;
    resolveTexture(L, st, texture, *e);
    readOptions(L, 2, *e);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TFUNCTION);
    e->cb = takeFn(L, 3);
    if (e->cb == LUA_NOREF && lua_istable(L, 2)) {  // options.on_click works too
        lua_getfield(L, 2, "on_click");
        if (lua_isfunction(L, -1)) e->cb = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    return addElement(L, st, std::move(e));
}

int c_winSize(lua_State* L) {  // bgd.cui.win_size() -> width, height of the screen in cocos2d points
    lua_pushnumber(L, g_winW);
    lua_pushnumber(L, g_winH);
    return 2;
}

int c_available(lua_State* L) {
    lua_pushboolean(L, initFns());
    return 1;
}

// ====================================================================== Lua: element methods

int m_remove(lua_State* L) {
    Elem* e = elemAt(L, 1);
    CuiState& st = cstate(L);
    int id = e->id;
    destroyLive(*e);
    dropFn(L, e->cb);
    st.elems.erase(id);
    return 0;
}

int m_setVisible(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->visible = lua_toboolean(L, 2) != 0;
    applyVisible(*e);
    return 0;
}
int m_show(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->visible = true;
    applyVisible(*e);
    return 0;
}
int m_hide(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->visible = false;
    applyVisible(*e);
    return 0;
}
int m_isVisible(lua_State* L) {
    lua_pushboolean(L, elemAt(L, 1)->visible);
    return 1;
}
int m_isLive(lua_State* L) {  // is it on screen right now (a matching scene is running and creation worked)?
    lua_pushboolean(L, elemAt(L, 1)->root != nullptr);
    return 1;
}

int m_setPosition(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->x = static_cast<float>(luaL_checknumber(L, 2));
    e->y = static_cast<float>(luaL_checknumber(L, 3));
    applyPosition(*e);
    return 0;
}
int m_getPosition(lua_State* L) {
    Elem* e = elemAt(L, 1);
    lua_pushnumber(L, e->x);
    lua_pushnumber(L, e->y);
    return 2;
}
int m_setAnchor(lua_State* L) {
    Elem* e = elemAt(L, 1);
    std::string name = luaL_checkstring(L, 2);
    checkAnchorName(L, name);
    e->anchor = name;
    applyPosition(*e);
    return 0;
}
int m_setPivot(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->px = static_cast<float>(luaL_checknumber(L, 2));
    e->py = static_cast<float>(luaL_checknumber(L, 3));
    if (e->body) {
        float pivot[2] = {e->px, e->py};
        bodyFns(*e).setAnchor(e->body, pivot);
    }
    return 0;
}
int m_setScale(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->scale = static_cast<float>(luaL_checknumber(L, 2));
    if (e->body) bodyFns(*e).setScale(e->body, e->scale);
    return 0;
}
int m_setRotation(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->rotation = static_cast<float>(luaL_checknumber(L, 2));
    if (e->body) bodyFns(*e).setRotation(e->body, e->rotation);
    return 0;
}
int m_setOpacity(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->opacity = std::clamp(static_cast<float>(luaL_checknumber(L, 2)), 0.0f, 1.0f);
    applyLook(*e);
    return 0;
}
int m_setColor(lua_State* L) {  // set_color(r, g, b) or set_color({r, g, b}); values 0..1
    Elem* e = elemAt(L, 1);
    if (lua_istable(L, 2)) {
        readColorTable(L, 2, e->color);
    } else {
        e->color[0] = toByte(static_cast<float>(luaL_checknumber(L, 2)));
        e->color[1] = toByte(static_cast<float>(luaL_checknumber(L, 3)));
        e->color[2] = toByte(static_cast<float>(luaL_checknumber(L, 4)));
    }
    applyLook(*e);
    return 0;
}
int m_setZ(lua_State* L) {
    Elem* e = elemAt(L, 1);
    e->z = luaL_checkinteger(L, 2);
    if (e->root) {
        void* parent = F.getParent(e->root);
        if (parent) F.reorderChild(parent, e->root, e->z);
        if (e->kind == Kind::Button) F.menuSetPriority(e->root, touchPriority(e->z));
    }
    return 0;
}
int m_setScenes(lua_State* L) {  // set_scenes({"MenuLayer"}) or set_scenes(nil) for every scene
    Elem* e = elemAt(L, 1);
    if (lua_isnoneornil(L, 2)) e->scenes.clear();
    else e->scenes = readSceneList(L, 2);
    return 0;  // the next frame builds or removes the nodes as needed
}
int m_setText(lua_State* L) {
    Elem* e = elemAt(L, 1);
    if (e->kind != Kind::Label) luaL_error(L, "set_text only works on labels");
    e->text = luaL_checkstring(L, 2);
    if (e->body) F.labelSetString(e->body, e->text.c_str());
    return 0;
}
int m_getText(lua_State* L) {
    Elem* e = elemAt(L, 1);
    lua_pushlstring(L, e->text.data(), e->text.size());
    return 1;
}
int m_setTexture(lua_State* L) {
    Elem* e = elemAt(L, 1);
    if (e->kind == Kind::Label) luaL_error(L, "set_texture only works on sprites and buttons");
    std::string texture = luaL_checkstring(L, 2);
    resolveTexture(L, cstate(L), texture, *e);
    destroyLive(*e);  // rebuilt with the new texture on the next frame
    return 0;
}
int m_setEnabled(lua_State* L) {
    Elem* e = elemAt(L, 1);
    if (e->kind != Kind::Button) luaL_error(L, "set_enabled only works on buttons");
    e->enabled = lua_toboolean(L, 2) != 0;
    if (e->body) F.itemSetEnabled(e->body, e->enabled);
    return 0;
}
int m_setCallback(lua_State* L) {
    Elem* e = elemAt(L, 1);
    if (e->kind != Kind::Button) luaL_error(L, "set_callback only works on buttons");
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TFUNCTION);
    dropFn(L, e->cb);
    e->cb = takeFn(L, 2);
    return 0;
}

// ====================================================================== Lua: bgd.scene

int s_current(lua_State* L) {
    if (g_scene.primary.empty()) lua_pushnil(L);
    else lua_pushlstring(L, g_scene.primary.data(), g_scene.primary.size());
    return 1;
}
int s_names(lua_State* L) {
    lua_createtable(L, static_cast<int>(g_scene.names.size()), 0);
    int i = 1;
    for (auto& n : g_scene.names) {
        lua_pushlstring(L, n.data(), n.size());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}
int s_is(lua_State* L) {  // bgd.scene.is("MenuLayer"): is that class among the scene's layers?
    std::string name = luaL_checkstring(L, 1);
    bool found = std::find(g_scene.names.begin(), g_scene.names.end(), name) != g_scene.names.end();
    lua_pushboolean(L, found);
    return 1;
}

}  // namespace

// ====================================================================== public API

void push_api(lua_State* L, const std::string& modId, const std::string& modDir) {
    auto st = std::make_unique<CuiState>();
    st->L = L;
    st->modId = modId;
    st->dir = fs::path(modDir);
    g_states[L] = std::move(st);

    luaL_newmetatable(L, "bgd.cui.element");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    setFn(L, "remove", m_remove);
    setFn(L, "show", m_show);
    setFn(L, "hide", m_hide);
    setFn(L, "is_visible", m_isVisible);
    setFn(L, "set_visible", m_setVisible);
    setFn(L, "is_live", m_isLive);
    setFn(L, "set_position", m_setPosition);
    setFn(L, "get_position", m_getPosition);
    setFn(L, "set_anchor", m_setAnchor);
    setFn(L, "set_pivot", m_setPivot);
    setFn(L, "set_scale", m_setScale);
    setFn(L, "set_rotation", m_setRotation);
    setFn(L, "set_opacity", m_setOpacity);
    setFn(L, "set_color", m_setColor);
    setFn(L, "set_z", m_setZ);
    setFn(L, "set_scenes", m_setScenes);
    setFn(L, "set_text", m_setText);
    setFn(L, "get_text", m_getText);
    setFn(L, "set_texture", m_setTexture);
    setFn(L, "set_enabled", m_setEnabled);
    setFn(L, "set_callback", m_setCallback);
    lua_pop(L, 1);

    lua_newtable(L);
    setFn(L, "sprite", c_sprite);
    setFn(L, "label", c_label);
    setFn(L, "button", c_button);
    setFn(L, "win_size", c_winSize);
    setFn(L, "available", c_available);
}

void push_scene_api(lua_State* L) {
    lua_newtable(L);
    setFn(L, "current", s_current);
    setFn(L, "names", s_names);
    setFn(L, "is", s_is);
}

void forget_state(lua_State* L) {
    auto it = g_states.find(L);
    if (it != g_states.end()) {
        if (g_fnState > 0)
            for (auto& [id, e] : it->second->elems) destroyLive(*e);
        g_states.erase(it);
    }
    g_clicks.erase(std::remove_if(g_clicks.begin(), g_clicks.end(), [&](const Click& c) { return c.L == L; }), g_clicks.end());
}

void update() {
    if (g_states.empty() || !initFns()) return;

    dispatchClicks();

    void* director = F.sharedDirector();
    if (!director) return;
    void* scene = F.getRunningScene(director);
    if (!scene) return;

    SceneInfo si = inspect(scene);
    if (si.transition) return;
    readWinSize(scene);

    if (scene != g_scene.scene || si.firstChild != g_scene.firstChild) {
        // A new scene is running: our nodes belonged to the old one.
        for (auto& [L, st] : g_states)
            for (auto& [id, e] : st->elems) destroyLive(*e);
        g_changePending = true;
        g_changePrev = g_scene.primary;
        g_changeName = si.primary;
        g_scene.scene = scene;
        g_scene.firstChild = si.firstChild;
    }
    g_scene.primary = si.primary;
    g_scene.names = si.names;

    for (auto& [L, st] : g_states)
        for (auto& [id, e] : st->elems) sync(*st, *e, scene);
}

bool take_scene_change(std::string& name, std::string& previous) {
    if (!g_changePending) return false;
    g_changePending = false;
    name = g_changeName;
    previous = g_changePrev;
    return true;
}

std::vector<std::string> describe_scene() {
    std::vector<std::string> lines;
    if (!initFns()) {
        lines.push_back("bgd.cui is unavailable (see the error above).");
        return lines;
    }
    void* director = F.sharedDirector();
    void* scene = director ? F.getRunningScene(director) : nullptr;
    if (!scene) {
        lines.push_back("No scene is running yet.");
        return lines;
    }
    char ptr[32];
    std::snprintf(ptr, sizeof ptr, "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(scene)));
    SceneInfo si = inspect(scene);
    lines.push_back(std::string("scene object ") + ptr + " of class " + (si.sceneClass.empty() ? "(unknown)" : si.sceneClass));
    if (si.transition) {
        lines.push_back("a scene transition is running; the new scene appears when it finishes");
        return lines;
    }
    lines.push_back("current scene name: " + si.primary + "   (what bgd.scene.current() returns)");
    std::string all;
    for (auto& n : si.names) all += (all.empty() ? "" : ", ") + n;
    lines.push_back("layers in the scene: " + (all.empty() ? std::string("(none)") : all));
    lines.push_back("use any of these names in `scenes = { ... }`");
    return lines;
}

}  // namespace bgd::cui
