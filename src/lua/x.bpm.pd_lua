local bpm = pd.Class:new():register("x.bpm")

function bpm:initialize(sel, atoms)
	self.inlets = 1
	self.outlets = 1
	self.min_ioi = tonumber(atoms[1]) or 80
	self.min_bpm = tonumber(atoms[2]) or 60
	self.max_bpm = tonumber(atoms[3]) or 120
	return true
end

local function median(values)
	if #values == 0 then
		return nil
	end
	table.sort(values)
	local n = #values
	if n % 2 == 1 then
		return values[math.floor(n / 2) + 1]
	end
	return (values[n / 2] + values[n / 2 + 1]) * 0.5
end

function bpm:in_1_list(atoms)
	if #atoms < 2 then
		pd.error("[bpm] need at least 2 onset times")
		return
	end
	local bpms = {}
	for i = 2, #atoms do
		local previous = tonumber(atoms[i - 1])
		local current = tonumber(atoms[i])
		if previous and current then
			local ioi = current - previous
			if ioi >= self.min_ioi then
				local value = 60000.0 / ioi
				while value < self.min_bpm do
					value = value * 2.0
				end
				while value > self.max_bpm do
					value = value * 0.5
				end
				table.insert(bpms, value)
			end
		end
	end
	local result = median(bpms)
	if result then
		self:outlet(1, "float", { result })
	end
end
