#include "bgd.hpp"

#include <windows.h>
#include <MinHook.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace bgd {

fs::path rootDir() {
    static const fs::path dir = [] {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path() / "BrilliantGd";
    }();
    return dir;
}

bool config_bool(const char* key, bool fallback) {
    try {
        std::ifstream f(rootDir() / "config.json");
        if (!f) return fallback;
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return fallback;
        return j.value(key, fallback);
    } catch (...) {
        return fallback;
    }
}

void write_log(Level level, const std::string& message) {
    static std::mutex mutex;
    static std::ofstream file = [] {
        std::error_code ec;
        fs::create_directories(rootDir(), ec);
        return std::ofstream(rootDir() / "log.txt", std::ios::trunc);
    }();
    static const char* tags[] = {"INFO", "WARN", "ERROR"};

    std::string line = std::string("[") + tags[static_cast<int>(level)] + "] " + message + "\n";
    std::lock_guard lock(mutex);
    OutputDebugStringA(("[BrilliantGd] " + line).c_str());
    file << line << std::flush;
    console::print(level, message);
}

bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace bgd

namespace {

// ---------------------------------------------------------------- crash log
// If the game dies with an access violation (or similar), write where it happened to
// BrilliantGd/crash.log. Uses only simple Win32 calls so it still works mid-crash.

using CaptureStackFn = USHORT(WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
HANDLE g_crashFile = INVALID_HANDLE_VALUE;
CaptureStackFn g_captureStack = nullptr;
LONG g_crashCount = 0;

void crashWrite(const char* s) {
    if (g_crashFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_crashFile, s, static_cast<DWORD>(std::strlen(s)), &written, nullptr);
}

void describeAddress(const void* addr, char* out, size_t size) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        std::snprintf(out, size, "%s+0x%llX", base ? base + 1 : path,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
    } else {
        std::snprintf(out, size, "0x%llX (unknown module)", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr)));
    }
}

LONG CALLBACK crashHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
        case 0xC0000005:  // access violation
        case 0xC000001D:  // illegal instruction
        case 0xC00000FD:  // stack overflow
        case 0xC0000094:  // integer divide by zero
        case 0xC0000096:  // privileged instruction
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_crashCount) > 6) return EXCEPTION_CONTINUE_SEARCH;

    char where[320], line[720];
    describeAddress(ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    std::snprintf(line, sizeof line, "EXCEPTION 0x%08lX at %s (thread %lu)\r\n", code, where, GetCurrentThreadId());
    crashWrite(line);

    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        std::snprintf(line, sizeof line, "  access violation: %s address 0x%llX\r\n",
                      kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
                      static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
        crashWrite(line);
    }

    if (g_captureStack) {
        void* frames[24];
        USHORT n = g_captureStack(0, 24, frames, nullptr);
        for (USHORT i = 0; i < n; i++) {
            describeAddress(frames[i], where, sizeof where);
            std::snprintf(line, sizeof line, "  #%u %s\r\n", static_cast<unsigned>(i), where);
            crashWrite(line);
        }
    }
    crashWrite("  (first-chance exception: harmless if the game keeps running)\r\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

void installCrashLog() {
    std::error_code ec;
    bgd::fs::create_directories(bgd::rootDir(), ec);
    g_crashFile = CreateFileW((bgd::rootDir() / "crash.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_captureStack = reinterpret_cast<CaptureStackFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCaptureStackBackTrace")));
    AddVectoredExceptionHandler(1, crashHandler);
    crashWrite("--- BrilliantGd session start ---\r\n");
}

// ---------------------------------------------------------------- startup timing
// Our DLL is loaded *by* libcocos2d.dll, so at that moment libcocos2d.dll is still half
// loaded. Patching it then is unsafe on real Windows. We wait until the game has created
// its window, which means every DLL has finished loading and the game is running.

struct WindowSearch {
    DWORD pid;
    bool found;
};

BOOL CALLBACK checkWindow(HWND h, LPARAM lp) {
    auto* s = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid || !IsWindowVisible(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 300 || r.bottom - r.top < 200) return TRUE;
    s->found = true;
    return FALSE;
}

bool waitForGameWindow(DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    WindowSearch search{GetCurrentProcessId(), false};
    while (GetTickCount() - start < timeoutMs) {
        EnumWindows(checkWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found) return true;
        Sleep(100);
    }
    return false;
}

#if defined(__GNUC__) && !defined(__thiscall)
#define __thiscall __attribute__((thiscall))
#endif

using DrawSceneFn = void(__thiscall*)(void*);
DrawSceneFn g_origDrawScene = nullptr;

// CCDirector::drawScene runs once per frame on the game thread. We hook it with
// __fastcall because MSVC can't declare a free function as __thiscall; on x86 the
// first two register args line up (ecx = this, edx = unused).
void __fastcall hkDrawScene(void* self, void*) {
    static bool first = true;
    static LARGE_INTEGER freq, last;

    if (first) {
        first = false;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        if (bgd::config_bool("ui", true)) bgd::ui::install_hooks();  // MinHook is initialized by now
        else bgd::write_log(bgd::Level::Info, "UI disabled in config.json");
        bgd::loader::load_all();
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(freq.QuadPart);
    last = now;

    bgd::loader::on_frame(dt);
    g_origDrawScene(self);
}

DWORD WINAPI bootstrap(LPVOID) {
    using bgd::Level;
    installCrashLog();
    bgd::write_log(Level::Info, "BrilliantGd 0.1.0 loaded; waiting for the game window");

    if (!waitForGameWindow(90000)) bgd::write_log(Level::Warn, "No game window after 90 seconds; continuing anyway");
    Sleep(250);  // let the game finish its first setup

    bgd::console::init_if_enabled();
    bgd::write_log(Level::Info, "BrilliantGd 0.1.0 starting");

    HMODULE cocos = GetModuleHandleA("libcocos2d.dll");
    if (!cocos) {
        bgd::write_log(Level::Error, "libcocos2d.dll is not loaded in this process");
        return 1;
    }

    // MSVC-mangled name of cocos2d::CCDirector::drawScene(). x86 first, then x64.
    const char* candidates[] = {
        "?drawScene@CCDirector@cocos2d@@QAEXXZ",
        "?drawScene@CCDirector@cocos2d@@QEAAXXZ",
    };
    FARPROC target = nullptr;
    for (const char* name : candidates) {
        if ((target = GetProcAddress(cocos, name))) break;
    }
    if (!target) {
        bgd::write_log(Level::Error,
                       "Could not find CCDirector::drawScene in libcocos2d.dll. "
                       "Check the export name with: dumpbin /exports libcocos2d.dll");
        return 1;
    }

    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&hkDrawScene),
                      reinterpret_cast<void**>(&g_origDrawScene)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
        bgd::write_log(Level::Error, "MinHook failed to install the frame hook");
        return 1;
    }

    bgd::write_log(Level::Info, "Frame hook installed; mods load on the first frame");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Never do real work under the loader lock; hand off to a thread.
        if (HANDLE h = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
