local dddd = require("dddd")

local loris_receive = pd.Class:new():register("x.loris.dddd")

function loris_receive:initialize()
    self.inlets = 1
    self.outlets = 1
    self.partials = {}
    return true
end

-- ─────────────────────────────────────
function loris_receive:in_1_begin()
    self.partials = {}
    self.current = nil
end

-- ─────────────────────────────────────
function loris_receive:in_1_partial(atoms)
    self.current = {atoms[1], {}}
    table.insert(self.partials, self.current)
end

-- ─────────────────────────────────────
function loris_receive:in_1_points(atoms)
    if not self.current then
        return
    end

    local points = self.current[2]

    for i = 1, #atoms, 5 do
        points[#points + 1] = {
            atoms[i],
            atoms[i + 1],
            atoms[i + 2],
            atoms[i + 3],
            atoms[i + 4]
        }
    end
end

-- ─────────────────────────────────────
function loris_receive:in_1_finish()
    local data = dddd:new_from_table(self, self.partials)
    data:output(1)
end
