# BrilliantGd documentation

BrilliantGd is a mod loader for Geometry Dash (64-bit, Windows or Linux through Proton). Mods are written in [Luau](https://luau.org), a fast, sandboxed Lua 5.1 dialect, and run inside the game.

## Install

1. Get `XInput1_4.dll` from the **BrilliantGd** artifact of the latest GitHub Actions build.
2. Put it in your Geometry Dash folder, next to `GeometryDash.exe`.
3. Create a `BrilliantGd/mods/` folder there and put your mods in it, one folder per mod.
4. **Linux/Proton only:** in Steam, set Geometry Dash's launch options to
   `WINEDLLOVERRIDES="XInput1_4=n,b" %command%`

```
Geometry Dash/
  GeometryDash.exe
  XInput1_4.dll
  BrilliantGd/
    mods/
      my-mod/
        mod.json
        main.luau
    log.txt        (created automatically)
    crash.log      (created automatically)
    config.json    (optional)
```

To uninstall, delete `XInput1_4.dll`.

When the game starts, the loader waits for the game window, then opens a console window and loads your mods.

## Writing a mod

Each mod is a folder in `BrilliantGd/mods/` with a `mod.json` and a script.

`mod.json`:
```json
{ "id": "me.my-mod", "name": "My Mod", "version": "1.0.0", "author": "Me", "main": "main.luau" }
```
`id` may use lowercase letters, digits, `.`, `_` and `-`, and must be unique. `main` defaults to `main.luau`.

`main.luau`:
```lua
bgd.log.info("Hello from " .. bgd.mod.name)

bgd.on("frame", function(dt)
	if bgd.key_down(0x75) then -- F6
		bgd.log.info("F6 held")
	end
end)
```

- Use `require("helper")` to load `helper.luau` from your mod folder.
- Mods are sandboxed: no file or OS access. They only get the `bgd` API below.
- A script call that runs longer than 50 ms is stopped. A handler that throws an error is switched off, and the error is logged.

## The `bgd` API

| Function | Description |
| --- | --- |
| `bgd.on("ready", fn)` | Runs once after all mods have loaded |
| `bgd.on("frame", fn(dt))` | Runs every frame; `dt` is seconds since the last frame |
| `bgd.after(sec, fn)` / `bgd.every(sec, fn)` | One-shot or repeating timer; returns an id |
| `bgd.cancel(id)` | Cancels a timer |
| `bgd.key_down(vk)` | Is a [virtual key](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) held (false when the game isn't focused) |
| `bgd.log.info/warn/error(...)` | Writes to the console and `log.txt` (`print` = `log.info`) |
| `bgd.base()` | Base address of the game exe |
| `bgd.module(name)` | Base address of a loaded DLL, or `nil` |
| `bgd.export(module, symbol)` | Address of a DLL export, or `nil` |
| `bgd.mem.read_*` / `write_*` | Raw memory access: `u8 u16 u32 i32 ptr f32 f64 bytes` |
| `bgd.mod` | This mod's info: `{ id, name, version, author, description, main, dir }` |
| `bgd.mods.list()` | Every installed mod, as a list of info tables (see below) |
| `bgd.mods.get(id)` | One mod's info table, or `nil` |
| `bgd.mods.is_loaded(id)` | Is that mod loaded right now |
| `bgd.mods.count()` | Returns `loaded, failed` counts |
| `bgd.mods.folder()` | Path of the `mods` folder |
| `bgd.version` | Loader version |
| `bgd.ui` | Windows and widgets drawn over the game, see [UI.md](UI.md) |

Editors with [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) can load `types/bgd.d.luau` for autocomplete.

There are no Geometry Dash bindings yet, so mods can't call game functions by name. They can read and write memory if you know the addresses.

### Installed mods

`bgd.mods.list()` returns a list with one table per mod folder that has a `mod.json`:

| Field | Meaning |
| --- | --- |
| `id`, `name`, `version`, `author`, `description` | From `mod.json` (`description` is optional) |
| `main`, `dir` | Script file name and the mod's folder |
| `status` | `"loaded"` or `"failed"` |
| `error` | Why it failed (only when `status` is `"failed"`) |
| `index` | Load order, starting at 1 (0 for failed mods) |
| `is_self` | `true` for the mod that is asking |
| `manifest` | The whole `mod.json` as a table, so you can read your own extra fields |

Call it inside `bgd.on("ready", ...)` to see every mod. While mods are still loading, a mod only sees the ones loaded before it. The list is a snapshot, so call it again to refresh. Mods can see each other's info but not each other's code.

```lua
bgd.on("ready", function()
	for _, mod in ipairs(bgd.mods.list()) do
		bgd.log.info(mod.name, mod.version, mod.status)
	end
end)
```

## Console

The console window shows live logs. You can type:

| Command | What it does |
| --- | --- |
| `help` | List commands |
| `mods` | List loaded mods |
| `reload` | Reload all mods |
| `clear` | Clear the console |
| `lua <code>` | Run Luau in a sandbox with the `bgd` API, e.g. `lua bgd.base()` |

**Ctrl+Shift+R** in the game also reloads all mods. The console's close button is removed on purpose, since closing it would close the game.

## Configuration

Optional file `BrilliantGd/config.json`:

```json
{ "console": true, "ui": true, "xinput_passthrough": false }
```

| Key | Default | Meaning |
| --- | --- | --- |
| `console` | `true` | Open the console window |
| `ui` | `true` | Allow `bgd.ui` windows (hooks the game's rendering) |
| `xinput_passthrough` | `false` | Forward gamepad input to the real Windows XInput. Off by default, so controllers do nothing while BrilliantGd is installed. |

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md). When reporting a problem, include `BrilliantGd/log.txt` and `BrilliantGd/crash.log`.

## Security

`bgd.mem` and `bgd.export` give mods direct access to the game's memory, so a mod can do anything native code can. Only install mods you trust.

## Building

Run `make` on Linux (needs MinGW-w64) or use the included GitHub Actions workflow, which builds `XInput1_4.dll` for you. See the main README for details.
