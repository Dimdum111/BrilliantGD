// BrilliantGd UI system.
//
// An overlay drawn with Dear ImGui on top of the game's OpenGL frame, exposed to mods as a
// retained-mode widget API (bgd.ui). Mods build windows/widgets once and get callbacks;
// this file draws them every frame and queues their events.
//
// Threading: everything here runs on the game's render thread (the one that calls
// SwapBuffers), the same thread that runs mod Luau code, so no locking is needed.
// Widget callbacks are queued while ImGui is drawing and run right after the frame is
// rendered, so mods can safely add/remove widgets from inside a callback.

#include "bgd.hpp"

#include <windows.h>
#include <GL/gl.h>

#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace bgd::ui {
namespace {

// ====================================================================== model

enum class Kind {
    Window, Frame, Section, Row,                       // containers
    Label, Button, Toggle, Slider, SliderInt, Input,   // widgets
    Dropdown, Color, Progress, Separator, Spacing
};

bool isContainer(Kind k) { return k == Kind::Window || k == Kind::Frame || k == Kind::Section || k == Kind::Row; }

struct Widget {
    int id = 0;
    Kind kind = Kind::Label;
    Widget* parent = nullptr;
    std::vector<std::unique_ptr<Widget>> kids;

    std::string text, tooltip, value, hint;
    std::string fmt = "%.2f";
    bool visible = true, enabled = true, wrap = false;
    float width = 0, height = 0;
    int cb = LUA_NOREF, cb2 = LUA_NOREF;  // main callback, secondary (submit / close)

    bool b = false;                       // toggle state, section open
    float f = 0, fmin = 0, fmax = 1;      // slider / progress
    int i = 0, imin = 0, imax = 100;      // int slider, dropdown index (0-based)
    float col[4] = {1, 1, 1, 1};
    bool hasColor = false;
    std::vector<std::string> opts;        // dropdown options
    std::vector<char> buf;                // text input storage
    bool dirty = true;                    // push value into ImGui on next draw
    bool password = false, multiline = false, border = true;

    // windows
    float x = 60, y = 60;
    bool closable = false, resizable = true, movable = true, titlebar = true, autosize = false, overlay = false;
    float opacity = 1.0f;
    bool posForce = false, sizeForce = false;
};

struct ModUi {
    lua_State* L = nullptr;
    std::string modId;
    std::vector<std::unique_ptr<Widget>> windows;
    std::unordered_map<int, Widget*> byId;
    int nextId = 1;
};

struct Event {
    enum Type { Click, Change, Submit, Close };
    lua_State* L = nullptr;
    int id = 0;
    Type type = Click;
    bool b = false;
    double num = 0;
    int idx = 0;
    std::string str;
    float col[4] = {0, 0, 0, 0};
};

std::unordered_map<lua_State*, std::unique_ptr<ModUi>> g_states;
std::vector<Event> g_events;

bool g_visible = true;       // master switch for non-overlay windows
int g_toggleKey = VK_F8;
float g_scale = 1.0f;

// ====================================================================== drawing

void queue(Event e) { g_events.push_back(std::move(e)); }

Event makeEvent(const ModUi& mu, const Widget& w, Event::Type t) {
    Event e;
    e.L = mu.L;
    e.id = w.id;
    e.type = t;
    return e;
}

void drawChildren(ModUi& mu, Widget& parent, bool row);

void drawWidget(ModUi& mu, Widget& w) {
    ImGui::PushID(w.id);
    if (!w.enabled) ImGui::BeginDisabled();
    const char* label = w.text.c_str();

    switch (w.kind) {
        case Kind::Label: {
            if (w.hasColor) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(w.col[0], w.col[1], w.col[2], w.col[3]));
            if (w.wrap) ImGui::TextWrapped("%s", label);
            else ImGui::TextUnformatted(label);
            if (w.hasColor) ImGui::PopStyleColor();
            break;
        }
        case Kind::Button: {
            if (ImGui::Button(label, ImVec2(w.width, w.height))) queue(makeEvent(mu, w, Event::Click));
            break;
        }
        case Kind::Toggle: {
            if (ImGui::Checkbox(label, &w.b)) {
                Event e = makeEvent(mu, w, Event::Change);
                e.b = w.b;
                queue(e);
            }
            break;
        }
        case Kind::Slider: {
            if (w.width > 0) ImGui::SetNextItemWidth(w.width);
            if (ImGui::SliderFloat(label, &w.f, w.fmin, w.fmax, w.fmt.c_str())) {
                Event e = makeEvent(mu, w, Event::Change);
                e.num = w.f;
                queue(e);
            }
            break;
        }
        case Kind::SliderInt: {
            if (w.width > 0) ImGui::SetNextItemWidth(w.width);
            if (ImGui::SliderInt(label, &w.i, w.imin, w.imax, "%d")) {
                Event e = makeEvent(mu, w, Event::Change);
                e.num = w.i;
                queue(e);
            }
            break;
        }
        case Kind::Input: {
            if (w.dirty) {
                std::fill(w.buf.begin(), w.buf.end(), 0);
                size_t n = std::min(w.value.size(), w.buf.size() - 1);
                std::copy_n(w.value.begin(), n, w.buf.begin());
                w.dirty = false;
            }
            ImGuiInputTextFlags flags = w.password ? ImGuiInputTextFlags_Password : 0;
            if (w.width > 0) ImGui::SetNextItemWidth(w.width);
            bool changed;
            if (w.multiline) {
                float h = w.height > 0 ? w.height : ImGui::GetTextLineHeight() * 6;
                changed = ImGui::InputTextMultiline(label, w.buf.data(), w.buf.size(), ImVec2(w.width, h), flags);
            } else if (!w.hint.empty()) {
                changed = ImGui::InputTextWithHint(label, w.hint.c_str(), w.buf.data(), w.buf.size(), flags);
            } else {
                changed = ImGui::InputText(label, w.buf.data(), w.buf.size(), flags);
            }
            if (changed) {
                w.value = w.buf.data();
                Event e = makeEvent(mu, w, Event::Change);
                e.str = w.value;
                queue(e);
            }
            if (!w.multiline && ImGui::IsItemDeactivated() &&
                (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                Event e = makeEvent(mu, w, Event::Submit);
                e.str = w.value;
                queue(e);
            }
            break;
        }
        case Kind::Dropdown: {
            const char* preview = (w.i >= 0 && w.i < static_cast<int>(w.opts.size())) ? w.opts[w.i].c_str() : "";
            if (w.width > 0) ImGui::SetNextItemWidth(w.width);
            if (ImGui::BeginCombo(label, preview)) {
                for (int k = 0; k < static_cast<int>(w.opts.size()); k++) {
                    bool selected = (k == w.i);
                    if (ImGui::Selectable(w.opts[k].c_str(), selected) && k != w.i) {
                        w.i = k;
                        Event e = makeEvent(mu, w, Event::Change);
                        e.idx = k + 1;
                        e.str = w.opts[k];
                        queue(e);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case Kind::Color: {
            if (ImGui::ColorEdit4(label, w.col)) {
                Event e = makeEvent(mu, w, Event::Change);
                std::copy(w.col, w.col + 4, e.col);
                queue(e);
            }
            break;
        }
        case Kind::Progress: {
            ImGui::ProgressBar(std::clamp(w.f, 0.0f, 1.0f), ImVec2(w.width > 0 ? w.width : -FLT_MIN, 0),
                               w.text.empty() ? nullptr : label);
            break;
        }
        case Kind::Separator: {
            if (w.text.empty()) ImGui::Separator();
            else ImGui::SeparatorText(label);
            break;
        }
        case Kind::Spacing: {
            ImGui::Spacing();
            break;
        }
        case Kind::Row: {
            ImGui::BeginGroup();
            drawChildren(mu, w, true);
            ImGui::EndGroup();
            break;
        }
        case Kind::Frame: {
            ImGuiChildFlags cf = (w.border ? ImGuiChildFlags_Borders : 0) | (w.height <= 0 ? ImGuiChildFlags_AutoResizeY : 0);
            if (ImGui::BeginChild("##frame", ImVec2(w.width, w.height), cf)) {
                if (!w.text.empty()) {
                    ImGui::TextUnformatted(label);
                    ImGui::Separator();
                }
                drawChildren(mu, w, false);
            }
            ImGui::EndChild();
            break;
        }
        case Kind::Section: {
            if (w.dirty) {
                ImGui::SetNextItemOpen(w.b, ImGuiCond_Always);
                w.dirty = false;
            }
            w.b = ImGui::CollapsingHeader(label);
            if (w.b) {
                ImGui::Indent();
                drawChildren(mu, w, false);
                ImGui::Unindent();
            }
            break;
        }
        case Kind::Window:
            break;
    }

    if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", w.tooltip.c_str());

    if (!w.enabled) ImGui::EndDisabled();
    ImGui::PopID();
}

void drawChildren(ModUi& mu, Widget& parent, bool row) {
    bool first = true;
    for (auto& kid : parent.kids) {
        if (!kid->visible) continue;
        if (row && !first) ImGui::SameLine();
        first = false;
        drawWidget(mu, *kid);
    }
}

void drawWindow(ModUi& mu, Widget& w) {
    if (!w.visible) return;
    if (!g_visible && !w.overlay) return;

    ImGuiWindowFlags flags = 0;
    if (!w.titlebar) flags |= ImGuiWindowFlags_NoTitleBar;
    if (!w.resizable) flags |= ImGuiWindowFlags_NoResize;
    if (!w.movable) flags |= ImGuiWindowFlags_NoMove;
    if (w.autosize) flags |= ImGuiWindowFlags_AlwaysAutoResize;
    if (w.overlay) {
        // HUD-style: always visible, click-through, never steals input.
        flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }

    ImGuiCond posCond = (w.posForce || w.overlay) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(w.x, w.y), posCond);
    w.posForce = false;
    ImGuiCond sizeCond = w.sizeForce ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowSize(ImVec2(w.width, w.height), sizeCond);
    w.sizeForce = false;
    ImGui::SetNextWindowBgAlpha(w.opacity);

    // "###" makes the ImGui ID independent of the title text, so set_text() doesn't reset the window.
    std::string title = w.text + "###bgd" + std::to_string(reinterpret_cast<uintptr_t>(&mu)) + "_" + std::to_string(w.id);
    bool open = true;
    if (ImGui::Begin(title.c_str(), w.closable ? &open : nullptr, flags)) drawChildren(mu, w, false);
    ImGui::End();

    if (w.closable && !open) {
        w.visible = false;
        queue(makeEvent(mu, w, Event::Close));
    }
}

bool anyWindowVisible() {
    for (auto& [L, mu] : g_states)
        for (auto& win : mu->windows)
            if (win->visible && (g_visible || win->overlay)) return true;
    return false;
}

// ====================================================================== event dispatch

template <class Push>
void callFn(ModUi& mu, int ref, int nargs, Push push) {
    if (ref == LUA_NOREF) return;
    lua_State* L = mu.L;
    lua_getref(L, ref);
    push(L);
    loader::arm_budget();
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        write_log(Level::Error, "[" + mu.modId + "] " + (err ? err : "(non-string error)"));
        lua_pop(L, 1);
    }
}

void dispatchEvents() {
    if (g_events.empty()) return;
    std::vector<Event> events;
    events.swap(g_events);

    for (Event& e : events) {
        auto it = g_states.find(e.L);
        if (it == g_states.end()) continue;
        ModUi& mu = *it->second;
        auto wi = mu.byId.find(e.id);
        if (wi == mu.byId.end()) continue;  // removed by an earlier callback
        Widget& w = *wi->second;
        int cb = w.cb, cb2 = w.cb2;

        switch (e.type) {
            case Event::Click:
                callFn(mu, cb, 0, [](lua_State*) {});
                break;
            case Event::Change:
                switch (w.kind) {
                    case Kind::Toggle:
                        callFn(mu, cb, 1, [&](lua_State* L) { lua_pushboolean(L, e.b); });
                        break;
                    case Kind::Slider:
                        callFn(mu, cb, 1, [&](lua_State* L) { lua_pushnumber(L, e.num); });
                        break;
                    case Kind::SliderInt:
                        callFn(mu, cb, 1, [&](lua_State* L) { lua_pushinteger(L, static_cast<int>(e.num)); });
                        break;
                    case Kind::Input:
                        callFn(mu, cb, 1, [&](lua_State* L) { lua_pushlstring(L, e.str.data(), e.str.size()); });
                        break;
                    case Kind::Dropdown:
                        callFn(mu, cb, 2, [&](lua_State* L) {
                            lua_pushinteger(L, e.idx);
                            lua_pushlstring(L, e.str.data(), e.str.size());
                        });
                        break;
                    case Kind::Color:
                        callFn(mu, cb, 4, [&](lua_State* L) {
                            for (int k = 0; k < 4; k++) lua_pushnumber(L, e.col[k]);
                        });
                        break;
                    default:
                        break;
                }
                break;
            case Event::Submit:
                callFn(mu, cb2, 1, [&](lua_State* L) { lua_pushlstring(L, e.str.data(), e.str.size()); });
                break;
            case Event::Close:
                callFn(mu, cb2, 0, [](lua_State*) {});
                break;
        }
    }
}

// ====================================================================== ImGui + hooks

bool g_ready = false;
bool g_failed = false;
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_ready) {
        ImGuiIO& io = ImGui::GetIO();

        // Toggle key: ignore auto-repeat, and don't fire while typing in a text box.
        if (msg == WM_KEYDOWN && static_cast<int>(wp) == g_toggleKey && !(lp & (1 << 30)) && !io.WantTextInput)
            g_visible = !g_visible;

        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);

        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                if (io.WantCaptureMouse) return 0;  // the click belongs to the UI, not the game
                break;
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR: case WM_SYSCHAR: case WM_UNICHAR:
                if (io.WantCaptureKeyboard) return 0;  // typing into a text box
                break;
            default:
                break;
        }
    }
    return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
}

bool initialize(HDC hdc) {
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd || !wglGetCurrentContext()) return false;  // not ready yet; try again next frame
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;

    int major = 2, minor = 1;
    if (const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION))) std::sscanf(ver, "%d.%d", &major, &minor);
    const char* glsl = major >= 3 ? "#version 130" : "#version 120";

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string ini = (rootDir() / "ui.ini").string();  // remembers window positions/sizes
    io.IniFilename = ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;     // the game keeps control of the cursor

    ImFontConfig fc;
    fc.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&fc);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(10, 10);
    style.ItemSpacing = ImVec2(8, 6);

    if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplOpenGL3_Init(glsl)) {
        write_log(Level::Error, "UI: failed to initialize ImGui (OpenGL " + std::to_string(major) + "." + std::to_string(minor) + ")");
        ImGui::DestroyContext();
        g_failed = true;
        return false;
    }

    g_hwnd = hwnd;
    g_origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
    g_ready = true;
    write_log(Level::Info, "UI ready (OpenGL " + std::to_string(major) + "." + std::to_string(minor) + ", " + glsl + "). Toggle with F8.");
    return true;
}

void renderFrame(HDC hdc) {
    if (g_failed || g_states.empty()) return;
    if (!g_ready && !initialize(hdc)) return;
    if (!anyWindowVisible() && g_events.empty()) return;

    ImGui::GetIO().FontGlobalScale = g_scale;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    for (auto& [L, mu] : g_states)
        for (auto& win : mu->windows) drawWindow(*mu, *win);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    dispatchEvents();  // run mod callbacks now that the frame is drawn
}

using SwapFn = BOOL(WINAPI*)(HDC);
SwapFn g_origGdiSwap = nullptr;
SwapFn g_origWglSwap = nullptr;
thread_local bool t_inSwap = false;  // wglSwapBuffers and SwapBuffers may call each other

BOOL WINAPI hkGdiSwap(HDC hdc) {
    if (t_inSwap) return g_origGdiSwap(hdc);
    t_inSwap = true;
    renderFrame(hdc);
    BOOL r = g_origGdiSwap(hdc);
    t_inSwap = false;
    return r;
}

BOOL WINAPI hkWglSwap(HDC hdc) {
    if (t_inSwap) return g_origWglSwap(hdc);
    t_inSwap = true;
    renderFrame(hdc);
    BOOL r = g_origWglSwap(hdc);
    t_inSwap = false;
    return r;
}

// ====================================================================== Lua helpers

ModUi& state(lua_State* L) {
    auto it = g_states.find(lua_mainthread(L));
    if (it == g_states.end()) luaL_error(L, "the UI is not available in this state");
    return *it->second;
}

Widget* widgetAt(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "__id");
    int id = lua_tointeger(L, -1);
    lua_pop(L, 1);
    ModUi& mu = state(L);
    auto it = mu.byId.find(id);
    if (it == mu.byId.end()) luaL_error(L, "this UI element was removed");
    return it->second;
}

void pushHandle(lua_State* L, const Widget& w) {
    lua_newtable(L);
    lua_pushinteger(L, w.id);
    lua_setfield(L, -2, "__id");
    luaL_getmetatable(L, "bgd.ui.widget");
    lua_setmetatable(L, -2);
}

void checkOptFn(lua_State* L, int idx) {
    if (!lua_isnoneornil(L, idx)) luaL_checktype(L, idx, LUA_TFUNCTION);
}

int takeFn(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return LUA_NOREF;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_pushvalue(L, idx);
    int ref = lua_ref(L, -1);  // Luau's lua_ref does not pop
    lua_pop(L, 1);
    return ref;
}

void dropFn(lua_State* L, int& ref) {
    if (ref != LUA_NOREF) lua_unref(L, ref);
    ref = LUA_NOREF;
}

void releaseTree(lua_State* L, ModUi& mu, Widget& w) {
    for (auto& k : w.kids) releaseTree(L, mu, *k);
    dropFn(L, w.cb);
    dropFn(L, w.cb2);
    mu.byId.erase(w.id);
}

Widget* newChild(lua_State* L, Kind kind) {  // `self` (the container) is argument 1
    ModUi& mu = state(L);
    Widget* parent = widgetAt(L, 1);
    if (!isContainer(parent->kind)) luaL_error(L, "this element cannot contain other elements");
    auto w = std::make_unique<Widget>();
    w->id = mu.nextId++;
    w->kind = kind;
    w->parent = parent;
    Widget* raw = w.get();
    parent->kids.push_back(std::move(w));
    mu.byId[raw->id] = raw;
    return raw;
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

bool optString(lua_State* L, int idx, const char* key, std::string& out) {
    lua_getfield(L, idx, key);
    bool ok = lua_type(L, -1) == LUA_TSTRING;
    if (ok) out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return ok;
}

std::vector<std::string> readStrings(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    std::vector<std::string> out;
    int n = lua_objlen(L, idx);
    for (int k = 1; k <= n; k++) {
        lua_rawgeti(L, idx, k);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        out.push_back(s ? std::string(s, len) : std::string());
        lua_pop(L, 1);
    }
    return out;
}

const char* kColorKeys[4] = {"r", "g", "b", "a"};

void readColor(lua_State* L, int idx, float out[4]) {
    luaL_checktype(L, idx, LUA_TTABLE);
    for (int k = 0; k < 4; k++) {
        lua_rawgeti(L, idx, k + 1);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, idx, kColorKeys[k]);
        }
        if (lua_isnumber(L, -1)) out[k] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

void pushColor(lua_State* L, const float c[4]) {
    lua_createtable(L, 4, 4);
    for (int k = 0; k < 4; k++) {
        lua_pushnumber(L, c[k]);
        lua_rawseti(L, -2, k + 1);
        lua_pushnumber(L, c[k]);
        lua_setfield(L, -2, kColorKeys[k]);
    }
}

void setFn(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

// ====================================================================== Lua: creating elements
// All of these are methods: container:label("text"), container:button("text", fn), ...

int l_label(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    Widget* w = newChild(L, Kind::Label);
    w->text = text;
    pushHandle(L, *w);
    return 1;
}

int l_button(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    checkOptFn(L, 3);
    Widget* w = newChild(L, Kind::Button);
    w->text = text;
    w->cb = takeFn(L, 3);
    pushHandle(L, *w);
    return 1;
}

int l_toggle(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    bool initial = lua_toboolean(L, 3) != 0;
    checkOptFn(L, 4);
    Widget* w = newChild(L, Kind::Toggle);
    w->text = text;
    w->b = initial;
    w->cb = takeFn(L, 4);
    pushHandle(L, *w);
    return 1;
}

int l_slider(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    float mn = static_cast<float>(luaL_checknumber(L, 3));
    float mx = static_cast<float>(luaL_checknumber(L, 4));
    float v = static_cast<float>(luaL_optnumber(L, 5, mn));
    checkOptFn(L, 6);
    Widget* w = newChild(L, Kind::Slider);
    w->text = text;
    w->fmin = mn;
    w->fmax = mx;
    w->f = std::clamp(v, std::min(mn, mx), std::max(mn, mx));
    w->cb = takeFn(L, 6);
    pushHandle(L, *w);
    return 1;
}

int l_sliderInt(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    int mn = luaL_checkinteger(L, 3);
    int mx = luaL_checkinteger(L, 4);
    int v = luaL_optinteger(L, 5, mn);
    checkOptFn(L, 6);
    Widget* w = newChild(L, Kind::SliderInt);
    w->text = text;
    w->imin = mn;
    w->imax = mx;
    w->i = std::clamp(v, std::min(mn, mx), std::max(mn, mx));
    w->cb = takeFn(L, 6);
    pushHandle(L, *w);
    return 1;
}

int l_input(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    std::string initial = luaL_optstring(L, 3, "");
    checkOptFn(L, 4);
    bool hasOpts = !lua_isnoneornil(L, 5);
    if (hasOpts) luaL_checktype(L, 5, LUA_TTABLE);

    Widget* w = newChild(L, Kind::Input);
    w->text = text;
    w->value = initial;
    w->cb = takeFn(L, 4);
    if (hasOpts) {
        optString(L, 5, "hint", w->hint);
        optBool(L, 5, "password", w->password);
        optBool(L, 5, "multiline", w->multiline);
        optNumber(L, 5, "width", w->width);
        optNumber(L, 5, "height", w->height);
        lua_getfield(L, 5, "on_submit");
        if (lua_isfunction(L, -1)) w->cb2 = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    w->buf.assign(w->multiline ? 16384 : 2048, 0);
    pushHandle(L, *w);
    return 1;
}

int l_dropdown(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    std::vector<std::string> opts = readStrings(L, 3);
    int selected = luaL_optinteger(L, 4, 1) - 1;
    checkOptFn(L, 5);
    Widget* w = newChild(L, Kind::Dropdown);
    w->text = text;
    w->opts = std::move(opts);
    w->i = w->opts.empty() ? -1 : std::clamp(selected, 0, static_cast<int>(w->opts.size()) - 1);
    w->cb = takeFn(L, 5);
    pushHandle(L, *w);
    return 1;
}

int l_color(lua_State* L) {
    std::string text = luaL_checkstring(L, 2);
    float col[4] = {1, 1, 1, 1};
    if (!lua_isnoneornil(L, 3)) readColor(L, 3, col);
    checkOptFn(L, 4);
    Widget* w = newChild(L, Kind::Color);
    w->text = text;
    std::copy(col, col + 4, w->col);
    w->cb = takeFn(L, 4);
    pushHandle(L, *w);
    return 1;
}

int l_progress(lua_State* L) {
    float frac = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    std::string overlay = luaL_optstring(L, 3, "");
    Widget* w = newChild(L, Kind::Progress);
    w->f = frac;
    w->text = overlay;
    pushHandle(L, *w);
    return 1;
}

int l_separator(lua_State* L) {
    std::string text = luaL_optstring(L, 2, "");
    Widget* w = newChild(L, Kind::Separator);
    w->text = text;
    pushHandle(L, *w);
    return 1;
}

int l_spacing(lua_State* L) {
    Widget* w = newChild(L, Kind::Spacing);
    pushHandle(L, *w);
    return 1;
}

int l_row(lua_State* L) {
    Widget* w = newChild(L, Kind::Row);
    pushHandle(L, *w);
    return 1;
}

int l_frame(lua_State* L) {
    std::string title = luaL_optstring(L, 2, "");
    bool hasOpts = !lua_isnoneornil(L, 3);
    if (hasOpts) luaL_checktype(L, 3, LUA_TTABLE);
    Widget* w = newChild(L, Kind::Frame);
    w->text = title;
    if (hasOpts) {
        optNumber(L, 3, "width", w->width);
        optNumber(L, 3, "height", w->height);
        optBool(L, 3, "border", w->border);
    }
    pushHandle(L, *w);
    return 1;
}

int l_section(lua_State* L) {
    std::string title = luaL_checkstring(L, 2);
    bool open = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    Widget* w = newChild(L, Kind::Section);
    w->text = title;
    w->b = open;
    pushHandle(L, *w);
    return 1;
}

// ====================================================================== Lua: element methods

int l_remove(lua_State* L) {
    ModUi& mu = state(L);
    Widget* w = widgetAt(L, 1);
    auto& owner = w->parent ? w->parent->kids : mu.windows;
    releaseTree(L, mu, *w);
    owner.erase(std::remove_if(owner.begin(), owner.end(), [&](const std::unique_ptr<Widget>& p) { return p.get() == w; }),
                owner.end());
    return 0;
}

int l_setVisible(lua_State* L) {
    widgetAt(L, 1)->visible = lua_toboolean(L, 2) != 0;
    return 0;
}
int l_show(lua_State* L) {
    widgetAt(L, 1)->visible = true;
    return 0;
}
int l_hide(lua_State* L) {
    widgetAt(L, 1)->visible = false;
    return 0;
}
int l_isVisible(lua_State* L) {
    lua_pushboolean(L, widgetAt(L, 1)->visible);
    return 1;
}
int l_setEnabled(lua_State* L) {
    widgetAt(L, 1)->enabled = lua_toboolean(L, 2) != 0;
    return 0;
}
int l_setText(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    w->text = luaL_checkstring(L, 2);
    return 0;
}
int l_getText(lua_State* L) {
    lua_pushlstring(L, widgetAt(L, 1)->text.data(), widgetAt(L, 1)->text.size());
    return 1;
}
int l_setTooltip(lua_State* L) {
    widgetAt(L, 1)->tooltip = luaL_optstring(L, 2, "");
    return 0;
}
int l_setWidth(lua_State* L) {
    widgetAt(L, 1)->width = static_cast<float>(luaL_checknumber(L, 2));
    return 0;
}
int l_setHeight(lua_State* L) {
    widgetAt(L, 1)->height = static_cast<float>(luaL_checknumber(L, 2));
    return 0;
}
int l_setWrap(lua_State* L) {
    widgetAt(L, 1)->wrap = lua_toboolean(L, 2) != 0;
    return 0;
}
int l_setCallback(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    checkOptFn(L, 2);
    dropFn(L, w->cb);
    w->cb = takeFn(L, 2);
    return 0;
}
int l_setSecondary(lua_State* L) {  // set_on_submit / set_on_close
    Widget* w = widgetAt(L, 1);
    checkOptFn(L, 2);
    dropFn(L, w->cb2);
    w->cb2 = takeFn(L, 2);
    return 0;
}

int l_setColor(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    if (lua_istable(L, 2)) {
        readColor(L, 2, w->col);
    } else {
        w->col[0] = static_cast<float>(luaL_checknumber(L, 2));
        w->col[1] = static_cast<float>(luaL_checknumber(L, 3));
        w->col[2] = static_cast<float>(luaL_checknumber(L, 4));
        w->col[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    if (w->kind == Kind::Label) w->hasColor = true;
    return 0;
}

int l_setDecimals(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    int n = std::clamp(static_cast<int>(luaL_checkinteger(L, 2)), 0, 6);
    w->fmt = "%." + std::to_string(n) + "f";  // fixed pattern only: user text never reaches printf
    return 0;
}

int l_setRange(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    if (w->kind == Kind::Slider) {
        w->fmin = static_cast<float>(luaL_checknumber(L, 2));
        w->fmax = static_cast<float>(luaL_checknumber(L, 3));
        w->f = std::clamp(w->f, std::min(w->fmin, w->fmax), std::max(w->fmin, w->fmax));
    } else if (w->kind == Kind::SliderInt) {
        w->imin = luaL_checkinteger(L, 2);
        w->imax = luaL_checkinteger(L, 3);
        w->i = std::clamp(w->i, std::min(w->imin, w->imax), std::max(w->imin, w->imax));
    } else {
        luaL_error(L, "set_range only works on sliders");
    }
    return 0;
}

int l_setOptions(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    if (w->kind != Kind::Dropdown) luaL_error(L, "set_options only works on dropdowns");
    w->opts = readStrings(L, 2);
    w->i = w->opts.empty() ? -1 : std::clamp(w->i, 0, static_cast<int>(w->opts.size()) - 1);
    return 0;
}

int l_setPosition(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    if (w->kind != Kind::Window) luaL_error(L, "set_position only works on windows");
    w->x = static_cast<float>(luaL_checknumber(L, 2));
    w->y = static_cast<float>(luaL_checknumber(L, 3));
    w->posForce = true;
    return 0;
}

int l_setSize(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    w->width = static_cast<float>(luaL_checknumber(L, 2));
    w->height = static_cast<float>(luaL_optnumber(L, 3, w->height));
    w->sizeForce = true;
    return 0;
}

int l_setOpacity(lua_State* L) {
    widgetAt(L, 1)->opacity = std::clamp(static_cast<float>(luaL_checknumber(L, 2)), 0.0f, 1.0f);
    return 0;
}

int l_setValue(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    switch (w->kind) {
        case Kind::Toggle:
            w->b = lua_toboolean(L, 2) != 0;
            break;
        case Kind::Slider:
            w->f = std::clamp(static_cast<float>(luaL_checknumber(L, 2)), std::min(w->fmin, w->fmax), std::max(w->fmin, w->fmax));
            break;
        case Kind::SliderInt:
            w->i = std::clamp(static_cast<int>(luaL_checkinteger(L, 2)), std::min(w->imin, w->imax), std::max(w->imin, w->imax));
            break;
        case Kind::Input:
            w->value = luaL_checkstring(L, 2);
            w->dirty = true;
            break;
        case Kind::Dropdown: {
            int idx = luaL_checkinteger(L, 2) - 1;
            if (idx < 0 || idx >= static_cast<int>(w->opts.size())) luaL_error(L, "dropdown index out of range");
            w->i = idx;
            break;
        }
        case Kind::Color:
            readColor(L, 2, w->col);
            break;
        case Kind::Progress:
            w->f = static_cast<float>(luaL_checknumber(L, 2));
            break;
        case Kind::Section:
            w->b = lua_toboolean(L, 2) != 0;
            w->dirty = true;
            break;
        default:
            luaL_error(L, "this element has no value");
    }
    return 0;
}

int l_getValue(lua_State* L) {
    Widget* w = widgetAt(L, 1);
    switch (w->kind) {
        case Kind::Toggle:
        case Kind::Section:
            lua_pushboolean(L, w->b);
            return 1;
        case Kind::Slider:
        case Kind::Progress:
            lua_pushnumber(L, w->f);
            return 1;
        case Kind::SliderInt:
            lua_pushinteger(L, w->i);
            return 1;
        case Kind::Input:
            lua_pushlstring(L, w->value.data(), w->value.size());
            return 1;
        case Kind::Dropdown:
            lua_pushinteger(L, w->i + 1);
            return 1;
        case Kind::Color:
            pushColor(L, w->col);
            return 1;
        default:
            luaL_error(L, "this element has no value");
    }
    return 0;
}

// ====================================================================== Lua: module functions (bgd.ui.*)

int m_window(lua_State* L) {
    ModUi& mu = state(L);
    bool has = !lua_isnoneornil(L, 1);
    if (has) luaL_checktype(L, 1, LUA_TTABLE);

    auto w = std::make_unique<Widget>();
    w->kind = Kind::Window;
    w->text = mu.modId;
    w->width = 320;
    w->height = 0;
    if (has) {
        optString(L, 1, "title", w->text);
        optNumber(L, 1, "x", w->x);
        optNumber(L, 1, "y", w->y);
        optNumber(L, 1, "width", w->width);
        optNumber(L, 1, "height", w->height);
        optBool(L, 1, "closable", w->closable);
        optBool(L, 1, "resizable", w->resizable);
        optBool(L, 1, "movable", w->movable);
        optBool(L, 1, "titlebar", w->titlebar);
        optBool(L, 1, "autosize", w->autosize);
        optBool(L, 1, "overlay", w->overlay);
        if (w->overlay) w->opacity = 0.35f;
        optNumber(L, 1, "opacity", w->opacity);
        w->opacity = std::clamp(w->opacity, 0.0f, 1.0f);
        lua_getfield(L, 1, "on_close");
        if (lua_isfunction(L, -1)) w->cb2 = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    w->id = mu.nextId++;
    Widget* raw = w.get();
    mu.windows.push_back(std::move(w));
    mu.byId[raw->id] = raw;
    pushHandle(L, *raw);
    return 1;
}

int m_show(lua_State*) {
    g_visible = true;
    return 0;
}
int m_hide(lua_State*) {
    g_visible = false;
    return 0;
}
int m_toggle(lua_State*) {
    g_visible = !g_visible;
    return 0;
}
int m_isVisible(lua_State* L) {
    lua_pushboolean(L, g_visible);
    return 1;
}
int m_setToggleKey(lua_State* L) {
    g_toggleKey = luaL_checkinteger(L, 1);
    return 0;
}
int m_setScale(lua_State* L) {
    g_scale = std::clamp(static_cast<float>(luaL_checknumber(L, 1)), 0.5f, 4.0f);
    return 0;
}
int m_screenSize(lua_State* L) {
    if (g_ready) {
        ImVec2 s = ImGui::GetIO().DisplaySize;
        lua_pushnumber(L, s.x);
        lua_pushnumber(L, s.y);
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
    }
    return 2;
}

}  // namespace

// ====================================================================== public API

void install_hooks() {
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    void* gdiSwap = gdi ? reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers")) : nullptr;
    void* wglSwap = gl ? reinterpret_cast<void*>(GetProcAddress(gl, "wglSwapBuffers")) : nullptr;

    int hooked = 0;
    if (gdiSwap && MH_CreateHook(gdiSwap, reinterpret_cast<void*>(&hkGdiSwap), reinterpret_cast<void**>(&g_origGdiSwap)) == MH_OK &&
        MH_EnableHook(gdiSwap) == MH_OK)
        hooked++;
    if (wglSwap && MH_CreateHook(wglSwap, reinterpret_cast<void*>(&hkWglSwap), reinterpret_cast<void**>(&g_origWglSwap)) == MH_OK &&
        MH_EnableHook(wglSwap) == MH_OK)
        hooked++;

    if (hooked == 0) write_log(Level::Error, "UI: could not hook SwapBuffers; bgd.ui windows will not appear");
    else write_log(Level::Info, "UI: render hooks installed (" + std::to_string(hooked) + ")");
}

void push_api(lua_State* L, const std::string& modId) {
    auto mu = std::make_unique<ModUi>();
    mu->L = L;
    mu->modId = modId;
    g_states[L] = std::move(mu);

    // Methods shared by every element handle.
    luaL_newmetatable(L, "bgd.ui.widget");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    setFn(L, "label", l_label);
    setFn(L, "button", l_button);
    setFn(L, "toggle", l_toggle);
    setFn(L, "slider", l_slider);
    setFn(L, "slider_int", l_sliderInt);
    setFn(L, "input", l_input);
    setFn(L, "dropdown", l_dropdown);
    setFn(L, "color", l_color);
    setFn(L, "progress", l_progress);
    setFn(L, "separator", l_separator);
    setFn(L, "spacing", l_spacing);
    setFn(L, "row", l_row);
    setFn(L, "frame", l_frame);
    setFn(L, "section", l_section);
    setFn(L, "remove", l_remove);
    setFn(L, "show", l_show);
    setFn(L, "hide", l_hide);
    setFn(L, "is_visible", l_isVisible);
    setFn(L, "set_visible", l_setVisible);
    setFn(L, "set_enabled", l_setEnabled);
    setFn(L, "set_text", l_setText);
    setFn(L, "get_text", l_getText);
    setFn(L, "set_tooltip", l_setTooltip);
    setFn(L, "set_width", l_setWidth);
    setFn(L, "set_height", l_setHeight);
    setFn(L, "set_wrap", l_setWrap);
    setFn(L, "set_callback", l_setCallback);
    setFn(L, "set_on_submit", l_setSecondary);
    setFn(L, "set_on_close", l_setSecondary);
    setFn(L, "set_color", l_setColor);
    setFn(L, "set_decimals", l_setDecimals);
    setFn(L, "set_range", l_setRange);
    setFn(L, "set_options", l_setOptions);
    setFn(L, "set_position", l_setPosition);
    setFn(L, "set_size", l_setSize);
    setFn(L, "set_opacity", l_setOpacity);
    setFn(L, "set_value", l_setValue);
    setFn(L, "get_value", l_getValue);
    lua_pop(L, 1);

    // The bgd.ui table itself.
    lua_newtable(L);
    setFn(L, "window", m_window);
    setFn(L, "show", m_show);
    setFn(L, "hide", m_hide);
    setFn(L, "toggle", m_toggle);
    setFn(L, "is_visible", m_isVisible);
    setFn(L, "set_toggle_key", m_setToggleKey);
    setFn(L, "set_scale", m_setScale);
    setFn(L, "screen_size", m_screenSize);
}

void forget_state(lua_State* L) {
    g_states.erase(L);
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const Event& e) { return e.L == L; }), g_events.end());
}

}  // namespace bgd::ui
