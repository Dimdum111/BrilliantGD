-- Type definitions for the BrilliantGd API (use with luau-lsp: "definitionFiles").

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

	mod: {
		id: string,
		name: string,
		version: string,
		author: string,
		dir: string,
	},
}
