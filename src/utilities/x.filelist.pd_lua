local lfilelist = pd.Class:new():register("x.filelist")

-- ─────────────────────────────────────
function lfilelist:initialize(_, args)
	self.inlets = 1
	self.outlets = 1

	self.background = { 245, 245, 245 }
	self:set_size(150, 100)

	self.files = {}
	self.selected = nil

	self.row_height = 12
	self.start = 2
	self.offset = 0

	self.nav_height = 14
	self.arrow_width = 20

	return true
end

-- ─────────────────────────────────────
function lfilelist:visible_rows()
	local _, height = self:get_size()
	return math.max(0, math.floor((height - self.nav_height - self.start) / self.row_height))
end

-- ─────────────────────────────────────
function lfilelist:in_1_list(args)
	table.insert(self.files, args[1])
	self:repaint()
end

-- ─────────────────────────────────────
function lfilelist:mouse_down(x, y)
	local width, height = self:get_size()
	local rows = self:visible_rows()

	-- Navigation arrows
	if y >= height - self.nav_height then
		if x >= width - self.arrow_width * 2 and x < width - self.arrow_width then
			-- Previous page
			self.offset = math.max(0, self.offset - rows)
			self:repaint()
		elseif x >= width - self.arrow_width then
			-- Next page
			local max_offset = math.max(0, #self.files - rows)
			self.offset = math.min(max_offset, self.offset + rows)
			self:repaint()
		end

		return
	end

	-- File selection
	if y < self.start or rows == 0 then
		return
	end

	local row = math.floor((y - self.start) / self.row_height) + 1

	if row < 1 or row > rows then
		return
	end

	local index = self.offset + row

	if index >= 1 and index <= #self.files then
		self.selected = index
		self:repaint()

		self:outlet(1, "symbol", { self.files[index] })
	end
end

-- ─────────────────────────────────────
function lfilelist:paint(g)
	local width, height = self:get_size()
	local rows = self:visible_rows()

	g:set_color(table.unpack(self.background))
	g:fill_all()

	-- Draw visible files
	for row = 1, rows do
		local index = self.offset + row
		local v = self.files[index]

		if not v then
			break
		end

		local y = self.start + (row - 1) * self.row_height

		if index == self.selected then
			g:set_color(90, 140, 220)
			g:fill_rect(2, y, width - 4, self.row_height)
			g:set_color(255, 255, 255)
		else
			g:set_color(0, 0, 0)
		end

		local file = v:match("([^/]+)$") or v
		g:draw_text(file, 5, y + 1, width - 10, 9)
	end

	-- Navigation footer
	local nav_y = height - self.nav_height

	-- g:set_color(225, 225, 225)
	-- g:fill_rect(0, nav_y, width, self.nav_height)

	-- File range indicator
	-- g:set_color(70, 70, 70)
	-- local first = #self.files > 0 and self.offset + 1 or 0
	-- local last = math.min(self.offset + rows, #self.files)

	-- local info = string.format("%d-%d / %d", first, last, #self.files)
	-- g:draw_text(info, 5, nav_y + 2, 110, 10)

	-- Previous arrow
	if self.offset > 0 then
		g:set_color(0, 0, 0)
	else
		g:set_color(160, 160, 160)
	end

	g:draw_text("<", width - self.arrow_width * 2 + 6, nav_y + 2, 12, 10)

	-- Next arrow
	if self.offset + rows < #self.files then
		g:set_color(0, 0, 0)
	else
		g:set_color(160, 160, 160)
	end

	g:draw_text(">", width - self.arrow_width + 6, nav_y + 2, 12, 10)
end

-- ─────────────────────────────────────
function lfilelist:in_1_clear()
	self.files = {}
	self.selected = nil
	self.offset = 0
	self:repaint()
end

-- ─────────────────────────────────────
function lfilelist:in_1_reload()
	if self.clock then
		pcall(function()
			self.clock:destruct()
		end)
		self.clock = nil
	end

	self:dofilex(self._scriptname)
	self:initialize("x.gui.draw", {})
	self:postinitialize()
end
