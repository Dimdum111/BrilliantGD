local util = {}

function util.hex(n: number): string
	return string.format("0x%X", n)
end

return util
