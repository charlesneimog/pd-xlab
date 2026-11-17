local xguistep = pd.Class:new():register("x.gui.step")

-- ─────────────────────────────────────
function xguistep:initialize(_, args)
	self.inlets = 0
	self.outlets = 0
	self.args = args
	self.font = args[2] or 10
	self.text = string.format("%.0f", args[1] or 1)
	self.padding = self.font * 0.25
	self.text_width = #self.text * self.font * 0.6
	self.diameter = math.ceil(math.max(self.font, self.text_width) + self.padding * 2)
	self.size = self.diameter + 2
	self:set_size(self.size, self.size)

	return true
end

-- ─────────────────────────────────────
function xguistep:paint(g)
	g:set_color(255, 0, 0)

	g:stroke_ellipse(1, 1, self.diameter, self.diameter, 1)

	g:draw_text(self.text, (self.size - self.text_width) / 2 + 1, (self.size - self.font) / 2, self.diameter, self.font)
end

-- ─────────────────────────────────────
function xguistep:in_1_reload()
	self:dofilex(self._scriptname)
	self:initialize("x.gui.step", self.args)
	self:postinitialize()
end
