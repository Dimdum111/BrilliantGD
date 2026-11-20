-- Press F8 in-game to show/hide the UI windows (the HUD overlay stays visible).

local win = bgd.ui.window({ title = "UI Demo", x = 60, y = 60, width = 340, closable = true })

local status = win:label("Welcome! Try the widgets below.")
status:set_color(0.4, 0.9, 1.0)

win:separator("Basics")

win:button("Click me", function()
	status:set_text("Button clicked!")
end)

win:toggle("A toggle", false, function(on)
	status:set_text("Toggle is " .. (on and "ON" or "OFF"))
end)

local speed = win:slider("Speed", 0, 10, 1, function(v)
	status:set_text(string.format("Speed = %.2f", v))
end)
speed:set_tooltip("Drag, or Ctrl+click to type a value")

win:slider_int("Count", 0, 20, 5, function(v)
	status:set_text("Count = " .. v)
end)

win:input("Name", "", function(text)
	status:set_text("Typing: " .. text)
end, {
	hint = "type something...",
	on_submit = function(text)
		bgd.log.info("Submitted: " .. text)
		status:set_text("Submitted: " .. text)
	end,
})

win:dropdown("Mode", { "Easy", "Normal", "Hard" }, 2, function(index, value)
	status:set_text("Mode " .. index .. ": " .. value)
end)

win:color("Color", { 1, 0.4, 0.2, 1 }, function(r, g, b, a)
	status:set_text(string.format("Color %.2f %.2f %.2f", r, g, b))
end)

local bar = win:progress(0)
local progress = 0
bgd.every(0.05, function()
	progress = (progress + 0.01) % 1
	bar:set_value(progress)
end)

-- Containers: frames group things, rows put them side by side, sections collapse.
local frame = win:frame("A frame", { border = true })
frame:label("Inside a frame")
local row = frame:row()
row:button("A", function() status:set_text("A") end)
row:button("B", function() status:set_text("B") end)
row:button("C", function() status:set_text("C") end)

local section = win:section("More options", true)
section:label("Collapsible section")
local removable = section:button("Remove me", function() end)
removable:set_callback(function()
	removable:remove()
	status:set_text("Button removed")
end)

-- A HUD: overlay windows are click-through and stay visible when the UI is hidden.
local hud = bgd.ui.window({ overlay = true, x = 12, y = 12 })
local fpsLabel = hud:label("FPS: ...")
local frames = 0
bgd.on("frame", function() frames += 1 end)
bgd.every(1, function()
	fpsLabel:set_text("FPS: " .. frames)
	frames = 0
end)
