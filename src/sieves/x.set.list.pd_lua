local list = pd.Class:new():register("x.set.list")
local sieve = require("sieve")

function list:initialize()
	self.inlets = 1
	self.outlets = 1
	self.value = {}
	return true
end

function list:in_1(selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.list(self.value) end)
	if result then sieve.output(self, result, "sieve-list") end
end

sieve.reload(list)
