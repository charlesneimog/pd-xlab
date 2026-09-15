local ldraw = pd.Class:new():register("x.gui.draw")
local bhack = require("bhack")

-- ─────────────────────────────────────
function ldraw:initialize(_, args)
	self.inlets = 1

	self.tick_ms = 40
	self.tick_count = 0
	self.dx_per_tick = 2
	self.background = { 225, 225, 225 }

	if #args == 2 then
		self.width = args[1]
		self.height = args[2]
	else
		self.width = 600
		self.height = 200
	end

	self.draws = {}
	self.color = { 255, 0, 0 }
	self:set_size(self.width, self.height)
	return true
end

-- ─────────────────────────────────────
function ldraw:postinitialize()
	self.clock = pd.Clock:new():register(self, "tick")
	self.clock:delay(self.tick_ms)
end

-- ─────────────────────────────────────
function ldraw:tick()
	self.tick_count = self.tick_count + 1
	self:repaint()
	if self.clock then
		self.clock:delay(self.tick_ms)
	end
end

-- ─────────────────────────────────────
function ldraw:in_1_dddd(atoms)
	local id = atoms[4]
	local dddd = bhack.dddd:new_fromid(self, id)

	local svg = dddd:get_table()

	self.draws[atoms[1]] = {
		"svg",
		atoms[2], -- x
		atoms[3], -- y
		atoms[5] or 50, -- width (must exist for scroll logic)
		atoms[6] or 50, -- height (optional but consistent)
		self.tick_count,
		self.color,
		svg,
	}
end

-- ─────────────────────────────────────
function ldraw:in_1_color(args)
	local r = tonumber(args[1]) or 0
	local g = tonumber(args[2]) or 0
	local b = tonumber(args[3]) or 0
	self.color = { r, g, b }
end

-- ─────────────────────────────────────
function ldraw:in_1_ellipse(args)
	local y = args[2]
	local w = args[3]
	local h = args[4]
	local x = self.width - w
	self.draws[args[1]] = {
		"ellipse",
		x,
		y,
		w,
		h,
		self.tick_count,
		self.color,
	}
end

-- ─────────────────────────────────────
function ldraw:in_1_rect(args)
	local y = args[2]
	local w = args[3]
	local h = args[4]
	local x = self.width - w
	self.draws[args[1]] = {
		"rect",
		x,
		y,
		w,
		h,
		self.tick_count,
		self.color,
	}
end

-- ─────────────────────────────────────
function ldraw:in_1_path(args)
	if (#args - 1) % 2 ~= 0 then
		self:error("Wrong args for path, must be pairs of x y")
		return
	end

	local pts = {}
	local min_x = math.huge
	local max_x = -math.huge

	for i = 2, #args, 2 do
		local x = tonumber(args[i])
		local y = tonumber(args[i + 1])

		if x < min_x then
			min_x = x
		end
		if x > max_x then
			max_x = x
		end

		pts[#pts + 1] = { x, y }
	end

	-- shift so the rightmost point starts at the right border
	local shift = (self.width - 1) - max_x

	for i = 1, #pts do
		pts[i][1] = pts[i][1] + shift
	end

	self.draws[args[1]] = {
		"path",
		min_x + shift,
		0,
		max_x - min_x,
		0,
		self.tick_count,
		self.color,
		pts,
	}
end

-- ─────────────────────────────────────
function ldraw:in_1_background(args)
	self.background = args
end

-- ─────────────────────────────────────
function ldraw:paint(g)
	g:set_color(table.unpack(self.background))
	g:fill_all()

	local to_remove = {}

	for k, v in pairs(self.draws) do
		g:set_color(v[7][1], v[7][2], v[7][3])
		local born_tick = v[6]
		local dx = (self.tick_count - born_tick) * self.dx_per_tick
		local x = v[2] - dx
		local w = tonumber(v[4])

		if (x + w) < w then
			to_remove[#to_remove + 1] = k
		else
			if v[1] == "ellipse" then
				g:fill_ellipse(x, v[3], v[4], v[5])
			elseif v[1] == "rect" then
				g:fill_rect(x, v[3], v[4], v[5])
			elseif v[1] == "path" then
				local pts = v[8]
				if #pts > 1 then
					local p = Path(pts[1][1] - dx, pts[1][2])

					for i = 2, #pts do
						p:line_to(pts[i][1] - dx, pts[i][2])
					end

					g:stroke_path(p, 1)
				end
			end
		end
	end

	for k, v in pairs(self.draws) do
		g:set_color(v[7][1], v[7][2], v[7][3])
		local born_tick = v[6]
		local dx = (self.tick_count - born_tick) * self.dx_per_tick
		local x = v[2] - dx
		local w = tonumber(v[4])

		if (x + w) < w then
			to_remove[#to_remove + 1] = k
		else
			if v[1] == "svg" then
				g:draw_svg(v[8], x, v[3])
			end
		end
	end

	for i = 1, #to_remove do
		self.draws[to_remove[i]] = nil
	end
end

-- ─────────────────────────────────────
function ldraw:in_1_reload()
	if self.clock then
		pcall(function()
			self.clock:destruct()
		end)
		self.clock = nil
	end
	self:dofilex(self._scriptname)
	self:initialize("x.gui.draw", {})
	self:postinitialize()
	--self:repaint()
end
