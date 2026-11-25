#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct lua_State;

namespace bgd {

namespace fs = std::filesystem;

enum class Level { Info, Warn, Error };

// <GD folder>/BrilliantGd
fs::path rootDir();

// Writes to <GD folder>/BrilliantGd/log.txt and the debugger output.
void write_log(Level level, const std::string& message);

// Reads a boolean from BrilliantGd/config.json (returns `fallback` if missing or invalid).
bool config_bool(const char* key, bool fallback);

// True when the foreground window belongs to this process.
bool game_focused();

namespace console {

// Opens a console window unless BrilliantGd/config.json says {"console": false}.
void init_if_enabled();
// Colored output; a no-op when the console is closed. (write_log calls this for you.)
void print(Level level, const std::string& message);
void clear();
// Lines the user typed. The loader drains these on the game thread.
bool pop_command(std::string& out);

}  // namespace console

namespace ui {

// Hooks SwapBuffers so mod windows can be drawn over the game. Needs MinHook initialized.
void install_hooks();
// Pushes the `bgd.ui` table for this Luau state (call once per state).
void push_api(lua_State* L, const std::string& modId);
// Drops every window/widget a state created. Call right before lua_close().
void forget_state(lua_State* L);

}  // namespace ui

namespace cui {

// bgd.cui: native cocos2d sprites, labels and buttons that live inside game scenes.
void push_api(lua_State* L, const std::string& modId, const std::string& modDir);
// bgd.scene: current(), names(), is().
void push_scene_api(lua_State* L);
// Removes every node a state created. Call right before lua_close().
void forget_state(lua_State* L);
// Once per frame on the game thread: detects scene changes, builds/removes nodes, runs button callbacks.
void update();
// Returns true once per scene change, with the new and previous scene names.
bool take_scene_change(std::string& name, std::string& previous);
// Human-readable scene details for the console `scene` command.
std::vector<std::string> describe_scene();

}  // namespace cui

namespace loader {

// Gives the next Luau call a fresh time budget (used by UI callbacks).
void arm_budget();


// Discover and run every mod in BrilliantGd/mods. Must run on the game thread.
void load_all();
void unload_all();

// Called once per rendered frame on the game thread.
void on_frame(double dt);

}  // namespace loader
}  // namespace bgd
