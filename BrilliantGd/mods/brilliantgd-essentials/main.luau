-- BrilliantGD mod manager
-- This mod shows Mods installed in BrilliantGD and gives abilites to manage them.

local win = bgd.ui.window({ title = "BrilliantGD V".. bgd.version, x = 600, y = 300, width = 600, height = 500, closable = true })
win:label("BrilliantGD V".. bgd.version .." By @Dimdum111")

win:separator("Mods")
local modsSection = win:section("Mods", true)

local function displayMods() 
    for _, mod in pairs(bgd.mods.list()) do
        bgd.log.info(mod.name)
        local sectionMod = modsSection:section(mod.name, false)

        sectionMod:label("Name: ".. mod.name)
        sectionMod:label("Author: ".. mod.author)
        sectionMod:label("Description: ".. mod.description)
        sectionMod:separator()

        sectionMod:label("Version: ".. mod.version)
        sectionMod:label("ID: ".. mod.id)
    end
end

bgd.on("ready", displayMods)