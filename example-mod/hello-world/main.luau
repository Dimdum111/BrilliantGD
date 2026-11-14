local util = require("util")

bgd.log.info("Hello from " .. bgd.mod.name .. " v" .. bgd.mod.version)

-- Fires once, after every mod has finished loading.
bgd.on("ready", function()
	bgd.log.info("BrilliantGd " .. bgd.version .. ", game base address = " .. util.hex(bgd.base()))
end)

-- Fires every rendered frame with the time since the last frame, in seconds.
local frames = 0
bgd.on("frame", function(dt: number)
	frames += 1
end)

-- Repeating timer: report FPS every 5 seconds.
bgd.every(5, function()
	bgd.log.info(string.format("~%.0f fps", frames / 5))
	frames = 0
end)

-- Polling a key (virtual-key code 0x75 = F6). Only reports while the game window is focused.
local F6 = 0x75
local wasDown = false
bgd.on("frame", function()
	local down = bgd.key_down(F6)
	if down and not wasDown then
		bgd.log.info("F6 pressed!")
	end
	wasDown = down
end)
