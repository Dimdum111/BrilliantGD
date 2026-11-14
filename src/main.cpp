#include "bgd.hpp"

#include <windows.h>
#include <MinHook.h>

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
}

bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace bgd

namespace {

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
    bgd::write_log(Level::Info, "BrilliantGd 0.1.0 starting");

    HMODULE cocos = nullptr;
    while (!(cocos = GetModuleHandleA("libcocos2d.dll"))) Sleep(10);

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
