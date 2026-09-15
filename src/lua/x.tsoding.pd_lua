local tsoding = pd.Class:new():register("x.tsoding")

-- ─────────────────────────────────────
function tsoding:initialize(name, args)
	self.inlets = 1
	return true
end

-- ─────────────────────────────────────
function tsoding:in_1_reload()
	self:dofilex(self._scriptname)
	self:initialize()
	--self:repaint()
end
