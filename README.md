# BrilliantGd

A small Geometry Dash mod loader where mods are written in [Luau](https://luau.org).

**Status: v0.1 skeleton.** It was written without being compiled or run against the game, so expect to fix a few build errors and verify the hook names (see "Things to verify").

## How it works

1. `XInput9_1_0.dll` (this project's output) sits next to `GeometryDash.exe`. The game imports the real XInput DLL, so Windows loads ours instead. It forwards every XInput call to the real DLL in System32, so controllers still work.
2. A bootstrap thread waits for `libcocos2d.dll`, then uses MinHook to hook `cocos2d::CCDirector::drawScene` (found by its exported name, so no hardcoded offsets).
3. On the first frame, every folder in `BrilliantGd/mods/` that has a `mod.json` is loaded. Each mod gets its **own sandboxed Luau VM**.
4. Every frame, timers run and `"frame"` handlers are called. A script call that runs longer than 50 ms is aborted, and a handler that throws is disabled, so a bad mod can't freeze the game or flood the log.

> **On Linux?** Skip to "Linux (Proton / Wine)" at the bottom. The Visual Studio steps below are for Windows.

## Build

Requires Visual Studio 2022 (MSVC) and CMake 3.21+. Dependencies (Luau, MinHook, nlohmann/json) are fetched automatically.

```
cmake -B build -A Win32
cmake --build build --config Release
```

Output: `build/Release/XInput9_1_0.dll`

## Install

```
GeometryDash/
  GeometryDash.exe
  XInput9_1_0.dll          <- the built DLL
  BrilliantGd/
    log.txt                <- created at runtime
    mods/
      hello-world/         <- copy example-mod/hello-world here
        mod.json
        main.luau
        util.luau
```

Press **Ctrl+Shift+R** in-game to reload all mods without restarting.

## Writing a mod

`mod.json`:

```json
{ "id": "your.mod-id", "name": "My Mod", "version": "1.0.0", "author": "You", "main": "main.luau" }
```

`id` must be lowercase letters, digits, `.`, `_` or `-`. `main` defaults to `main.luau`.

`main.luau`:

```lua
bgd.on("frame", function(dt)
	-- runs every frame
end)
```

Use `require("name")` to load `name.luau` from your mod folder (relative paths only).

## API (`bgd` global)

| Function | Description |
| --- | --- |
| `bgd.on("ready", fn)` | Called once after all mods have loaded |
| `bgd.on("frame", fn(dt))` | Called every frame; `dt` is seconds since last frame |
| `bgd.after(sec, fn)` / `bgd.every(sec, fn)` | One-shot / repeating timers; return an id |
| `bgd.cancel(id)` | Cancel a timer |
| `bgd.key_down(vk)` | Is a virtual key held (false when GD isn't focused) |
| `bgd.log.info/warn/error(...)` | Write to `BrilliantGd/log.txt` (`print` = `log.info`) |
| `bgd.base()` | Base address of the game exe |
| `bgd.module(name)` | Base address of a loaded DLL, or `nil` |
| `bgd.export(module, symbol)` | Address of a DLL export, or `nil` |
| `bgd.mem.read_*/write_*` | Raw memory access (`u8 u16 u32 i32 f32 f64 bytes`) |
| `bgd.mod` | `{ id, name, version, author, dir }` |
| `bgd.version` | Loader version |

`types/bgd.d.luau` gives editors with [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) autocomplete for all of this.

The Luau standard library is available, minus anything that touches the filesystem or OS.

## Things to verify

- **Architecture.** GD for Windows has traditionally been 32-bit (`-A Win32`). If your build is 64-bit, configure with `-A x64`; the code tries both name manglings.
- **The proxy DLL name.** Check that your `GeometryDash.exe` imports it: `dumpbin /imports GeometryDash.exe | findstr /i xinput`. If it imports a different XInput DLL, rename the output (`OUTPUT_NAME`) and adjust the `System32` path in `src/proxy.cpp`.
- **The hook target.** Check that `libcocos2d.dll` exports it: `dumpbin /exports libcocos2d.dll | findstr drawScene`. Adjust `candidates` in `src/main.cpp` if not.
- **Pinned dependencies.** `CMakeLists.txt` uses MinHook's `master`; pin it to a commit you've tested.

## Security note

`bgd.mem` and `bgd.export` give mods direct access to process memory, so a mod can do anything native code can. Only install mods you trust. Mods are sandboxed from the filesystem and OS, but not from the game process.

## Roadmap ideas

- Typed bindings to cocos2d / GD classes (generated from a bindings file, like Geode's Broma)
- Mod dependencies, settings UI, and a `.zip` mod package format
- Hooks callable from Luau (`bgd.hook(address, fn)`)
- More events: level start/exit, player death, scene change

## Linux (Proton / Wine)

Geometry Dash runs on Linux through Proton, so BrilliantGd is still a *Windows* DLL. You cross-compile it with MinGW-w64; plain `g++` would produce a Linux binary, which is no use here.

**1. Install the toolchain**

- Debian / Ubuntu: `sudo apt install mingw-w64 cmake git`, then switch MinGW to the POSIX thread model (needed for `std::mutex`):
  ```
  sudo update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-posix
  sudo update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix
  ```
- Arch: `sudo pacman -S mingw-w64-gcc cmake git`

**2. Build**

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/i686-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output: `build/XInput9_1_0.dll`. For a 64-bit game build you would need an `x86_64-w64-mingw32` toolchain file instead.

**3. Install**

Copy the DLL and the example mod into the game folder, usually
`~/.local/share/Steam/steamapps/common/Geometry Dash/`:

```
cp build/XInput9_1_0.dll "<GD folder>/"
mkdir -p "<GD folder>/BrilliantGd/mods"
cp -r example-mod/hello-world "<GD folder>/BrilliantGd/mods/"
```

**4. Tell Wine to use it**

Wine ships its own XInput DLL and prefers it by default. In Steam, right-click Geometry Dash, open Properties, and set the launch options to:

```
WINEDLLOVERRIDES="XInput9_1_0=n,b" %command%
```

Start the game and read `<GD folder>/BrilliantGd/log.txt`. It should show "Frame hook installed" and "Loaded Hello World".

To check what the game imports from Linux, run `x86_64-w64-mingw32-objdump -p GeometryDash.exe | grep -i xinput` (and `-p libcocos2d.dll` plus `grep -i drawScene` for the exports).

### Building with `make` instead of CMake

A plain Makefile is included for Linux (or an MSYS2 MinGW shell). It needs `make`, `git`, `curl` and MinGW-w64, and downloads Luau, MinHook and nlohmann/json into `deps/` on the first run:

```
make                                   # builds build/XInput9_1_0.dll
make install GD_DIR="$HOME/.local/share/Steam/steamapps/common/Geometry Dash"
make clean                             # remove build output
make distclean                         # also remove downloaded dependencies
```

Options: `make TRIPLE=x86_64-w64-mingw32` for a 64-bit build, or `make CXX=g++ CC=gcc` to use the compilers on your PATH. If a download is interrupted, run `make distclean` and try again. Remember the `WINEDLLOVERRIDES` launch option from step 4 above.

### No MinGW on your machine? Build in the cloud

The repo includes a GitHub Actions workflow (`.github/workflows/build.yml`). Push the project to a GitHub repository, open the **Actions** tab, run the **Build** workflow, then download the `BrilliantGd` artifact. It contains `XInput9_1_0.dll` and the example mod. Nothing needs to be installed locally.
