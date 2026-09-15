local decompose = pd.Class:new():register("x.set.decompose")
local sieve = require("sieve")

function decompose:initialize()
	self.inlets = 1
	self.outlets = 1
	self.value = {}
	return true
end

function decompose:in_1(selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.decompose(self.value) end)
	if result then sieve.output(self, result, "sieve-list") end
end

sieve.reload(decompose)
