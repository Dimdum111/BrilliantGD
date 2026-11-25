-- Native in-game UI demo. Type `scene` in the console to see scene names.

-- 1. An image from this mod's folder, only on the main menu (top right corner)
bgd.cui.sprite("icon.png", {
	scenes = { "MenuLayer" },
	anchor = "topright", x = -40, y = -40,
})

-- 2. A label in the game's gold font, in every scene (bottom left corner)
local label = bgd.cui.label("BrilliantGD", {
	font = "goldFont.fnt",
	anchor = "bottomleft", x = 8, y = 8, pivot = { 0, 0 },
	scale = 0.45, opacity = 0.8,
})

-- 3. A button using one of the game's own textures, on the main menu only
local clicks = 0
bgd.cui.button("game:GJ_button_01.png", {
	scenes = { "MenuLayer" },
	anchor = "bottom", x = 0, y = 40,
	z = 10,
}, function()
	clicks += 1
	label:set_text("Clicked " .. clicks)
end)

bgd.on("scene_change", function(name, previous)
	bgd.log.info("scene:", previous or "(none)", "->", name)
end)
