# BrilliantGd native UI (`bgd.cui`)

`bgd.cui` puts **sprites, labels and buttons inside the game's scenes**. They are real cocos2d
nodes, so they draw with the game, follow its z-order, and take clicks like the game's own buttons.
It complements `bgd.ui` (the ImGui overlay in [UI.md](UI.md)), which is better for settings panels and debug tools.

```lua
bgd.cui.sprite("icon.png", { scenes = { "MenuLayer" }, anchor = "topright", x = -40, y = -40 })
bgd.cui.label("Hello", { font = "goldFont.fnt", anchor = "bottomleft", x = 8, y = 8, pivot = { 0, 0 }, scale = 0.5 })
bgd.cui.button("game:GJ_button_01.png", { anchor = "bottom", y = 40 }, function() bgd.log.info("clicked") end)
```

## Creating elements

| Function | Makes |
| --- | --- |
| `bgd.cui.sprite(texture, options?)` | An image |
| `bgd.cui.label(text, options?)` | Text in one of the game's bitmap fonts |
| `bgd.cui.button(texture, options?, callback?)` | A clickable image. It dims while pressed. `options.on_click` also works. |

Each returns an element you can keep and change later.

**Textures:** a plain name is a file inside your mod folder (`"icon.png"`, `"images/logo.png"`; PNG or JPG; no `..`).
`"game:NAME"` is one of the game's own textures, for example `"game:GJ_button_01.png"`.
A missing mod file is an error at once. A wrong game texture name is logged when the element is first shown.

**Fonts:** `bigFont.fnt` (default), `goldFont.fnt` or `chatFont.fnt`.

## Options (all optional)

| Option | Default | Meaning |
| --- | --- | --- |
| `x`, `y` | 0, 0 | Offset from the anchor, in cocos2d points. **Up is positive.** |
| `anchor` | `"bottomleft"` | Where on the **screen** `x`, `y` are measured from: `bottomleft`, `bottom`, `bottomright`, `left`, `center`, `right`, `topleft`, `top`, `topright` |
| `pivot` | `{0.5, 0.5}` | Which point of the **element** sits there: `{0,0}` is its bottom-left corner, `{1,1}` its top-right |
| `scale`, `rotation` | 1, 0 | Rotation in degrees |
| `opacity` | 1 | 0 to 1 |
| `color` | white | `{r, g, b}`, each 0 to 1 |
| `z` | 0 | Draw order inside the scene: higher is on top (see Buttons) |
| `scenes` | every scene | A list of scene names. The element only exists in those scenes. |
| `visible`, `enabled` | true | `enabled` is for buttons |
| `font` | `"bigFont.fnt"` | Labels only |

Top-left corner, 10 points from the edges:
`{ anchor = "topleft", x = 10, y = -10, pivot = { 0, 1 } }`

`bgd.cui.win_size()` returns the screen width and height in points (0, 0 until the first scene is detected).

## Scenes

Each screen of the game (main menu, level select, a level) is a scene, and the game throws away everything
in a scene when you leave it. `bgd.cui` keeps a description of each element and **rebuilds it whenever a matching
scene appears**, so your handle stays valid the whole time. No `scenes` option means every scene.

Scene names are the game's layer class names (`MenuLayer`, `PlayLayer`, ...). To find them, open the console and type:

```
scene
```

```lua
bgd.scene.current()        -- "MenuLayer" (or nil before the first scene)
bgd.scene.names()          -- every layer in the scene, e.g. { "MenuLayer", "FLAlertLayer" }
bgd.scene.is("MenuLayer")  -- is that layer in the scene right now?
bgd.on("scene_change", function(name, previous) end)
```

A `scenes` entry matches any layer in the scene, so `scenes = { "FLAlertLayer" }` shows an element only while a popup is open.
During a scene transition the old state is kept until the new scene is ready.

## Changing elements

| Method | Notes |
| --- | --- |
| `:set_position(x, y)`, `:get_position()` | |
| `:set_anchor(name)`, `:set_pivot(x, y)` | |
| `:set_scale(s)`, `:set_rotation(deg)`, `:set_opacity(a)`, `:set_color(r, g, b)` | |
| `:set_z(z)` | |
| `:set_scenes(list)` | `nil` means every scene |
| `:show()`, `:hide()`, `:set_visible(b)`, `:is_visible()` | |
| `:set_text(s)`, `:get_text()` | Labels |
| `:set_texture(texture)` | Sprites and buttons |
| `:set_enabled(b)`, `:set_callback(fn)` | Buttons |
| `:is_live()` | Is it on screen right now? |
| `:remove()` | Removes it for good |

## Buttons and z-order

A button's touch priority follows its `z`: higher `z` receives clicks first, and a click on a button is not passed to anything
below it. Buttons at the same `z` use the newest first. The game's own menus use a fixed priority and I can't see their z-order,
so a mod button at `z = 0` generally wins over a game menu button it overlaps, but a popup (which uses a much higher priority)
still takes clicks first. Give a button a negative `z` if it must stay under the game's buttons.

Callbacks run on the next frame, so it is safe to `remove()` the button from its own callback.

## If something doesn't show up

- **Console error "libcocos2d.dll does not export ..."**: this version of the game lacks a function `bgd.cui` needs, so `bgd.cui` and `bgd.scene` are switched off. The error lists what is missing.
- **Nothing appears**: type `scene` in the console and check the name you used in `scenes`. Names are case-sensitive.
- **"could not create an image from ..."**: the game texture name or file is wrong, or the file isn't a PNG/JPG.
- `elem:is_live()` tells you whether the element exists on screen at this moment.

## Limits

- This only uses the game's cocos2d library, not the game's own classes, so there is no access to the player, level data or the game's own special buttons (the bouncy ones).
- Fonts are limited to the game's three bitmap fonts for now.
- Text has no wrapping or multi-line layout yet.
