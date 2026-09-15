local scala = pd.Class:new():register("x.scala")

-- ─────────────────────────────────────
function scala:initialize(_, _)
	self.inlets = 1
	self.outlets = 1
	return true
end

-- ─────────────────────────────────────
function scala:ratio_to_cents(ratio)
	local a, b = ratio:match("^(%d+)%s*/%s*(%d+)$")
	if not a then
		error("Invalid ratio: " .. ratio)
	end

	a = tonumber(a)
	b = tonumber(b)

	return 1200 * math.log(a / b, 2)
end

-- ─────────────────────────────────────
function scala:value_to_cents(s)
	s = s:gsub("^%s+", ""):gsub("%s+$", "")
	if s:find("/") then
		return self:ratio_to_cents(s)
	end
	return tonumber(s)
end

-- ─────────────────────────────────────
function scala:read_scl(filename)
	local cents = { 0.0 } -- 1/1

	local file = assert(io.open(filename, "r"))
	local expected_notes = nil
	local note_count = 0

	for line in file:lines() do
		local l_line = line:gsub("!.*$", "")
		l_line = l_line:gsub("^%s+", ""):gsub("%s+$", "")
		if l_line ~= "" then
			if not expected_notes then
				local n = tonumber(l_line)
				if n then
					expected_notes = n
				end
			else
				local c = self:value_to_cents(l_line)
				table.insert(cents, c)
				note_count = note_count + 1
				if note_count >= expected_notes then
					break
				end
			end
		end
	end

	file:close()
	return cents
end

-- ─────────────────────────────────────
function scala:in_1_open(args)
	local file = pd._canvaspath(self._object) .. "/" .. args[1]
	local escala = self:read_scl(file)

	self:outlet(1, "list", escala)
end

-- ─────────────────────────────────────
function scala:in_1_reload()
	self:dofilex(self._scriptname)
	self:initialize()
	--self:repaint()
end
