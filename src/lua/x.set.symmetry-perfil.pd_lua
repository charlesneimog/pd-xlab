local symmetry = pd.Class:new():register("x.set.symmetry-perfil")
local sieve = require("sieve")

function symmetry:initialize(_, atoms)
	self.inlets = 3
	self.outlets = 1
	self.value = { { 19, 16 }, { 11, 16 } }
	self.range = { tonumber(atoms[1]) or 25, tonumber(atoms[2]) or 500 }
	self.mode = tonumber(atoms[3]) or 1
	return true
end

function symmetry:in_n(inlet, selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if inlet == 2 then
		if value ~= nil then self.range = value end
		return
	elseif inlet == 3 then
		self.mode = type(value) == "table" and value[1] or value
		return
	end
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function()
		return sieve.symmetry_profile(self.value, self.range, self.mode)
	end)
	if result then sieve.output(self, result, "list") end
end

sieve.reload(symmetry)
