local dddd = require("dddd")

local M = {}
local unpack = table.unpack or unpack

local function copy(value, seen)
	if type(value) ~= "table" then return value end
	seen = seen or {}
	if seen[value] then return seen[value] end
	local result = {}
	seen[value] = result
	for key, item in pairs(value) do result[copy(key, seen)] = copy(item, seen) end
	return result
end

local function gcd(a, b)
	a, b = math.abs(a), math.abs(b)
	while b ~= 0 do a, b = b, a % b end
	return a
end

local function rat_new(numerator, denominator)
	if denominator == 0 then error("ratio denominator cannot be zero") end
	if denominator < 0 then numerator, denominator = -numerator, -denominator end
	local divisor = gcd(numerator, denominator)
	return { _ratio = true, n = numerator / divisor, d = denominator / divisor }
end

local function approximate_ratio(value)
	if value == math.floor(value) then return rat_new(value, 1) end
	local sign = value < 0 and -1 or 1
	value = math.abs(value)
	local denominator = 1000000
	return rat_new(sign * math.floor(value * denominator + 0.5), denominator)
end

local function rat(value)
	if type(value) == "table" and value._ratio then return value end
	if type(value) == "table" and value[1] == "ratio" then
		return rat_new(tonumber(value[2]), tonumber(value[3]))
	end
	if type(value) == "string" then
		local numerator, denominator = value:match("^%s*([+-]?%d+)%s*/%s*([+-]?%d+)%s*$")
		if numerator then return rat_new(tonumber(numerator), tonumber(denominator)) end
		value = tonumber(value)
	end
	if type(value) ~= "number" then error("expected a number or ratio") end
	return approximate_ratio(value)
end

local function radd(a, b) a, b = rat(a), rat(b); return rat_new(a.n * b.d + b.n * a.d, a.d * b.d) end
local function rmul(a, b) a, b = rat(a), rat(b); return rat_new(a.n * b.n, a.d * b.d) end
local function rdiv(a, b) a, b = rat(a), rat(b); return rat_new(a.n * b.d, a.d * b.n) end
local function rinv(a) a = rat(a); return rat_new(a.d, a.n) end
local function rnum(a) a = rat(a); return a.n / a.d end

local function rpow(value, exponent)
	value = rat(value)
	exponent = tonumber(exponent)
	if exponent ~= math.floor(exponent) then return approximate_ratio(rnum(value) ^ exponent) end
	if exponent < 0 then value, exponent = rinv(value), -exponent end
	return rat_new(value.n ^ exponent, value.d ^ exponent)
end

local function public(value, seen)
	if type(value) ~= "table" then return value end
	if value._ratio then
		if value.d == 1 then return value.n end
		return string.format("%d/%d", value.n, value.d)
	end
	seen = seen or {}
	if seen[value] then return seen[value] end
	local result = {}
	seen[value] = result
	for key, item in pairs(value) do result[key] = public(item, seen) end
	return result
end

local function integer(value, label)
	local number = tonumber(value)
	if not number or number ~= math.floor(number) then error((label or "value") .. " must be an integer") end
	return number
end

local function scalar(value)
	if type(value) == "table" and #value == 1 then return value[1] end
	return value
end

local function recursively(value, fn)
	if type(value) ~= "table" then return fn(value) end
	local result = {}
	for index, item in ipairs(value) do result[index] = recursively(item, fn) end
	return result
end

local function contains(values, wanted)
	for _, value in ipairs(values) do if value == wanted then return true end end
	return false
end

local function combinations(values, count, start, prefix, result)
	result, prefix, start = result or {}, prefix or {}, start or 1
	if #prefix == count then result[#result + 1] = copy(prefix); return result end
	for index = start, #values - (count - #prefix) + 1 do
		prefix[#prefix + 1] = values[index]
		combinations(values, count, index + 1, prefix, result)
		prefix[#prefix] = nil
	end
	return result
end

local function difference(left, right)
	local result = {}
	for _, value in ipairs(left) do if not contains(right, value) then result[#result + 1] = value end end
	return result
end

local function product(values)
	local result = rat_new(1, 1)
	for _, value in ipairs(values) do result = rmul(result, value) end
	return result
end

local function octave_reduce_one(value, range)
	value, range = rat(value), rat(range)
	local numeric, base = rnum(value), rnum(range)
	if numeric <= 0 or base <= 0 or base == 1 then error("ratios and reduction range must be positive") end
	local exponent = math.floor(math.log(numeric) / math.log(base))
	return rdiv(value, rpow(range, exponent))
end

local function sorted_ratios(values)
	local result = copy(values)
	table.sort(result, function(a, b) return rnum(a) < rnum(b) end)
	return result
end

local function unique_numbers(values, epsilon)
	local result = {}
	for _, value in ipairs(values) do
		local found = false
		for _, other in ipairs(result) do if math.abs(value - other) <= (epsilon or 1e-9) then found = true break end end
		if not found then result[#result + 1] = value end
	end
	return result
end

local function adjacent_ratios(values)
	local sorted = sorted_ratios(values)
	local result = {}
	for index = 1, #sorted - 1 do result[#result + 1] = rdiv(sorted[index], sorted[index + 1]) end
	return result
end

local function mos_values(generator, stacking, range)
	stacking = integer(stacking, "stacking")
	local result = { rat_new(1, 1) }
	for exponent = 1, stacking do result[#result + 1] = octave_reduce_one(rpow(generator, exponent), range) end
	result[#result + 1] = rat(range)
	return result
end

local function mos_verify(values)
	local intervals = adjacent_ratios(values)
	local numeric = {}
	for _, value in ipairs(intervals) do numeric[#numeric + 1] = rnum(value) end
	local unique = unique_numbers(numeric)
	if #unique ~= 2 then return "This is not a x.ji.mos", intervals end
	table.sort(unique)
	local labels = {}
	for _, value in ipairs(numeric) do labels[#labels + 1] = math.abs(value - unique[2]) < 1e-9 and "s" or "L" end
	return labels, intervals
end

local function nearest_step(value, divisions)
	divisions = tonumber(divisions)
	if not divisions or divisions <= 0 then error("temperament divisions must be positive") end
	local step = 1 / divisions
	if value >= 0 then return math.floor(value / step + 0.5) * step end
	return math.ceil(value / step - 0.5) * step
end

local function nearest(value, tuning, minimum, maximum)
	local best, distance
	for _, candidate in ipairs(tuning) do
		local current = math.abs(value - candidate)
		if (minimum == nil or current >= minimum) and (maximum == nil or current <= maximum)
			and (distance == nil or current < distance) then
			best, distance = candidate, current
		end
	end
	return best
end

local function prime_factors(value)
	value = integer(value, "harmonic")
	if value < 1 then error("harmonics must be positive") end
	local result, divisor = {}, 2
	while divisor * divisor <= value do
		while value % divisor == 0 do result[#result + 1] = divisor; value = value / divisor end
		divisor = divisor + 1
	end
	if value > 1 then result[#result + 1] = value end
	return result
end

local function cps_ratios(sets)
	if type(sets[1]) ~= "table" then sets = { sets } end
	local result = {}
	for _, set in ipairs(sets) do result[#result + 1] = octave_reduce_one(product(set), 2) end
	return result
end

local functions = {}

functions["x.ji.rttom"] = function(ratios, fundamental)
	fundamental = tonumber(scalar(fundamental)) or 60
	return recursively(ratios, function(value) return fundamental + 12 * math.log(rnum(value), 2) end)
end

functions["x.ji.range-reduce"] = function(notes, low, high)
	low, high = tonumber(scalar(low)), tonumber(scalar(high))
	if not low or not high or high - low < 12 then error("range must span at least 12 semitones") end
	return recursively(notes, function(note)
		while note < low do note = note + 12 end
		while note > high do note = note - 12 end
		return note
	end)
end

functions["x.ji.filter-ac-inst"] = function(notes, tolerance, temperament)
	local result = {}
	for _, note in ipairs(notes) do
		if math.abs(nearest_step(note, scalar(temperament)) - note) <= tonumber(scalar(tolerance)) then
			result[#result + 1] = note
		end
	end
	return result
end

functions["x.ji.modulation-notes"] = function(left, right, tolerance)
	tolerance = tonumber(scalar(tolerance))
	local result = {}
	for _, a in ipairs(left) do for _, b in ipairs(right) do
		local delta = ((a - b + 6) % 12) - 6
		if math.abs(delta) <= tolerance then result[#result + 1] = { a, b } end
	end end
	return result
end

functions["x.ji.modulation-notes-fund"] = function(left, right, tolerance, temperament)
	tolerance = tonumber(scalar(tolerance))
	local records, shifts = {}, {}
	for _, a in ipairs(left) do for _, b in ipairs(right) do
		local delta = a - b
		local shift = nearest_step(delta, scalar(temperament))
		if math.abs(delta - shift) <= tolerance then
			records[#records + 1] = { a, b, shift }
			if math.abs(shift) > tolerance and not contains(shifts, shift) then shifts[#shifts + 1] = shift end
		end
	end end
	table.sort(shifts)
	return { records, shifts }
end

functions["x.ji.rt-octave"] = function(values, range)
	return recursively(values, function(value) return octave_reduce_one(value, scalar(range)) end)
end

functions["x.ji.change-notes"] = function(notes, tuning)
	if type(notes) ~= "table" then return nearest(notes, tuning) end
	local result = {}
	for _, note in ipairs(notes) do result[#result + 1] = nearest(note, tuning) end
	return result
end

functions["x.ji.range-change-notes"] = function(notes, tuning, range)
	local result = {}
	for _, note in ipairs(notes) do
		result[#result + 1] = nearest(note, tuning, tonumber(range[1]), tonumber(range[2])) or "nil"
	end
	return result
end

functions["x.ji.diamond"] = function(limit)
	limit = integer(scalar(limit), "limit")
	local identities = {}
	for value = 1, limit, 2 do identities[#identities + 1] = value end
	table.sort(identities, function(a, b) return rnum(octave_reduce_one(a, 2)) < rnum(octave_reduce_one(b, 2)) end)
	local utonal, otonal = {}, {}
	for _, x in ipairs(identities) do
		local urow, orow = {}, {}
		for _, y in ipairs(identities) do urow[#urow + 1] = rdiv(x, y); orow[#orow + 1] = rdiv(y, x) end
		utonal[#utonal + 1], otonal[#otonal + 1] = urow, orow
	end
	return { utonal, otonal }
end

functions["x.ji.diamond-identity"] = function(identities)
	local utonal, otonal = {}, {}
	for _, x in ipairs(identities) do
		local urow, orow = {}, {}
		for _, y in ipairs(identities) do urow[#urow + 1] = rdiv(x, y); orow[#orow + 1] = rdiv(y, x) end
		utonal[#utonal + 1], otonal[#otonal + 1] = urow, orow
	end
	return { utonal, otonal }
end

functions["x.ji.chord-inverse"] = function(chord) return recursively(chord, rinv) end
functions["x.ji.cpstoidentity"] = function(sets)
	if type(sets[1]) ~= "table" then return { product(sets) } end
	local result = {}
	for _, set in ipairs(sets) do result[#result + 1] = product(set) end
	return result
end
functions["x.ji.cpstoratio"] = cps_ratios

functions["x.ji.mos"] = mos_values
functions["x.ji.mos-verify"] = function(values)
	local labels, intervals = mos_verify(values)
	return { labels, intervals }
end
functions["x.ji.mos-check"] = function(generator, maximum, range, interval_count)
	local result = {}
	for stacking = 1, integer(maximum, "maximum stacking") do
		local _, intervals = mos_verify(mos_values(generator, stacking, range))
		local numeric = {}; for _, item in ipairs(intervals) do numeric[#numeric + 1] = rnum(item) end
		if #unique_numbers(numeric) == integer(interval_count or 2) then result[#result + 1] = stacking end
	end
	return result
end
functions["x.ji.mos-complementary"] = function(generator, range, maximum)
	local result = {}
	for stacking = 2, integer(maximum, "maximum stacking") do
		local left = select(1, mos_verify(mos_values(generator, stacking, range)))
		local right = select(1, mos_verify(mos_values(rdiv(range, generator), stacking, range)))
		if type(left) == "table" and type(right) == "table" and #left == #right then
			local match = true
			for index = 1, #left do if left[index] ~= right[#right - index + 1] then match = false break end end
			if match then result[#result + 1] = stacking end
		end
	end
	return result
end

functions["x.ji.hexany"] = function(values)
	if #values ~= 4 then error("x.ji.hexany expects exactly four identities") end
	return combinations(values, 2)
end

functions["x.ji.hexany-triads"] = function(values)
	if #values ~= 4 then error("x.ji.hexany-triads expects exactly four identities") end
	local subharmonic, harmonic = {}, {}
	for _, excluded in ipairs(values) do
		for _, pair in ipairs(combinations(difference(values, { excluded }), 2)) do
			subharmonic[#subharmonic + 1] = octave_reduce_one(product(pair), 2)
		end
		local row = {}
		for _, other in ipairs(difference(values, { excluded })) do
			row[#row + 1] = octave_reduce_one(rmul(excluded, other), 2)
		end
		harmonic[#harmonic + 1] = row
	end
	return { subharmonic, harmonic }
end

functions["x.ji.hexany-connections"] = function(vertex, hexany)
	local result = {}
	for _, edge in ipairs(hexany) do
		if contains(edge, vertex[1]) or contains(edge, vertex[2]) then result[#result + 1] = edge end
	end
	return result
end

functions["x.ji.eikosany"] = function(values)
	if #values ~= 6 then error("x.ji.eikosany expects exactly six identities") end
	return combinations(values, 3)
end

functions["x.ji.eikosany-triads"] = function(values)
	if #values ~= 6 then error("x.ji.eikosany-triads expects exactly six identities") end
	local subharmonic, harmonic = {}, {}
	for _, vertex in ipairs(combinations(values, 3)) do
		local complement = difference(values, vertex)
		local vertex_pairs = cps_ratios(combinations(vertex, 2))
		for _, identity in ipairs(complement) do
			local row = {}
			for _, ratio in ipairs(vertex_pairs) do
				row[#row + 1] = octave_reduce_one(rmul(ratio, identity), 2)
			end
			subharmonic[#subharmonic + 1] = row
		end
		local complement_pairs = cps_ratios(combinations(complement, 2))
		for _, ratio in ipairs(complement_pairs) do
			local row = {}
			for _, identity in ipairs(vertex) do
				row[#row + 1] = octave_reduce_one(rmul(identity, ratio), 2)
			end
			harmonic[#harmonic + 1] = row
		end
	end
	return { subharmonic, harmonic }
end

functions["x.ji.eikosany-tetrads"] = function(values)
	if #values ~= 6 then error("x.ji.eikosany-tetrads expects exactly six identities") end
	local subharmonic, harmonic = {}, {}
	for _, tetrad in ipairs(combinations(values, 4)) do
		subharmonic[#subharmonic + 1] = cps_ratios(combinations(tetrad, 3))
		local complement_product = product(difference(values, tetrad))
		local row = {}
		for _, identity in ipairs(tetrad) do
			row[#row + 1] = octave_reduce_one(rmul(identity, complement_product), 2)
		end
		harmonic[#harmonic + 1] = row
	end
	return { subharmonic, harmonic }
end

functions["x.ji.eikosany-connections"] = function(vertex, eikosany)
	local result = {}
	for _, item in ipairs(eikosany) do
		local shared = 0; for _, value in ipairs(vertex) do if contains(item, value) then shared = shared + 1 end end
		if shared >= 2 then result[#result + 1] = item end
	end
	return result
end

functions["x.ji.interval-sob"] = function(interval, exponents)
	local utonal, otonal = {}, {}
	for _, exponent in ipairs(exponents) do
		utonal[#utonal + 1] = rpow(rinv(interval), exponent)
		otonal[#otonal + 1] = rpow(interval, exponent)
	end
	return { utonal, otonal }
end
functions["x.ji.arith-mean"] = function(low, high) return rdiv(radd(low, high), 2) end
functions["x.ji.arith-mean-sob"] = function(low, high)
	local mean = rdiv(radd(low, high), 2)
	return { rat(low), rdiv(high, mean), rmul(low, mean), rat(high) }
end
functions["x.ji.johnston-sob"] = function(interval, stacking, fundamental)
	stacking, fundamental = integer(stacking, "stacking"), tonumber(fundamental)
	local semitones = 12 * math.log(rnum(interval), 2)
	local result = {}; for exponent = 1, stacking do result[#result + 1] = fundamental - semitones * exponent end
	result[#result + 1] = fundamental
	for exponent = 1, stacking do result[#result + 1] = fundamental + semitones * exponent end
	return result
end

functions["ji.choose"] = function(values, positions)
	if type(positions) ~= "table" then return values[integer(positions, "position")] end
	local result = {}
	for _, position in ipairs(positions) do result[#result + 1] = values[integer(position, "position")] end
	return result
end
functions["x.ji.prime-decomposition"] = function(values)
	if type(values) ~= "table" then values = { values } end
	local all, odd = {}, {}
	for _, value in ipairs(values) do
		local factors = prime_factors(value)
		all[#all + 1] = factors
		local reduced = {}
		for _, factor in ipairs(factors) do
			if factor ~= 2 then reduced[#reduced + 1] = factor end
		end
		if #reduced == 0 then reduced[1] = 1 end
		odd[#odd + 1] = reduced
	end
	return { all, odd }
end
functions["x.ji.mk-temperament"] = function(fundamental, interval, divisions)
	fundamental, divisions = tonumber(fundamental), integer(divisions, "divisions")
	local step = 12 * math.log(rnum(interval), 2) / divisions
	local result = { fundamental }
	for index = 1, divisions do result[#result + 1] = fundamental + step * index end
	return result
end

local function voice_events(voice)
	if type(voice) ~= "table" or voice[1] ~= "voice" then
		error("voice expects (voice pitches velocities channels durations [onsets])")
	end
	local pitches, velocities = voice[2] or {}, voice[3] or {}
	local channels, durations, onsets = voice[4] or {}, voice[5] or {}, voice[6]
	local events, onset = {}, 0
	for index, pitch in ipairs(pitches) do
		local duration = durations[index] or durations[1] or 0
		events[#events + 1] = {
			onsets and onsets[index] or onset,
			pitch,
			velocities[index] or velocities[1] or 100,
			channels[index] or channels[1] or 1,
			duration,
		}
		onset = onset + math.abs(duration)
	end
	return events
end
functions["ji.voicetotext"] = function(voice)
	local result = {}
	for _, event in ipairs(voice_events(voice)) do
		result[#result + 1] = { event[1], ",", event[2], event[3], event[4], event[5], ";" }
	end
	return result
end
functions["ji.play-om#"] = function(voice) return { "play", voice_events(voice) } end

local specs = {
	["x.ji.rttom"] = { { "1/1", "11/8", "7/4" }, 60 },
	["x.ji.range-reduce"] = { { 48, 72, 60 }, 60, 79.02 },
	["x.ji.filter-ac-inst"] = { { 60, 65.30, 72.03, 50.49 }, 0.10, 2 },
	["x.ji.modulation-notes"] = { { 60, 65.30 }, { 72.03, 50.49 }, 0.02 },
	["x.ji.modulation-notes-fund"] = { defaults = { { 60, 65.30 }, { 72.03, 50.49 }, 0.10, 4 }, outlets = 2 },
	["x.ji.rt-octave"] = { { "1/3", 1, "5/3" }, 2 },
	["x.ji.change-notes"] = { { 60, 61, 62 }, { 60, 64.98, 69.96, 62.94, 67.92 } },
	["x.ji.range-change-notes"] = { { 60, 61, 62 }, { 60, 64.98, 69.96, 62.94, 67.92 }, { 0.30, 5 } },
	["x.ji.diamond"] = { defaults = { 11 }, outlets = 2 },
	["x.ji.diamond-identity"] = { defaults = { { 11, 19, 97 } }, outlets = 2 },
	["x.ji.chord-inverse"] = { { "1/1", "3/2", "5/4" } },
	["x.ji.cpstoidentity"] = { { { 1, 3 }, { 1, 5 }, { 3, 5 } } },
	["x.ji.cpstoratio"] = { { 1, 3, 5, 7 } },
	["x.ji.mos"] = { "4/3", 11, 2 },
	["x.ji.mos-verify"] = { defaults = { { 1, "4/3", "16/9", 2 } }, outlets = 2 },
	["x.ji.mos-check"] = { "4/3", 60, 2, 2 },
	["x.ji.mos-complementary"] = { "3/2", 4, 50 },
	["x.ji.hexany"] = { { 5, 7, 13, 17 } },
	["x.ji.hexany-triads"] = { defaults = { { 1, 3, 5, 7 } }, outlets = 2 },
	["x.ji.hexany-connections"] = { { 3, 13 }, { { 3, 5 }, { 3, 13 }, { 5, 13 }, { 3, 21 }, { 5, 21 }, { 13, 21 } } },
	["x.ji.eikosany"] = { { 1, 3, 5, 7, 9, 11 } },
	["x.ji.eikosany-triads"] = { defaults = { { 1, 3, 5, 7, 9, 11 } }, outlets = 2 },
	["x.ji.eikosany-tetrads"] = { defaults = { { 1, 3, 5, 7, 9, 11 } }, outlets = 2 },
	["x.ji.eikosany-connections"] = { { 1, 3, 9 }, combinations({ 1, 3, 5, 7, 9, 11 }, 3) },
	["x.ji.interval-sob"] = { defaults = { "11/8", { 2, 3, 7, 11, 12 } }, outlets = 2 },
	["x.ji.arith-mean"] = { "1/1", "2/1" },
	["x.ji.arith-mean-sob"] = { "1/1", "5/4" },
	["x.ji.johnston-sob"] = { "3/2", 3, 72 },
	["ji.choose"] = { { 1, 2, 3, 4, 5 }, 2 },
	["x.ji.prime-decomposition"] = { defaults = { { 9, 18, 172 } }, outlets = 2 },
	["x.ji.mk-temperament"] = { 60, 2, 24 },
	["ji.play-om#"] = { { "voice", {}, {}, {}, {} } },
	["ji.voicetotext"] = { { "voice", {}, {}, {}, {} } },
}

local function input(pdobj, selector, atoms)
	if selector == "dddd" then return dddd:new_from_atoms(pdobj, atoms):get_table() end
	if selector == "bang" then return nil end
	if selector == "float" or selector == "symbol" then return atoms[1] end
	if selector == "list" then return dddd:new(pdobj, atoms):get_table() or copy(atoms) end
	local message = { selector }; for _, atom in ipairs(atoms or {}) do message[#message + 1] = atom end
	return dddd:new(pdobj, message):get_table() or message
end

local function output(pdobj, outlet, value)
	dddd:new_from_table(pdobj, public(value)):output(outlet)
end

function M.attach(class, name)
	local spec = assert(specs[name], "missing OM-JI specification for " .. name)
	local defaults = spec.defaults or spec
	local outlets = spec.outlets or 1
	function class:initialize(_, atoms)
		self.inlets, self.outlets, self.values = #defaults, outlets, copy(defaults)
		for index, atom in ipairs(atoms or {}) do if index <= #self.values then self.values[index] = atom end end
		return true
	end
	function class:in_n(inlet, selector, atoms)
		local value = input(self, selector, atoms)
		if value ~= nil then self.values[inlet] = value end
		if inlet ~= 1 then return end
		local ok, result = pcall(function() return functions[name](unpack(self.values)) end)
		if not ok then self:error("[" .. name .. "] " .. tostring(result)); return end
		if outlets == 1 then output(self, 1, result) else
			for outlet = outlets, 1, -1 do output(self, outlet, result[outlet]) end
		end
	end
	function class:in_1_reload() self:dofilex(self._scriptname) end
end

M.functions = functions
M.public = public

return M
