local eikosany = pd.Class:new():register("x.ji.eikosany-draw")
local dddd = require("dddd")

local DEFAULT_IDENTITIES = { 1, 3, 5, 7, 9, 11 }
local MIN_SIZE = 240
local TWO_PI = math.pi * 2

-- An Euler circuit through K5. Each pair represents a vertex containing the
-- first identity; its three-element complement represents the opposite vertex.
-- This gives the diagram two clean decagons while preserving the CPS symmetry.
local PAIR_ORDER = {
	{ 2, 3 }, { 3, 4 }, { 4, 5 }, { 5, 6 }, { 6, 2 },
	{ 2, 4 }, { 4, 6 }, { 6, 3 }, { 3, 5 }, { 5, 2 },
}

local function copy(values)
	local result = {}
	for index, value in ipairs(values) do result[index] = value end
	return result
end

local function key(indices)
	local values = copy(indices)
	table.sort(values)
	return table.concat(values, ":")
end

local function complement(pair)
	local result = {}
	for index = 2, 6 do
		if index ~= pair[1] and index ~= pair[2] then result[#result + 1] = index end
	end
	return result
end

local function shared_count(left, right)
	local count = 0
	for _, a in ipairs(left) do
		for _, b in ipairs(right) do
			if a == b then count = count + 1 break end
		end
	end
	return count
end

local function enabled(value)
	return value ~= nil and value ~= 0 and value ~= "0"
end

function eikosany:initialize(_, args)
	self.inlets = 1
	self.outlets = 3
	self.width = math.max(tonumber(args[1]) or 520, MIN_SIZE)
	self.height = math.max(tonumber(args[2]) or self.width, MIN_SIZE)
	self.identities = copy(DEFAULT_IDENTITIES)
	self.show_labels = true
	self.show_connections = true
	self.selected = nil
	self.hovered = nil
	self.vertices = {}
	self.edges = {}
	self:set_size(self.width, self.height)
	self:build_graph()
	return true
end

function eikosany:build_graph()
	local vertices, by_key = {}, {}
	for position, pair in ipairs(PAIR_ORDER) do
		local angle = -math.pi / 2 + (position - 1) * TWO_PI / #PAIR_ORDER
		local outer_indices = { 1, pair[1], pair[2] }
		table.sort(outer_indices)
		local inner_indices = complement(pair)
		local outer = { indices = outer_indices, angle = angle, ring = "outer" }
		local inner = { indices = inner_indices, angle = angle + math.pi, ring = "inner" }
		vertices[#vertices + 1] = outer
		vertices[#vertices + 1] = inner
	end

	for index, vertex in ipairs(vertices) do
		vertex.values = {}
		for _, identity_index in ipairs(vertex.indices) do
			vertex.values[#vertex.values + 1] = self.identities[identity_index]
		end
		vertex.label = table.concat(vertex.values, "-")
		by_key[key(vertex.indices)] = index
	end

	local edges = {}
	for left = 1, #vertices - 1 do
		for right = left + 1, #vertices do
			if shared_count(vertices[left].indices, vertices[right].indices) == 2 then
				edges[#edges + 1] = { left, right }
			end
		end
	end

	self.vertices = vertices
	self.vertex_by_key = by_key
	self.edges = edges
	self:update_positions()
end

function eikosany:update_positions()
	local cx, cy = self.width / 2, self.height / 2 + 5
	local available = math.min(self.width, self.height - 28)
	local outer_radius = available * 0.39
	local inner_radius = available * 0.22
	for _, vertex in ipairs(self.vertices) do
		local radius = vertex.ring == "outer" and outer_radius or inner_radius
		vertex.x = cx + math.cos(vertex.angle) * radius
		vertex.y = cy + math.sin(vertex.angle) * radius
		vertex.label_radius = radius + (vertex.ring == "outer" and 16 or 13)
	end
end

function eikosany:set_identities(values)
	if #values ~= 6 then
		self:error("[x.ji.eikosany-draw] expected exactly six identities")
		return
	end
	self.identities = copy(values)
	self.selected = nil
	self.hovered = nil
	self:build_graph()
	self:repaint()
end

function eikosany:in_1_list(atoms)
	self:set_identities(atoms)
end

function eikosany:in_1_bang()
	if self.selected then self:output_selection(self.selected) end
end

function eikosany:in_1_labels(atoms)
	self.show_labels = enabled(atoms[1])
	self:repaint()
end

function eikosany:in_1_connections(atoms)
	self.show_connections = enabled(atoms[1])
	self:repaint()
end

function eikosany:in_1_size(atoms)
	self.width = math.max(tonumber(atoms[1]) or self.width, MIN_SIZE)
	self.height = math.max(tonumber(atoms[2]) or atoms[1] or self.height, MIN_SIZE)
	self:set_size(self.width, self.height)
	self:update_positions()
	self:repaint()
end

function eikosany:in_1_clear()
	self.selected = nil
	self:repaint()
end

function eikosany:in_1_reset()
	self:set_identities(DEFAULT_IDENTITIES)
end

function eikosany:values_for(indices)
	local values = {}
	for _, index in ipairs(indices) do values[#values + 1] = self.identities[index] end
	return values
end

function eikosany:tetrads_for(index)
	local selected = self.vertices[index].indices
	local selected_set = {}
	for _, identity in ipairs(selected) do selected_set[identity] = true end

	-- A subharmonic tetrad contains the four triples of a four-identity set.
	local subharmonic = {}
	for identity = 1, 6 do
		if not selected_set[identity] then
			local set = copy(selected)
			set[#set + 1] = identity
			table.sort(set)
			local row = {}
			for omitted = 1, 4 do
				local triple = {}
				for position = 1, 4 do
					if position ~= omitted then triple[#triple + 1] = set[position] end
				end
				row[#row + 1] = self:values_for(triple)
			end
			subharmonic[#subharmonic + 1] = row
		end
	end

	-- A harmonic tetrad contains the four triples sharing one identity pair.
	local harmonic = {}
	for left = 1, 2 do
		for right = left + 1, 3 do
			local fixed = { selected[left], selected[right] }
			local fixed_set = { [fixed[1]] = true, [fixed[2]] = true }
			local row = {}
			for identity = 1, 6 do
				if not fixed_set[identity] then
					local triple = { fixed[1], fixed[2], identity }
					table.sort(triple)
					row[#row + 1] = self:values_for(triple)
				end
			end
			harmonic[#harmonic + 1] = row
		end
	end
	return subharmonic, harmonic
end

function eikosany:output_selection(index)
	local subharmonic, harmonic = self:tetrads_for(index)
	-- Follow Pd's right-to-left outlet order.
	dddd:new_from_table(self, harmonic):output(3)
	dddd:new_from_table(self, subharmonic):output(2)
	self:outlet(1, "list", copy(self.vertices[index].values))
end

function eikosany:vertex_at(x, y)
	local nearest, nearest_distance = nil, math.huge
	for index, vertex in ipairs(self.vertices) do
		local dx, dy = x - vertex.x, y - vertex.y
		local distance = dx * dx + dy * dy
		if distance < nearest_distance then
			nearest, nearest_distance = index, distance
		end
	end
	if nearest_distance <= 14 * 14 then return nearest end
	return nil
end

function eikosany:mouse_move(x, y)
	local hovered = self:vertex_at(x, y)
	if hovered ~= self.hovered then
		self.hovered = hovered
		self:repaint()
	end
end

function eikosany:mouse_down(x, y)
	local selected = self:vertex_at(x, y)
	if selected then
		self.selected = selected
		self:output_selection(selected)
	else
		self.selected = nil
	end
	self:repaint()
end

function eikosany:is_connected(index)
	if not self.selected or index == self.selected then return false end
	return shared_count(self.vertices[index].indices, self.vertices[self.selected].indices) == 2
end

function eikosany:draw_edge(g, edge, color, width)
	local left, right = self.vertices[edge[1]], self.vertices[edge[2]]
	g:set_color(table.unpack(color))
	g:draw_line(left.x, left.y, right.x, right.y, width)
end

function eikosany:paint(g)
	g:set_color(250, 250, 248)
	g:fill_all()

	if self.show_connections then
		for _, edge in ipairs(self.edges) do self:draw_edge(g, edge, { 211, 214, 218 }, 1) end
	end

	if self.selected then
		for _, edge in ipairs(self.edges) do
			if edge[1] == self.selected or edge[2] == self.selected then
				self:draw_edge(g, edge, { 38, 112, 166 }, 2)
			end
		end
	end

	local cx, cy = self.width / 2, self.height / 2 + 5
	for index, vertex in ipairs(self.vertices) do
		local radius, fill = 5, { 44, 48, 52 }
		if self:is_connected(index) then fill = { 60, 145, 195 }; radius = 6 end
		if index == self.hovered then fill = { 236, 151, 49 }; radius = 7 end
		if index == self.selected then fill = { 205, 63, 55 }; radius = 8 end
		g:set_color(250, 250, 248)
		g:fill_ellipse(vertex.x - radius - 2, vertex.y - radius - 2, radius * 2 + 4, radius * 2 + 4)
		g:set_color(table.unpack(fill))
		g:fill_ellipse(vertex.x - radius, vertex.y - radius, radius * 2, radius * 2)

		if self.show_labels then
			local dx, dy = vertex.x - cx, vertex.y - cy
			local length = math.sqrt(dx * dx + dy * dy)
			local label_x = cx + dx / length * vertex.label_radius
			local label_y = cy + dy / length * vertex.label_radius
			local text_width = math.max(28, #vertex.label * 5.5)
			g:set_color(250, 250, 248)
			g:fill_rect(label_x - text_width / 2 - 2, label_y - 6, text_width + 4, 12)
			g:set_color(31, 35, 38)
			g:draw_text(vertex.label, label_x - text_width / 2, label_y - 5, text_width, 10)
		end
	end

	g:set_color(31, 35, 38)
	local title = "Eikosany  " .. table.concat(self.identities, "-")
	g:draw_text(title, 7, 5, self.width - 14, 12)
end

function eikosany:in_1_reload()
	self:dofilex(self._scriptname)
end
