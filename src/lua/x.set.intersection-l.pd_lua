local intersection_list = pd.Class:new():register("x.set.intersection-l")
local sieve = require("sieve")

function intersection_list:initialize()
	self.inlets = 1
	self.outlets = 1
	self.value = {}
	return true
end

function intersection_list:in_1(selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.intersection_list(self.value) end)
	if result then sieve.output(self, result, "sieve") end
end

sieve.reload(intersection_list)
