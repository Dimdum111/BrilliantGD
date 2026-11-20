# Troubleshooting

BrilliantGd writes three things next to the game, inside `BrilliantGd/`:

| File | What it holds |
| --- | --- |
| `log.txt` | Everything the loader and your mods logged this run. Start here. |
| `crash.log` | If the game hit an access violation or similar, where it happened (module + offset, and a short stack). It is appended to, never cleared. Some entries are harmless first-chance exceptions. |
| `ui.ini` | Remembered UI window positions. Safe to delete. |

## The game crashes at startup

1. Open `log.txt` and `crash.log` and note the last lines.
2. Try turning features off, one at a time, with a `BrilliantGd/config.json` file:

   ```json
   { "console": false, "ui": false }
   ```

   - `"console": false` stops the console window from opening.
   - `"ui": false` stops the loader from hooking the game's rendering (mods that use `bgd.ui` then show nothing).

   If the game stops crashing with one of them off, that tells us where to look.
3. Remove everything from `BrilliantGd/mods/` to rule out a mod.
4. Delete `XInput1_4.dll` to confirm the game runs without the loader.

When you report a problem, include: your Windows version (or Proton version), `log.txt`, and `crash.log`.

## Nothing happens

No `log.txt` at all means the DLL never loaded. Check that `XInput1_4.dll` is next to `GeometryDash.exe`, and on Linux that the Steam launch option is `WINEDLLOVERRIDES="XInput1_4=n,b" %command%`.
