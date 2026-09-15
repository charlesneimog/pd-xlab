local union_list = pd.Class:new():register("x.set.union-l")
local sieve = require("sieve")

function union_list:initialize()
	self.inlets = 1
	self.outlets = 1
	self.value = {}
	return true
end

function union_list:in_1(selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.union_list(self.value) end)
	if result then sieve.output(self, result, "sieve") end
end

sieve.reload(union_list)
