local complement = pd.Class:new():register("x.set.complement")
local sieve = require("sieve")

function complement:initialize()
	self.inlets = 1
	self.outlets = 1
	self.value = { "sieve", 2, 0, 18 }
	return true
end

function complement:in_1(selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.complement(self.value) end)
	if result then sieve.output(self, result, "sieve") end
end

sieve.reload(complement)
