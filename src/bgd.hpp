#pragma once
#include <filesystem>
#include <string>

namespace bgd {

namespace fs = std::filesystem;

enum class Level { Info, Warn, Error };

// <GD folder>/BrilliantGd
fs::path rootDir();

// Writes to <GD folder>/BrilliantGd/log.txt and the debugger output.
void write_log(Level level, const std::string& message);

// True when the foreground window belongs to this process.
bool game_focused();

namespace loader {

// Discover and run every mod in BrilliantGd/mods. Must run on the game thread.
void load_all();
void unload_all();

// Called once per rendered frame on the game thread.
void on_frame(double dt);

}  // namespace loader
}  // namespace bgd
