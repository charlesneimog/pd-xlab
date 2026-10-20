local gain = pd.Class:new():register("x.gain~")

local STRIP_WIDTH = 6
local HEIGHT = 80
local TRACK_TOP = 2
local TRACK_BOTTOM = HEIGHT - 1
local HANDLE_HEIGHT = 3

-- ─────────────────────────────────────
local function clamp(value, minimum, maximum)
	return math.max(minimum, math.min(maximum, value))
end

-- ─────────────────────────────────────
function gain:initialize(_, atoms)
	self.inlets = { SIGNAL }
	self.outlets = { SIGNAL }

	-- Saved gain arguments give the GUI a useful size before DSP starts.
	self.channels = math.max(1, #atoms)
	self.target = {}
	self.current = {}

	for channel = 1, self.channels do
		local initial = tonumber(atoms[channel]) or 1
		self.target[channel] = clamp(initial, 0, 1)
		self.current[channel] = self.target[channel]
	end

	self.active_channel = nil
	self.smoothing = 1
	self.blocksize = 64
	self:set_size(STRIP_WIDTH * self.channels, HEIGHT)
	return true
end

-- ─────────────────────────────────────
function gain:dsp(samplerate, blocksize, input_channels)
	self.blocksize = blocksize
	-- A 10 ms one-pole ramp avoids clicks when a fader moves.
	self.smoothing = 1 - math.exp(-1 / (0.01 * samplerate))

	local channels = input_channels and input_channels[1] or 1
	if channels ~= self.channels then
		for channel = self.channels + 1, channels do
			self.target[channel] = 1
			self.current[channel] = 1
		end
		for channel = channels + 1, self.channels do
			self.target[channel] = nil
			self.current[channel] = nil
		end
		self.channels = channels
		self:set_size(STRIP_WIDTH * self.channels, HEIGHT)
		self:repaint()
	end

	self:signal_setmultiout(1, self.channels)
end

-- ─────────────────────────────────────
function gain:perform(input)
	local output = {}

	for frame = 1, self.blocksize do
		for channel = 1, self.channels do
			local current = self.current[channel]
			current = current + (self.target[channel] - current) * self.smoothing
			self.current[channel] = current

			local index = frame + (channel - 1) * self.blocksize
			output[index] = input[index] * current
		end
	end

	return output
end

-- ─────────────────────────────────────
function gain:set_gain(channel, value)
	channel = math.floor(tonumber(channel) or 0)
	value = tonumber(value)
	if channel < 1 or channel > self.channels or not value then
		self:error("gain: expected gain <channel> <value from 0 to 1>")
		return
	end

	self.target[channel] = clamp(value, 0, 1)
	self:repaint()
end

-- ─────────────────────────────────────
-- Messages can be sent to the first inlet, e.g. "gain 2 0.5".
function gain:in_1_gain(atoms)
	self:set_gain(atoms[1], atoms[2])
	self:save_state()
end

-- ─────────────────────────────────────
function gain:in_1_reset()
	for channel = 1, self.channels do
		self.target[channel] = 1
	end
	self:repaint()
	self:save_state()
end

-- ─────────────────────────────────────
function gain:channel_at(x)
	return clamp(math.floor(x / STRIP_WIDTH) + 1, 1, self.channels)
end

-- ─────────────────────────────────────
function gain:update_fader(channel, y)
	local value = (TRACK_BOTTOM - clamp(y, TRACK_TOP, TRACK_BOTTOM)) / (TRACK_BOTTOM - TRACK_TOP)
	self.target[channel] = value
	self:repaint()
end

-- ─────────────────────────────────────
function gain:mouse_down(x, y)
	self.active_channel = self:channel_at(x)
	self:update_fader(self.active_channel, y)
end

-- ─────────────────────────────────────
function gain:mouse_drag(x, y)
	if self.active_channel then
		self:update_fader(self.active_channel, y)
	end
end

-- ─────────────────────────────────────
function gain:mouse_up(x, y)
	if self.active_channel then
		self:update_fader(self.active_channel, y)
		self.active_channel = nil
		self:save_state()
	end
end

-- ─────────────────────────────────────
function gain:save_state()
	local atoms = {}
	for channel = 1, self.channels do
		atoms[#atoms + 1] = self.target[channel]
	end
	self:set_args(atoms)
end

-- ─────────────────────────────────────
function gain:paint(g)
	g:set_color(244, 244, 244)
	g:fill_all()

	for channel = 1, self.channels do
		local left = (channel - 1) * STRIP_WIDTH
		local value = self.target[channel]
		local handle_y = TRACK_BOTTOM - value * (TRACK_BOTTOM - TRACK_TOP)
		local fill_y = handle_y + HANDLE_HEIGHT / 2
		local fill_height = TRACK_BOTTOM - fill_y

		g:set_color(0, 0, 0)
		g:stroke_rect(left, 0, STRIP_WIDTH, HEIGHT, 1)

		if fill_height > 0 then
			g:set_color(80, 235, 80)
			g:fill_rect(left + 1, fill_y, STRIP_WIDTH - 2, fill_height)
		end

		g:set_color(92, 94, 98)
		g:fill_rect(left + 1, handle_y - HANDLE_HEIGHT / 4, STRIP_WIDTH - 2, HANDLE_HEIGHT)

		-- g:set_color(25, 25, 25)
		-- g:draw_line(left + 1, handle_y, left + STRIP_WIDTH - 1, handle_y, 1)
	end
end
