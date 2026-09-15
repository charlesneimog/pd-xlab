local dddd = require("dddd")
local rhythm = pd.Class:new():register("x.rhythm")

-- ─────────────────────────────────────
local function report_error(self, message)
	if self.error then
		self:error(message)
	elseif pd.error then
		pd.error(message)
	else
		error(message)
	end
end

-- ─────────────────────────────────────
local function median(values)
	if #values == 0 then
		return nil
	end
	table.sort(values)
	local middle = math.floor(#values / 2) + 1
	if #values % 2 == 1 then
		return values[middle]
	end
	return (values[middle - 1] + values[middle]) * 0.5
end

-- ─────────────────────────────────────
-- Tempo is ambiguous from onsets alone. Preserve the old 60--120 BPM estimate;
-- set bpm explicitly when the intended interpretation contains tuplets.
local function estimate_bpm(onsets)
	local values = {}
	for i = 2, #onsets do
		local interval = onsets[i] - onsets[i - 1]
		if interval >= 80 then
			local value = 60000 / interval
			while value < 60 do
				value = value * 2
			end
			while value > 120 do
				value = value * 0.5
			end
			values[#values + 1] = value
		end
	end
	return median(values)
end

-- ─────────────────────────────────────
local function division_hierarchy(max_division)
	local preferred = { 1, 2, 4, 3, 6, 5, 8, 7, 10, 12, 16, 9, 14, 11, 13, 15 }
	local result, present = {}, {}
	for _, division in ipairs(preferred) do
		if division <= max_division then
			result[#result + 1], present[division] = division, true
		end
	end
	for division = 1, max_division do
		if not present[division] then
			result[#result + 1] = division
		end
	end
	return result
end

-- ─────────────────────────────────────
local function quantize_beat(positions, max_division)
	local best_division, best_ticks, best_max_error, best_total_error
	for _, division in ipairs(division_hierarchy(max_division)) do
		local ticks, seen, valid = {}, {}, true
		local max_error, total_error = 0, 0
		for _, position in ipairs(positions) do
			local scaled = position * division
			local tick = math.floor(scaled + 0.5)
			if tick < 0 or tick >= division or seen[tick] then
				valid = false
				break
			end
			local quantized_error = math.abs(position - (tick / division))
			max_error = math.max(max_error, quantized_error)
			total_error = total_error + quantized_error
			seen[tick], ticks[#ticks + 1] = true, tick
		end
		if valid then
			-- Prefer the grid with the smallest worst onset error.  The total
			-- error breaks ties; division_hierarchy keeps simpler notation when
			-- two grids are equally precise.
			if not best_max_error
				or max_error < best_max_error - 1e-12
				or (math.abs(max_error - best_max_error) <= 1e-12
					and total_error < best_total_error - 1e-12)
			then
				table.sort(ticks)
				best_division, best_ticks = division, ticks
				best_max_error, best_total_error = max_error, total_error
			end
		end
	end
	return best_division, best_ticks, best_max_error
end

-- ─────────────────────────────────────
local function tied_value(value, tied)
	return tied and (tostring(value) .. "_") or value
end

-- ─────────────────────────────────────
local function build_tree(onsets, bpm, numerator, denominator, max_division, tolerance_ms)
	local beat_ms = (60000 / bpm) * (4 / denominator)
	local relative, origin = {}, onsets[1]
	for i, onset in ipairs(onsets) do
		relative[i] = onset - origin
	end

	-- Put an onset on the adjacent beat when that boundary is closer than the
	-- finest point available on the requested grid.
	local boundary_epsilon = 0.5 / max_division
	local beat_positions, boundary_error_ms = {}, 0
	for _, milliseconds in ipairs(relative) do
		local beat_position = milliseconds / beat_ms
		local nearest = math.floor(beat_position + 0.5)
		if math.abs(beat_position - nearest) <= boundary_epsilon then
			local boundary_error = math.abs(beat_position - nearest) * beat_ms
			boundary_error_ms = math.max(boundary_error_ms, boundary_error)
			beat_position = nearest
		end
		beat_positions[#beat_positions + 1] = beat_position
	end

	local last_beat = math.floor(beat_positions[#beat_positions])
	local measure_count = math.floor(last_beat / numerator) + 1
	local total_beats = measure_count * numerator
	local positions_by_beat = {}
	for beat = 0, total_beats - 1 do
		positions_by_beat[beat] = {}
	end
	for _, beat_position in ipairs(beat_positions) do
		local beat = math.floor(beat_position)
		positions_by_beat[beat][#positions_by_beat[beat] + 1] = beat_position - beat
	end

	local beat_data, maximum_error_ms = {}, boundary_error_ms
	for beat = 0, total_beats - 1 do
		local division, ticks, normalized_error = quantize_beat(positions_by_beat[beat], max_division)
		if not division then
			return nil, string.format(
				"onset quantization is limited to %d divisions per beat; increase maxdivision for better precision",
				max_division
			)
		end
		maximum_error_ms = math.max(maximum_error_ms, normalized_error * beat_ms)
		local attacks = {}
		for _, tick in ipairs(ticks) do
			attacks[tick] = true
		end
		beat_data[beat] = { division = division, attacks = attacks, ticks = ticks }
	end

	if maximum_error_ms > tolerance_ms then
		local message = "onset quantization using at most %d divisions per beat differs "
			.. "from the detected onset by %.1f ms; increase maxdivision for better precision"
		return nil, string.format(
			message,
			max_division,
			maximum_error_ms
		)
	end

	local measures = {}
	for measure = 0, measure_count - 1 do
		local entries = {}
		for beat_in_measure = 0, numerator - 1 do
			local beat = measure * numerator + beat_in_measure
			local data, boundaries = beat_data[beat], { 0 }
			for _, tick in ipairs(data.ticks) do
				if tick > 0 then
					boundaries[#boundaries + 1] = tick
				end
			end
			boundaries[#boundaries + 1] = data.division

			local children = {}
			for i = 1, #boundaries - 1 do
				local stop = boundaries[i + 1]
				local continues = stop == data.division
					and beat < total_beats - 1
					and not beat_data[beat + 1].attacks[0]
				children[#children + 1] = tied_value(stop - boundaries[i], continues)
			end

			if data.division == 1 and #children == 1 then
				entries[#entries + 1] = children[1]
			else
				-- A power-of-two child sum is ordinary notation. Other sums make
				-- bhack.voice render the corresponding tuplet.
				entries[#entries + 1] = { 1, children }
			end
		end
		measures[#measures + 1] = { { numerator, denominator }, entries }
	end
	return measures
end

-- ─────────────────────────────────────
function rhythm:initialize(_, atoms)
	atoms = atoms or {}
	self.inlets, self.outlets = 1, 2
	self.bpm = tonumber(atoms[1]) or 0
	self.numerator = math.floor(tonumber(atoms[2]) or 4)
	self.denominator = math.floor(tonumber(atoms[3]) or 4)
	self.max_division = math.floor(tonumber(atoms[4]) or 8)
	self.tolerance_ms = tonumber(atoms[5]) or 50
	return self.bpm >= 0
		and self.numerator > 0
		and self.denominator > 0
		and self.max_division > 0
		and self.tolerance_ms >= 0
end

-- ─────────────────────────────────────
function rhythm:in_1_bpm(atoms)
	local value = tonumber(atoms and atoms[1])
	if not value or value < 0 then
		return report_error(self, "[x.rhythm] bpm expects a non-negative number")
	end
	self.bpm = value
end

-- ─────────────────────────────────────
function rhythm:in_1_timesig(atoms)
	local numerator = math.floor(tonumber(atoms and atoms[1]) or 0)
	local denominator = math.floor(tonumber(atoms and atoms[2]) or 0)
	if numerator < 1 or denominator < 1 then
		return report_error(self, "[x.rhythm] timesig expects a positive numerator and denominator")
	end
	self.numerator, self.denominator = numerator, denominator
end
rhythm.in_1_meter = rhythm.in_1_timesig

-- ─────────────────────────────────────
function rhythm:in_1_maxdivision(atoms)
	local value = math.floor(tonumber(atoms and atoms[1]) or 0)
	if value < 1 then
		return report_error(self, "[x.rhythm] maxdivision expects a positive integer")
	end
	self.max_division = value
end

-- ─────────────────────────────────────
function rhythm:in_1_tolerance(atoms)
	local value = tonumber(atoms and atoms[1])
	if not value or value < 0 then
		return report_error(self, "[x.rhythm] tolerance expects a non-negative number of milliseconds")
	end
	self.tolerance_ms = value
end

-- ─────────────────────────────────────
function rhythm:in_1_dddd(atoms)
	local l = dddd:new_from_id(self, atoms[1])
	local atom_l = l:get_table()
	self:in_1_list(atom_l)
end

-- ─────────────────────────────────────
function rhythm:in_1_list(atoms)
	if #atoms < 2 then
		return report_error(self, "[x.rhythm] need at least two onset times")
	end
	local onsets = {}
	for i, atom in ipairs(atoms) do
		local onset = tonumber(atom)
		if not onset then
			return report_error(self, string.format("[x.rhythm] onset %d is not numeric", i))
		end
		if i > 1 and onset <= onsets[i - 1] then
			return report_error(self, "[x.rhythm] onset times must be strictly increasing")
		end
		onsets[i] = onset
	end

	local bpm = self.bpm ~= 0 and self.bpm or estimate_bpm(onsets)
	if not bpm or bpm <= 0 then
		return report_error(self, "[x.rhythm] could not determine the tempo")
	end

	local tree, message = build_tree(
		onsets,
		bpm,
		self.numerator,
		self.denominator,
		self.max_division,
		self.tolerance_ms
	)

	if not tree then
		return report_error(self, "[x.rhythm] " .. message)
	end

	self:outlet(2, "float", { bpm })
	dddd:new_from_table(self, tree):output(1)
end
