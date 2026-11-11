local markers = pd.Class:new():register("x.markers")

-- ─────────────────────────────────────
function markers:initialize(_, atoms)
	self.inlets = 1
	self:set_size(200, 100)

	self.pos = {}
	return true
end

-- ─────────────────────────────────────
function markers:in_1_tab(atoms)
	self.table = pd.Table:new():sync(atoms[1])
	self:repaint()
end

-- ─────────────────────────────────────
function markers:mouse_down(x, y)
	self.pos[#self.pos + 1] = x
	self:repaint()
end

-- ─────────────────────────────────────
function markers:paint(g)
	local width, height = self:get_size()
	local length = self.table:length()

	g:set_color(255, 255, 255)
	g:fill_all()

	if length < 2 then
		return
	end

	g:set_color(0, 0, 0)

	for x = 0, width - 1 do
		local start = math.floor(x * length / width)
		local stop = math.min(math.floor((x + 1) * length / width) - 1, length - 1)

		local min = 1
		local max = -1

		for i = start, stop do
			local sample = self.table:get(i)

			if sample < min then
				min = sample
			end
			if sample > max then
				max = sample
			end
		end

		local y1 = (1 - max) * height / 2
		local y2 = (1 - min) * height / 2

		g:draw_line(x, y1, x, y2, 1)
	end

	g:set_color(225, 0, 0)

	for i = 1, #self.pos do
		local x = self.pos[i]
		g:draw_line(x, 2, x, height - 2, 1)
	end
end

-- ─────────────────────────────────────
function markers:in_1_reload()
	self:dofilex(self._scriptname)
	self:initialize("x.markers", {})
	self:postinitialize()
end
