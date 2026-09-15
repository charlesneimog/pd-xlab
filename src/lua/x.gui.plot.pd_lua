local lplot = pd.Class:new():register("x.gui.plot")

-- ─────────────────────────────────────
function lplot:initialize(_, args)
	self.inlets = 1

	self.tick_ms = 35
	self.tick_count = 0
	self.dx_per_tick = 2
	self.background = { 225, 225, 225 }

	if #args == 2 then
		self.width = args[1]
		self.height = args[2]
	else
		self.width = 400
		self.height = 200
	end

	self.draws = {}
	self.color = { 255, 0, 0 }
	self.background = { 245, 245, 245 }
	self:set_size(self.width, self.height)

	self.draws = {}

	return true
end

-- ─────────────────────────────────────
function lplot:postinitialize()
	self.clock = pd.Clock:new():register(self, "tick")
	self.clock:delay(self.tick_ms)
end

-- ─────────────────────────────────────
function lplot:tick()
	self.tick_count = self.tick_count + 1
	self:repaint()
	if self.clock then
		self.clock:delay(self.tick_ms)
	end
end
-- ─────────────────────────────────────
function lplot:in_1(sel, args)
	if #args > 1 then
		self:error("Just possible to render float")
		return
	end

	local val = args[1]
	if not self.draws[sel] then
		self.draws[sel] = {}
	end

	table.insert(self.draws[sel], val)

	-- Discard oldest if exceeding max points
	local max_points = math.floor(self.width / self.dx_per_tick)
	if #self.draws[sel] > max_points then
		table.remove(self.draws[sel], 1)
	end
end

-- HSV to RGB helper
local function hsv_to_rgb(h, s, v)
	local c = v * s
	local x = c * (1 - math.abs((h / 60) % 2 - 1))
	local m = v - c
	local r, g, b
	if h < 60 then
		r, g, b = c, x, 0
	elseif h < 120 then
		r, g, b = x, c, 0
	elseif h < 180 then
		r, g, b = 0, c, x
	elseif h < 240 then
		r, g, b = 0, x, c
	elseif h < 300 then
		r, g, b = x, 0, c
	else
		r, g, b = c, 0, x
	end
	return { math.floor((r + m) * 255), math.floor((g + m) * 255), math.floor((b + m) * 255) }
end

-- deterministic color from string
function lplot:string_to_color(sel)
	local hash = 0
	for i = 1, #sel do
		hash = (hash * 31 + sel:byte(i)) % 360 -- keep hash in 0..359
	end
	local hue = hash
	local sat = 0.8 -- fixed saturation
	local val = 0.9 -- fixed brightness
	return hsv_to_rgb(hue, sat, val)
end

-- ─────────────────────────────────────
function lplot:paint(g)
	g:set_color(table.unpack(self.background))
	g:fill_all()

	local margin = 5

	for sel, values in pairs(self.draws) do
		local col = self:string_to_color(sel)
		g:set_color(table.unpack(col))

		local x = self.width - margin
		local prev_x, prev_y = nil, nil

		for i = #values, 1, -1 do
			local y = margin + (1 - values[i]) * (self.height - 2 * margin) -- scale inside margin

			-- draw point (optional)
			g:fill_ellipse(x, y, 5, 5, 2)

			-- draw line to previous point
			if prev_x then
				g:draw_line(x, y, prev_x, prev_y, 1)
			end

			prev_x, prev_y = x, y
			x = x - self.dx_per_tick
			if x < margin then
				break
			end
		end
	end
end

-- ─────────────────────────────────────
function lplot:in_1_reload()
	if self.clock then
		pcall(function()
			self.clock:destruct()
		end)
		self.clock = nil
	end
	self:dofilex(self._scriptname)
	self:initialize("x.gui.plot", {})
	self:postinitialize()
	--self:repaint()
end
