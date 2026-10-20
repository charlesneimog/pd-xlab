local union = pd.Class:new():register("x.set.union")
local sieve = require("sieve")

function union:initialize()
	self.inlets = 2
	self.outlets = 1
	self.left = { "sieve", 2, 0, 18 }
	self.right = { "sieve", 2, 0, 18 }
	return true
end

function union:in_n(inlet, selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if inlet == 2 then
		if value ~= nil then self.right = value end
		return
	end
	if value ~= nil then self.left = value end
	local result = sieve.report(self, function() return sieve.union(self.left, self.right) end)
	if result then sieve.output(self, result, "sieve") end
end

sieve.reload(union)
