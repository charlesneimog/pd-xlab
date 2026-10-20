local ariza = pd.Class:new():register("x.set.ariza")
local sieve = require("sieve")

function ariza:initialize(_, atoms)
	self.inlets = 2
	self.outlets = 1
	self.limit = tonumber(atoms[1]) or 225
	self.value = nil
	return true
end

function ariza:in_n(inlet, selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if inlet == 2 then
		self.limit = tonumber(type(value) == "table" and value[1] or value) or self.limit
		return
	end
	if value ~= nil then self.value = value end
	if self.value == nil then return end
	local result = sieve.report(self, function() return sieve.ariza(self.value, self.limit) end)
	if result then sieve.output(self, result, "sieve") end
end

sieve.reload(ariza)
