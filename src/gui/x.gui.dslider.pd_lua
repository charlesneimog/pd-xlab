local lslider = pd.Class:new():register("x.gui.dslider")

-- ─────────────────────────────────────
function lslider:initialize(_, args)
	self.inlets = 1
	self.outlets = 1

	self.width = 200
	self.height = 10

	self.draws = {}
	self.color = { 255, 0, 0 }
	self:set_size(self.width, self.height)

	self.rect1 = { 1, 1 }
	self.rect2 = { self.width / 2, 1 }
	self.margin = 4

	self.arraysize = 100
	self.proportion = self.arraysize / (self.width - self.margin)

	return true
end

-- ─────────────────────────────────────
function lslider:in_1_arraysize(atoms)
	self.arraysize = atoms[1]
	self.proportion = self.arraysize / (self.width - self.margin)
	pd.post(self.arraysize)
end

-- ─────────────────────────────────────
function lslider:mouse_down(x, y)
	if x > self.rect1[1] and x < self.rect1[1] + self.margin then
		self.rect1down = true
	end
	if x > self.rect2[1] and x < self.rect2[1] + self.margin then
		self.rect2down = true
	end
end

-- ─────────────────────────────────────
function lslider:mouse_up(x, y)
	self.rect1down = false
	self.rect2down = false
end

-- ─────────────────────────────────────
function lslider:mouse_drag(x, y)
	if self.rect1down and x < self.rect2[1] - self.margin then
		if x > self.margin / 2 and x < self.width - self.margin then
			self.rect1[1] = x
		end
	end

	if self.rect2down and x > self.rect1[1] + self.margin then
		if x > self.margin / 2 and x < self.width - self.margin then
			self.rect2[1] = x
		end
	end

	self:outlet(1, "list", { self.rect1[1] * self.proportion, self.rect2[1] * self.proportion })
	self:repaint()
end

-- ─────────────────────────────────────
function lslider:paint(g)
	g:set_color(245, 245, 245)
	g:fill_all()

	g:set_color(255, 30, 30)
	g:fill_rect(self.rect1[1], self.rect1[2], self.margin, self.height - self.margin / 2)

	g:set_color(30, 200, 30)
	g:fill_rect(self.rect2[1], self.rect2[2], self.margin, self.height - self.margin / 2)
end

-- ─────────────────────────────────────
function lslider:in_1_reload()
	self:dofilex(self._scriptname)
	self:initialize("x.gui.draw", {})
	self:postinitialize()
	--self:repaint()
end
