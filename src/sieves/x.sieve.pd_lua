local reveal = pd.Class:new():register("x.sieve")
local sieve = require("sieve")

function reveal:initialize()
	self.inlets = 1
	self.outlets = 1
	self.value = { "sieve", 2, 0, 18 }
	return true
end

function reveal:in_1(selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.reveal(self.value) end)
	if result then sieve.output(self, result, "list") end
end

sieve.reload(reveal)
