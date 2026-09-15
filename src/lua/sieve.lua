local dddd = require("dddd")

local M = {}

local unpack = table.unpack or unpack

local function copy(value, seen)
	if type(value) ~= "table" then
		return value
	end
	seen = seen or {}
	if seen[value] then
		return seen[value]
	end
	local result = {}
	seen[value] = result
	for key, item in pairs(value) do
		result[copy(key, seen)] = copy(item, seen)
	end
	return result
end

local function integer(value, label)
	local number = tonumber(value)
	if not number or number ~= math.floor(number) then
		error((label or "value") .. " must be an integer")
	end
	return number
end

local function elementary(value)
	return type(value) == "table"
		and #value == 3
		and tonumber(value[1]) ~= nil
		and tonumber(value[2]) ~= nil
		and tonumber(value[3]) ~= nil
end

local function tagged(value, tag)
	return type(value) == "table" and value[1] == tag
end

local function append(result, value)
	result[#result + 1] = value
	return result
end

local function sorted_unique(values)
	local present = {}
	local result = {}
	for _, value in ipairs(values) do
		value = integer(value, "sieve member")
		if not present[value] then
			present[value] = true
			result[#result + 1] = value
		end
	end
	table.sort(result)
	return result
end

local function sequence(modulus, offset, limit)
	modulus = integer(modulus, "modulus")
	offset = integer(offset, "offset")
	limit = integer(limit, "limit")
	if modulus <= 0 then
		error("modulus must be greater than zero")
	end
	local result = {}
	if offset > limit then
		return result
	end
	for value = offset, limit, modulus do
		result[#result + 1] = value
	end
	return result
end

local function set_union(left, right)
	local values = {}
	for _, value in ipairs(left) do values[value] = true end
	for _, value in ipairs(right) do values[value] = true end
	local result = {}
	for value in pairs(values) do result[#result + 1] = value end
	table.sort(result)
	return result
end

local function set_intersection(left, right)
	local values = {}
	for _, value in ipairs(left) do values[value] = true end
	local result = {}
	for _, value in ipairs(right) do
		if values[value] then result[#result + 1] = value end
	end
	return sorted_unique(result)
end

local function expression_limit(expression)
	if tagged(expression, "sieve") then
		return integer(expression[4], "limit")
	elseif elementary(expression) then
		return integer(expression[3], "limit")
	elseif tagged(expression, "sieve-c") then
		return expression_limit(expression[2])
	elseif tagged(expression, "sieve-u") or tagged(expression, "sieve-i") then
		local maximum
		for index = 2, #expression do
			local value = expression_limit(expression[index])
			maximum = maximum and math.max(maximum, value) or value
		end
		if maximum == nil then error("empty sieve operation") end
		return maximum
	end
	error("invalid sieve expression")
end

function M.is_expression(value)
	return elementary(value)
		or tagged(value, "sieve")
		or tagged(value, "sieve-u")
		or tagged(value, "sieve-i")
		or tagged(value, "sieve-c")
end

function M.normalize(value)
	if elementary(value) then
		return { "sieve", integer(value[1]), integer(value[2]), integer(value[3]) }
	end
	if tagged(value, "sieve") then
		if #value ~= 4 then error("sieve expects modulus, offset, and limit") end
		local modulus = integer(value[2], "modulus")
		if modulus <= 0 then error("modulus must be greater than zero") end
		return { "sieve", modulus, integer(value[3], "offset"), integer(value[4], "limit") }
	end
	if tagged(value, "sieve-c") then
		if #value ~= 2 then error("sieve-c expects one sieve") end
		return { "sieve-c", M.normalize(value[2]) }
	end
	if tagged(value, "sieve-u") or tagged(value, "sieve-i") then
		if #value < 3 then error(value[1] .. " expects at least two sieves") end
		local result = { value[1] }
		for index = 2, #value do append(result, M.normalize(value[index])) end
		return result
	end
	error("expected a sieve expression")
end

function M.union(...)
	local args = { ... }
	if #args < 2 then error("x.set.union expects at least two sieves") end
	local result = { "sieve-u" }
	for _, value in ipairs(args) do append(result, M.normalize(value)) end
	return result
end

function M.intersection(...)
	local args = { ... }
	if #args < 2 then error("x.set.intersection expects at least two sieves") end
	local result = { "sieve-i" }
	for _, value in ipairs(args) do append(result, M.normalize(value)) end
	return result
end

function M.complement(value)
	return { "sieve-c", M.normalize(value) }
end

function M.reveal(value)
	local expression = M.normalize(value)
	local operation = expression[1]
	if operation == "sieve" then
		return sequence(expression[2], expression[3], expression[4])
	elseif operation == "sieve-u" then
		local result = {}
		for index = 2, #expression do
			result = set_union(result, M.reveal(expression[index]))
		end
		return result
	elseif operation == "sieve-i" then
		local result = M.reveal(expression[2])
		for index = 3, #expression do
			result = set_intersection(result, M.reveal(expression[index]))
		end
		return result
	elseif operation == "sieve-c" then
		local source = M.reveal(expression[2])
		if #source == 0 then return {} end
		local present = {}
		for _, item in ipairs(source) do present[item] = true end
		local result = {}
		for item = source[1], expression_limit(expression[2]) do
			if not present[item] then result[#result + 1] = item end
		end
		return result
	end
	error("unknown sieve operation " .. tostring(operation))
end

function M.list(values)
	if type(values) ~= "table" then error("x.set.list expects a list") end
	if M.is_expression(values) then return { M.normalize(values) } end
	local result = {}
	for _, value in ipairs(values) do append(result, M.normalize(value)) end
	return result
end

function M.union_list(values)
	local expressions = M.list(values)
	if #expressions == 0 then error("x.set.union-l expects a non-empty list") end
	if #expressions == 1 then return expressions[1] end
	return M.union(unpack(expressions))
end

function M.intersection_list(values)
	local expressions = M.list(values)
	if #expressions == 0 then error("x.set.intersection-l expects a non-empty list") end
	if #expressions == 1 then return expressions[1] end
	return M.intersection(unpack(expressions))
end

local function token_expression(token, limit)
	if type(token) ~= "string" then return nil end
	local modulus, offset = token:match("^([+-]?%d+)@([+-]?%d+)$")
	if not modulus then return nil end
	return M.normalize({ tonumber(modulus), tonumber(offset), limit })
end

local function ariza_group(group, limit)
	if type(group) == "string" then
		local expression = token_expression(group, limit)
		if not expression then error("invalid Ariza token " .. group) end
		return expression
	end
	if M.is_expression(group) then return M.normalize(group) end
	if type(group) ~= "table" or #group == 0 then error("invalid Ariza expression") end
	if #group == 1 then return ariza_group(group[1], limit) end
	local result = ariza_group(group[1], limit)
	local index = 2
	while index <= #group do
		local operator = group[index]
		local right = group[index + 1]
		if right == nil then error("Ariza expression ends with an operator") end
		if operator == "u" then
			result = M.union(result, ariza_group(right, limit))
		elseif operator == "i" then
			result = M.intersection(result, ariza_group(right, limit))
		else
			error("expected Ariza operator u or i")
		end
		index = index + 2
	end
	return result
end

function M.ariza(value, limit)
	return ariza_group(value, integer(limit or 225, "limit"))
end

function M.limit(values, limit, mode)
	if type(values) ~= "table" then error("x.set.limite expects a list of sieves") end
	limit = integer(limit or 225, "limit")
	local expressions = {}
	for _, value in ipairs(values) do
		if elementary(value) then
			expressions[#expressions + 1] = M.normalize({ value[1], value[2], limit })
		elseif type(value) == "table" and #value == 2
			and tonumber(value[1]) and tonumber(value[2]) then
			expressions[#expressions + 1] = M.normalize({ value[1], value[2], limit })
		else
			error("x.set.limite entries must be {modulus offset} pairs")
		end
	end
	mode = mode or 1
	if mode == 1 or mode == "1" or mode == "union" or mode == "u" then
		return M.union_list(expressions)
	end
	if mode == 2 or mode == "2" or mode == "intersection" or mode == "i" then
		return M.intersection_list(expressions)
	end
	error("mode must be 1 (union) or 2 (intersection)")
end

local function differences(values)
	local result = {}
	for index = 2, #values do result[#result + 1] = values[index] - values[index - 1] end
	return result
end

local function palindrome(values)
	for index = 1, math.floor(#values / 2) do
		if values[index] ~= values[#values - index + 1] then return false end
	end
	return true
end

function M.symmetry_profile(values, range, mode)
	if type(values) ~= "table" or type(range) ~= "table" then
		error("x.set.symmetry-perfil expects sieve list and {first last} range")
	end
	local first = integer(range[1], "range start")
	local last = integer(range[2], "range end")
	local result = {}
	for limit = first, last do
		local expression = M.limit(values, limit, mode)
		local revealed = M.reveal(expression)
		if #revealed > 3 and palindrome(differences(revealed)) then
			for _, member in ipairs(revealed) do
				if member == limit then result[#result + 1] = limit break end
			end
		end
	end
	return result
end

local function progression_in_set(start, step, limit, present)
	if step <= 0 then return false end
	for value = start, limit, step do
		if not present[value] then return false end
	end
	return true
end

function M.decompose(value)
	local values
	if tagged(value, "sieve") or tagged(value, "sieve-u")
		or tagged(value, "sieve-i") or tagged(value, "sieve-c") then
		values = M.reveal(value)
	else
		values = sorted_unique(value)
	end
	if #values == 0 then return {} end
	local present = {}
	for _, item in ipairs(values) do present[item] = true end
	local remaining = copy(present)
	local limit = values[#values]
	local result = {}
	while next(remaining) do
		local start
		for _, item in ipairs(values) do
			if remaining[item] then start = item break end
		end
		local best_step
		for _, other in ipairs(values) do
			local step = math.abs(start - other)
			if step > 0 and progression_in_set(start, step, limit, present)
				and (best_step == nil or step < best_step) then
				best_step = step
			end
		end
		if best_step == nil then best_step = math.max(1, limit - start + 1) end
		result[#result + 1] = { best_step, start, limit }
		for item = start, limit, best_step do remaining[item] = nil end
	end
	return result
end

function M.input(pdobj, selector, atoms)
	if selector == "dddd" then
		return dddd:new_from_atoms(pdobj, atoms):get_table()
	elseif selector == "bang" then
		return nil
	elseif selector == "float" or selector == "symbol" then
		return atoms[1]
	elseif selector == "list" then
		return dddd:new(pdobj, atoms):get_table() or copy(atoms)
	end
	local message = { selector }
	for _, atom in ipairs(atoms or {}) do message[#message + 1] = atom end
	return dddd:new(pdobj, message):get_table() or message
end

function M.output(pdobj, value, typename)
	local object = dddd:new_from_table(pdobj, copy(value))
	if typename then object:set_type(typename) end
	object:output(1)
end

function M.report(pdobj, callback)
	local ok, result = pcall(callback)
	if not ok then
		pdobj:error("[" .. pdobj._name .. "] " .. tostring(result))
		return nil
	end
	return result
end

function M.reload(class)
	function class:in_1_reload()
		self:dofilex(self._scriptname)
	end
end

return M
