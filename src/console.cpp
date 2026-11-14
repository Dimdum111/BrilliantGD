#include "bgd.hpp"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

namespace bgd::console {
namespace {

std::mutex g_mutex;
std::deque<std::string> g_commands;
HANDLE g_out = INVALID_HANDLE_VALUE;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w, int len) {
    if (len <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

void writeRaw(const std::string& s) {
    std::wstring w = widen(s);
    DWORD done = 0;
    WriteConsoleW(g_out, w.data(), static_cast<DWORD>(w.size()), &done, nullptr);
}

// Swallow Ctrl+C / Ctrl+Break so they don't kill the game.
BOOL WINAPI ctrlHandler(DWORD type) { return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT; }

// BrilliantGd/config.json: { "console": false } turns the console off. Default: on.
bool wantConsole() {
    try {
        std::ifstream f(rootDir() / "config.json");
        if (!f) return true;
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return true;
        return j.value("console", true);
    } catch (...) {
        return true;
    }
}

DWORD WINAPI inputThread(LPVOID) {
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return 1;

    wchar_t buf[1024];
    DWORD n = 0;
    while (ReadConsoleW(in, buf, 1023, &n, nullptr) && n > 0) {
        std::string line = narrow(buf, static_cast<int>(n));
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        std::lock_guard lock(g_mutex);
        if (g_commands.size() < 64) g_commands.push_back(line);
    }
    return 0;
}

}  // namespace

void init_if_enabled() {
    if (g_out != INVALID_HANDLE_VALUE || !wantConsole()) return;
    if (!AllocConsole()) return;

    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    if (g_out == INVALID_HANDLE_VALUE) return;

    SetConsoleTitleW(L"BrilliantGd");
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Closing the console window would kill the game, so remove the close button.
    if (HWND w = GetConsoleWindow())
        if (HMENU menu = GetSystemMenu(w, FALSE)) DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);

    if (HANDLE t = CreateThread(nullptr, 0, inputThread, nullptr, 0, nullptr)) CloseHandle(t);

    write_log(Level::Info, "BrilliantGD Loaded. run 'help' for list of avalible commands.");
}

void print(Level level, const std::string& message) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    std::lock_guard lock(g_mutex);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[16];
    std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);

    static const char* tags[] = {"INFO ", "WARN ", "ERROR"};
    static const WORD colors[] = {
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,   // info: cyan
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,    // warn: yellow
        FOREGROUND_RED | FOREGROUND_INTENSITY,                       // error: red
    };
    const int i = static_cast<int>(level);

    SetConsoleTextAttribute(g_out, FOREGROUND_INTENSITY);  // dim grey timestamp
    writeRaw(stamp);
    SetConsoleTextAttribute(g_out, colors[i]);
    writeRaw(tags[i]);
    SetConsoleTextAttribute(g_out, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    writeRaw(" " + message + "\n");
}

void clear() {
    if (g_out == INVALID_HANDLE_VALUE) return;
    std::lock_guard lock(g_mutex);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return;
    DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
    DWORD written = 0;
    COORD home{0, 0};
    FillConsoleOutputCharacterW(g_out, L' ', cells, home, &written);
    FillConsoleOutputAttribute(g_out, info.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(g_out, home);
}

bool pop_command(std::string& out) {
    std::lock_guard lock(g_mutex);
    if (g_commands.empty()) return false;
    out = std::move(g_commands.front());
    g_commands.pop_front();
    return true;
}

}  // namespace bgd::console
