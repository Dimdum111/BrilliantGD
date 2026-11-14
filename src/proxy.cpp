// Forwards the XInput 1.4 API to the real DLL in System32, so the game keeps
// working while BrilliantGd gets loaded just by sitting next to the exe.
// (libcocos2d.dll imports XINPUT1_4.dll, which is why we impersonate that one.)
#include <windows.h>

#include <string>

namespace {

HMODULE realXInput() {
    static HMODULE h = [] {
        wchar_t dir[MAX_PATH];
        GetSystemDirectoryW(dir, MAX_PATH);
        std::wstring path = std::wstring(dir) + L"\\XInput1_4.dll";
        return LoadLibraryW(path.c_str());
    }();
    return h;
}

template <class F>
F resolve(const char* name) {
    HMODULE h = realXInput();
    return h ? reinterpret_cast<F>(GetProcAddress(h, name)) : nullptr;
}

}  // namespace

#if defined(_MSC_VER)
// MSVC: x86 stdcall exports are decorated (_name@bytes); x64 exports are not.
#ifdef _WIN64
#define BGD_EXPORT(name, bytes) __pragma(comment(linker, "/EXPORT:" #name "=" #name))
#else
#define BGD_EXPORT(name, bytes) __pragma(comment(linker, "/EXPORT:" #name "=_" #name "@" #bytes))
#endif
#define BGD_DLLEXPORT
#else
// MinGW: link with -Wl,--kill-at (CMakeLists/Makefile do this) so any @N suffix is dropped.
#define BGD_EXPORT(name, bytes)
#define BGD_DLLEXPORT __declspec(dllexport)
#endif

extern "C" {

BGD_DLLEXPORT DWORD WINAPI XInputGetState(DWORD index, void* state) {
    static auto fn = resolve<DWORD(WINAPI*)(DWORD, void*)>("XInputGetState");
    return fn ? fn(index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

BGD_DLLEXPORT DWORD WINAPI XInputSetState(DWORD index, void* vibration) {
    static auto fn = resolve<DWORD(WINAPI*)(DWORD, void*)>("XInputSetState");
    return fn ? fn(index, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

BGD_DLLEXPORT DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, void* caps) {
    static auto fn = resolve<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetCapabilities");
    return fn ? fn(index, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

BGD_DLLEXPORT void WINAPI XInputEnable(BOOL enable) {
    static auto fn = resolve<void(WINAPI*)(BOOL)>("XInputEnable");
    if (fn) fn(enable);
}

BGD_DLLEXPORT DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE devType, void* info) {
    static auto fn = resolve<DWORD(WINAPI*)(DWORD, BYTE, void*)>("XInputGetBatteryInformation");
    return fn ? fn(index, devType, info) : ERROR_DEVICE_NOT_CONNECTED;
}

BGD_DLLEXPORT DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, void* keystroke) {
    static auto fn = resolve<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetKeystroke");
    return fn ? fn(index, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

BGD_DLLEXPORT DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID* render, GUID* capture) {
    static auto fn = resolve<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>("XInputGetDSoundAudioDeviceGuids");
    return fn ? fn(index, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"

BGD_EXPORT(XInputGetState, 8)
BGD_EXPORT(XInputSetState, 8)
BGD_EXPORT(XInputGetCapabilities, 12)
BGD_EXPORT(XInputEnable, 4)
BGD_EXPORT(XInputGetBatteryInformation, 12)
BGD_EXPORT(XInputGetKeystroke, 12)
BGD_EXPORT(XInputGetDSoundAudioDeviceGuids, 12)
