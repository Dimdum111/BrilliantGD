cmake_minimum_required(VERSION 3.21)
project(BrilliantGd LANGUAGES CXX)

# Current Geometry Dash for Windows is a 64-bit process, so configure with:
#   cmake -B build -A x64
# (use -A Win32 only for an old 32-bit GD build)
message(STATUS "Pointer size: ${CMAKE_SIZEOF_VOID_P} bytes")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Static CRT so users don't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# --- Luau (Roblox's Lua dialect) ---
set(LUAU_BUILD_CLI   OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
  GIT_REPOSITORY https://github.com/luau-lang/luau.git
  GIT_TAG        0.650)

# --- MinHook (inline hooking) ---
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        master) # pin to a release/commit once you pick one

# --- JSON for mod.json ---
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

# --- Dear ImGui (overlay UI) ---
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9)

FetchContent_MakeAvailable(luau minhook json imgui)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

add_library(BrilliantGd SHARED
  src/main.cpp
  src/console.cpp
  src/ui.cpp
  src/loader.cpp
  src/proxy.cpp)

target_include_directories(BrilliantGd PRIVATE src)
target_link_libraries(BrilliantGd PRIVATE Luau.VM Luau.Compiler minhook nlohmann_json::nlohmann_json imgui_lib opengl32 gdi32 imm32 dwmapi)
target_compile_definitions(BrilliantGd PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# The DLL masquerades as the XInput DLL that Geometry Dash imports, so Windows
# loads it automatically when the game starts. It forwards real calls to System32.
set_target_properties(BrilliantGd PROPERTIES
  OUTPUT_NAME "XInput1_4"
  PREFIX "")

if (MINGW)
  # Fully static so the DLL doesn't need libstdc++/libwinpthread inside the Wine prefix,
  # and --kill-at so the XInput exports aren't decorated (XInputGetState@8).
  target_link_options(BrilliantGd PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
  target_compile_options(BrilliantGd PRIVATE -Wno-cast-function-type)
endif()
