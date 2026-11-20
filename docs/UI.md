# BrilliantGd UI

Mods can draw windows and widgets over the game with `bgd.ui`. The UI is drawn with
[Dear ImGui](https://github.com/ocornut/imgui) on top of the game's OpenGL frame.

- **F8** shows or hides all normal windows. Change it with `bgd.ui.set_toggle_key(vk)`.
- **Overlay** windows (HUDs) stay visible and never take mouse or keyboard input.
- While the mouse is over a window, or you're typing in a text box, clicks and keys go to the UI instead of the game.
- Window positions and sizes are remembered in `BrilliantGd/ui.ini`.
- The game controls the mouse cursor. If a level hides it, you can't click the UI there.

You build the UI once, then react to callbacks. There is no draw loop to write.

```lua
local win = bgd.ui.window({ title = "My Mod", x = 50, y = 50 })
local label = win:label("Hello")
win:button("Click me", function() label:set_text("Clicked!") end)
```

## Windows

`bgd.ui.window(options?)` returns a window. Options (all optional):

| Option | Default | Meaning |
| --- | --- | --- |
| `title` | mod id | Title bar text |
| `x`, `y` | 60, 60 | Starting position (a position saved in `ui.ini` wins) |
| `width`, `height` | 320, auto | Starting size (height 0 fits the content) |
| `closable` | false | Shows an X button |
| `resizable`, `movable`, `titlebar` | true | |
| `autosize` | false | Always fit the content |
| `overlay` | false | HUD: click-through, no title bar, always visible |
| `opacity` | 1 (0.35 for overlays) | Background opacity, 0 to 1 |
| `on_close` | none | Called when the user closes the window |

## Creating elements

Windows, frames, sections and rows are **containers**. Call these on any container.
Each returns an element you can keep and change later. Callbacks are optional.

| Method | Callback arguments |
| --- | --- |
| `:label(text)` | |
| `:button(text, callback)` | |
| `:toggle(text, initial, callback)` | `on` (boolean) |
| `:slider(text, min, max, initial, callback)` | `value` (number) |
| `:slider_int(text, min, max, initial, callback)` | `value` (integer) |
| `:input(text, initial, callback, options)` | `text` (called on every edit) |
| `:dropdown(text, options, selected, callback)` | `index` (1-based), `value` |
| `:color(text, {r, g, b, a}, callback)` | `r, g, b, a` (0 to 1) |
| `:progress(fraction, overlayText)` | |
| `:separator(text)` | |
| `:spacing()` | |
| `:row()` | container: its children sit side by side |
| `:frame(title, {width, height, border})` | container: a bordered group |
| `:section(title, open)` | container: a collapsible header |

`:input` options: `hint`, `password`, `multiline`, `width`, `height`, `on_submit(text)` (Enter pressed).

## Changing elements

| Method | Works on |
| --- | --- |
| `:set_text(s)` / `:get_text()` | everything with text (window title, label, button, ...) |
| `:set_value(v)` / `:get_value()` | toggle (boolean), slider/progress (number), slider_int (integer), input (string), dropdown (1-based index), color (`{r,g,b,a}`), section (open) |
| `:show()` / `:hide()` / `:set_visible(b)` / `:is_visible()` | everything |
| `:set_enabled(b)` | everything (greys out and disables) |
| `:set_tooltip(s)` | everything |
| `:set_width(n)` / `:set_height(n)` | sized widgets, frames, windows |
| `:set_callback(fn)` | widgets with a callback |
| `:set_on_submit(fn)` / `:set_on_close(fn)` | input / window |
| `:set_color(r, g, b, a)` | labels (text colour) |
| `:set_wrap(b)` | labels |
| `:set_decimals(n)` | float sliders |
| `:set_range(min, max)` | sliders |
| `:set_options(list)` | dropdowns |
| `:set_position(x, y)`, `:set_size(w, h)`, `:set_opacity(a)` | windows |
| `:remove()` | everything (removes its children too) |

`set_value` does not fire the callback; callbacks only run for user input.
A removed element errors if you use it again.

## Global functions

`bgd.ui.show()`, `hide()`, `toggle()`, `is_visible()`, `set_toggle_key(vk)`,
`set_scale(s)` (0.5 to 4, UI size), `screen_size()` returns width, height.

## Limits

- Text inputs hold up to ~2000 characters (16000 for multiline).
- Callbacks run right after the frame is drawn, under the same 50 ms script limit as everything else.
- The `lua` console command can build UI too, but the windows disappear when you `reload`.
