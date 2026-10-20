local limite = pd.Class:new():register("x.set.limite")
local sieve = require("sieve")

function limite:initialize(_, atoms)
	self.inlets = 3
	self.outlets = 1
	self.value = { { 19, 16 }, { 11, 16 } }
	self.limit = tonumber(atoms[1]) or 225
	self.mode = tonumber(atoms[2]) or 1
	return true
end

function limite:in_n(inlet, selector, atoms)
	local value = sieve.input(self, selector, atoms)
	if inlet == 2 then
		self.limit = tonumber(type(value) == "table" and value[1] or value) or self.limit
		return
	elseif inlet == 3 then
		self.mode = type(value) == "table" and value[1] or value
		return
	end
	if value ~= nil then self.value = value end
	local result = sieve.report(self, function() return sieve.limit(self.value, self.limit, self.mode) end)
	if result then sieve.output(self, result, "sieve") end
end

sieve.reload(limite)
