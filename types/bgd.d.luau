-- Type definitions for the BrilliantGd API (use with luau-lsp: "definitionFiles").

-- Any UI element: a window, container, or widget. Which methods apply depends on the kind.
declare class UIElement
	-- Containers (window, frame, section, row) can create children:
	function label(self, text: string): UIElement
	function button(self, text: string, callback: (() -> ())?): UIElement
	function toggle(self, text: string, initial: boolean?, callback: ((on: boolean) -> ())?): UIElement
	function slider(self, text: string, min: number, max: number, initial: number?, callback: ((value: number) -> ())?): UIElement
	function slider_int(self, text: string, min: number, max: number, initial: number?, callback: ((value: number) -> ())?): UIElement
	function input(self, text: string, initial: string?, callback: ((text: string) -> ())?, options: { hint: string?, password: boolean?, multiline: boolean?, width: number?, height: number?, on_submit: ((text: string) -> ())? }?): UIElement
	function dropdown(self, text: string, options: { string }, selected: number?, callback: ((index: number, value: string) -> ())?): UIElement
	function color(self, text: string, rgba: { number }?, callback: ((r: number, g: number, b: number, a: number) -> ())?): UIElement
	function progress(self, fraction: number?, overlay: string?): UIElement
	function separator(self, text: string?): UIElement
	function spacing(self): UIElement
	function row(self): UIElement
	function frame(self, title: string?, options: { width: number?, height: number?, border: boolean? }?): UIElement
	function section(self, title: string, open: boolean?): UIElement

	-- Any element:
	function remove(self)
	function show(self)
	function hide(self)
	function is_visible(self): boolean
	function set_visible(self, visible: boolean)
	function set_enabled(self, enabled: boolean)
	function set_text(self, text: string)
	function get_text(self): string
	function set_tooltip(self, text: string?)
	function set_width(self, width: number)
	function set_height(self, height: number)
	function set_callback(self, callback: (...any) -> ())
	function set_value(self, value: any)
	function get_value(self): any
	function set_wrap(self, wrap: boolean)
	function set_color(self, r: any, g: number?, b: number?, a: number?)
	function set_decimals(self, decimals: number)
	function set_range(self, min: number, max: number)
	function set_options(self, options: { string })
	function set_on_submit(self, callback: ((text: string) -> ())?)
	function set_on_close(self, callback: (() -> ())?)

	-- Windows:
	function set_position(self, x: number, y: number)
	function set_size(self, width: number, height: number?)
	function set_opacity(self, opacity: number)
end

declare bgd: {
	version: string,

	-- Events: "ready" (once, after all mods load) and "frame" (every frame, receives dt in seconds).
	on: ((event: "ready", callback: () -> ()) -> ()) & ((event: "frame", callback: (dt: number) -> ()) -> ()),

	-- Timers return an id you can pass to cancel().
	after: (seconds: number, callback: () -> ()) -> number,
	every: (seconds: number, callback: () -> ()) -> number,
	cancel: (id: number) -> (),

	-- Virtual-key code check; always false while the game window is unfocused.
	key_down: (vk: number) -> boolean,

	-- Process info.
	base: () -> number,
	module: (name: string) -> number?,
	export: (module: string, symbol: string) -> number?,

	log: {
		info: (...any) -> (),
		warn: (...any) -> (),
		error: (...any) -> (),
	},

	-- Raw memory access (addresses are plain numbers). Errors on invalid addresses.
	mem: {
		read_u8: (addr: number) -> number,
		read_u16: (addr: number) -> number,
		read_u32: (addr: number) -> number,
		read_i32: (addr: number) -> number,
		read_ptr: (addr: number) -> number, -- pointer-sized (8 bytes on 64-bit GD)
		read_f32: (addr: number) -> number,
		read_f64: (addr: number) -> number,
		read_bytes: (addr: number, length: number) -> string,
		write_u8: (addr: number, value: number) -> (),
		write_u16: (addr: number, value: number) -> (),
		write_u32: (addr: number, value: number) -> (),
		write_i32: (addr: number, value: number) -> (),
		write_ptr: (addr: number, value: number) -> (),
		write_f32: (addr: number, value: number) -> (),
		write_f64: (addr: number, value: number) -> (),
		write_bytes: (addr: number, data: string) -> (),
	},

	ui: {
		window: (options: {
			title: string?, x: number?, y: number?, width: number?, height: number?,
			closable: boolean?, resizable: boolean?, movable: boolean?, titlebar: boolean?,
			autosize: boolean?, overlay: boolean?, opacity: number?, on_close: (() -> ())?,
		}?) -> UIElement,
		show: () -> (),
		hide: () -> (),
		toggle: () -> (),
		is_visible: () -> boolean,
		set_toggle_key: (vk: number) -> (),
		set_scale: (scale: number) -> (),
		screen_size: () -> (number, number),
	},

	mod: {
		id: string,
		name: string,
		version: string,
		author: string,
		dir: string,
	},
}
