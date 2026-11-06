--[[
	gmod-mcp bootstrap agent (client realm)

	Loaded once into the client Lua state by core.dll. Exposes a single entry
	point, MCP._dispatch(name, argJson), which the native pump calls on the main
	thread. All game interaction (input synthesis, state introspection, VGUI)
	lives here where the Lua API is rich; the C++ side only handles IPC, the D3D9
	screenshot hook, and console capture.

	Input is injected through a CreateMove hook reading MCP.state — this never
	touches the OS mouse/keyboard.
]]

MCP = MCP or {}
MCP.version = "0.1.0"

----------------------------------------------------------------------
-- State
----------------------------------------------------------------------
MCP.state = MCP.state or {
	held = {},                                   -- buttonName -> expireTime | true
	move = { forward = 0, side = 0, up = 0, expire = 0 }, -- expire: time | true | 0(off)
	taps = {},                                   -- { {button=IN_x, expire=time}, ... }
	look = nil,                                  -- { p, y, r, speed } | nil
}

-- Friendly button names -> IN_ enums.
local BUTTONS = {
	attack = IN_ATTACK, attack2 = IN_ATTACK2, fire = IN_ATTACK, altfire = IN_ATTACK2,
	jump = IN_JUMP, duck = IN_DUCK, crouch = IN_DUCK,
	forward = IN_FORWARD, back = IN_BACK, use = IN_USE, reload = IN_RELOAD,
	moveleft = IN_MOVELEFT, moveright = IN_MOVERIGHT,
	turnleft = IN_LEFT, turnright = IN_RIGHT,
	speed = IN_SPEED, sprint = IN_SPEED, walk = IN_WALK, zoom = IN_ZOOM,
	score = IN_SCORE, weapon1 = IN_WEAPON1, weapon2 = IN_WEAPON2,
}

----------------------------------------------------------------------
-- Helpers
----------------------------------------------------------------------
local function v2t(v) return { x = v.x, y = v.y, z = v.z } end
local function a2t(a) return { pitch = a.p, yaw = a.y, roll = a.r } end

local function entInfo(e, fromPos)
	if not IsValid(e) then return nil end
	local info = { id = e:EntIndex(), class = e:GetClass(), pos = v2t(e:GetPos()) }
	if fromPos then info.dist = math.floor(fromPos:Distance(e:GetPos())) end
	local mdl = e.GetModel and e:GetModel()
	if mdl then info.model = mdl end
	if e:IsPlayer() then
		info.isPlayer = true
		info.name = e:Nick()
		info.health = e:Health()
	elseif e:IsNPC() then
		info.isNPC = true
		info.health = e:Health()
	end
	return info
end

local function safeText(panel)
	if not panel.GetText then return nil end
	local ok, t = pcall(panel.GetText, panel)
	if ok and type(t) == "string" and t ~= "" then return t end
	return nil
end

local function tableKeys(t)
	local out = {}
	for k in pairs(t) do out[#out + 1] = k end
	return out
end

-- Depth-first search of the VGUI tree for the first visible panel whose class,
-- name, or text contains `needle` (lowercased substring).
local function findPanel(needle)
	local match
	local function walk(p)
		if match or not IsValid(p) then return end
		if p:IsVisible() then
			local cls = string.lower(p:GetClassName() or "")
			local nm = p.GetName and string.lower(p:GetName() or "") or ""
			local txt = safeText(p)
			txt = txt and string.lower(txt) or ""
			if cls:find(needle, 1, true) or nm:find(needle, 1, true) or txt:find(needle, 1, true) then
				match = p
				return
			end
		end
		for _, c in ipairs(p:GetChildren()) do walk(c) end
	end
	walk(vgui.GetWorldPanel())
	return match
end

----------------------------------------------------------------------
-- CreateMove: apply synthesised input every tick (no OS input involved)
----------------------------------------------------------------------
hook.Add("CreateMove", "gmod_mcp_createmove", function(cmd)
	local s = MCP.state
	local now = CurTime()

	-- Held buttons
	local btn = cmd:GetButtons()
	for name, expire in pairs(s.held) do
		if expire == true or expire > now then
			local b = BUTTONS[name]
			if b then btn = bit.bor(btn, b) end
		else
			s.held[name] = nil
		end
	end

	-- Discrete taps
	for i = #s.taps, 1, -1 do
		local t = s.taps[i]
		if now <= t.expire then
			btn = bit.bor(btn, t.button)
		else
			table.remove(s.taps, i)
		end
	end
	cmd:SetButtons(btn)

	-- Movement
	local m = s.move
	if m.expire == true or (type(m.expire) == "number" and m.expire > now) then
		if m.forward ~= 0 then cmd:SetForwardMove(m.forward) end
		if m.side ~= 0 then cmd:SetSideMove(m.side) end
		if m.up ~= 0 then cmd:SetUpMove(m.up) end
	else
		m.forward, m.side, m.up, m.expire = 0, 0, 0, 0
	end

	-- Smoothed look toward a target angle
	if s.look then
		local cur = cmd:GetViewAngles()
		local spd = s.look.speed or 8
		local dp = math.Clamp(s.look.p - cur.p, -spd, spd)
		local dy = math.Clamp(math.AngleDifference(s.look.y, cur.y), -spd, spd)
		local na = Angle(cur.p + dp, cur.y + dy, 0)
		na.p = math.Clamp(na.p, -89, 89)
		cmd:SetViewAngles(na)
		if math.abs(s.look.p - na.p) < 0.15 and math.abs(math.AngleDifference(s.look.y, na.y)) < 0.15 then
			s.look = nil
		end
	end
end)

----------------------------------------------------------------------
-- Command pump: drain native commands every frame, inside a render pass so
-- screenshots (render.Capture) work. _mcp_native_pump is a C function the DLL
-- registers as a global before this script runs.
----------------------------------------------------------------------
-- DrawOverlay runs after all VGUI/HUD painting, so render.Capture there sees the
-- whole frame including menus. (PostRender runs *before* VGUI is drawn, so
-- captures from it miss every panel.) PostRender is kept as a fallback in case
-- another addon's DrawOverlay hook returns a value and stops ours being called.
if _mcp_native_pump then
	local lastOverlay = 0
	hook.Add("DrawOverlay", "gmod_mcp_pump", function()
		lastOverlay = SysTime()
		_mcp_native_pump()
	end)
	hook.Add("PostRender", "gmod_mcp_pump", function()
		if SysTime() - lastOverlay > 0.25 then _mcp_native_pump() end
	end)
end

----------------------------------------------------------------------
-- Methods (each takes a decoded args table, returns a result table)
----------------------------------------------------------------------
function MCP.Run(args)
	local code = args.code or ""
	local out = {}
	local realPrint = print
	local function cap(...)
		local parts = {}
		for i = 1, select("#", ...) do parts[i] = tostring(select(i, ...)) end
		out[#out + 1] = table.concat(parts, "\t")
	end

	local fn, cerr = CompileString(code, "gmod_mcp_run", false)
	if not fn then return { output = "", returns = {}, error = "compile error: " .. tostring(cerr) } end

	print = cap
	local results = { pcall(fn) }
	print = realPrint

	if not results[1] then
		return { output = table.concat(out, "\n"), returns = {}, error = tostring(results[2]) }
	end
	local returns = {}
	for i = 2, #results do returns[#returns + 1] = tostring(results[i]) end
	return { output = table.concat(out, "\n"), returns = returns }
end

function MCP.GetState(args)
	local radius = args.radius or 1024
	local maxEntities = args.maxEntities or 30
	-- During a map/server load the client realm exists but its gameplay globals
	-- (LocalPlayer, CurTime, ...) aren't installed yet, so LocalPlayer() throws
	-- "attempt to call global 'LocalPlayer' (a nil value)". Report a loading
	-- state instead of erroring when called mid-transition.
	if type(LocalPlayer) ~= "function" then
		return { loading = true, map = (game and game.GetMap and game.GetMap()) or nil }
	end
	local ply = LocalPlayer()
	if not IsValid(ply) then return { loading = true, error = "no local player (in menu or loading)" } end

	local origin = ply:GetPos()
	local s = {
		map = game.GetMap(),
		curtime = CurTime(),
		player = {
			name = ply:Nick(),
			steamid = ply:SteamID(),
			pos = v2t(origin),
			eyePos = v2t(ply:EyePos()),
			eyeAngles = a2t(ply:EyeAngles()),
			velocity = v2t(ply:GetVelocity()),
			health = ply:Health(),
			armor = ply:Armor(),
			alive = ply:Alive(),
			team = ply:Team(),
			onGround = ply:IsOnGround(),
			crouching = ply:Crouching(),
		},
	}

	local wep = ply:GetActiveWeapon()
	if IsValid(wep) then
		s.weapon = {
			class = wep:GetClass(),
			clip1 = wep:Clip1(),
			ammo1 = ply:GetAmmoCount(wep:GetPrimaryAmmoType()),
		}
	end

	local tr = ply:GetEyeTrace()
	if tr and tr.Hit then
		s.aim = {
			hit = true,
			pos = v2t(tr.HitPos),
			dist = math.floor(ply:EyePos():Distance(tr.HitPos)),
			entity = IsValid(tr.Entity) and entInfo(tr.Entity, origin) or nil,
		}
	end

	local nearby = {}
	for _, e in ipairs(ents.FindInSphere(origin, radius)) do
		if e ~= ply and IsValid(e) then
			local info = entInfo(e, origin)
			if info then nearby[#nearby + 1] = info end
		end
	end
	table.sort(nearby, function(a, b) return (a.dist or 0) < (b.dist or 0) end)
	while #nearby > maxEntities do table.remove(nearby) end
	s.entities = nearby

	s.ui = {
		cursorVisible = vgui.CursorVisible(),
		gameUIVisible = gui.IsGameUIVisible and gui.IsGameUIVisible() or false,
	}
	return s
end

function MCP.SetInput(args)
	local s = MCP.state
	local expire = args.durationMs and (CurTime() + args.durationMs / 1000) or true
	if args.clearOthers then s.held = {} end

	if args.buttons then
		for _, name in ipairs(args.buttons) do
			name = string.lower(tostring(name))
			if BUTTONS[name] then s.held[name] = expire end
		end
	end
	if args.forward or args.side or args.up then
		-- forward/side/up arrive as a fraction (-1..1) of move speed; scale to the
		-- engine units SetForwardMove expects (the server clamps to the real max).
		local ply = LocalPlayer()
		local sp = (IsValid(ply) and ply.GetRunSpeed and ply:GetRunSpeed()) or 400
		if not sp or sp <= 0 then sp = 400 end
		s.move.forward = (args.forward or 0) * sp
		s.move.side = (args.side or 0) * sp
		s.move.up = (args.up or 0) * sp
		s.move.expire = expire
	end
	return { held = tableKeys(s.held), move = { forward = s.move.forward, side = s.move.side, up = s.move.up } }
end

function MCP.ClearInput(args)
	MCP.state.held = {}
	MCP.state.move = { forward = 0, side = 0, up = 0, expire = 0 }
	MCP.state.taps = {}
	if args.look then MCP.state.look = nil end
	return { cleared = true }
end

function MCP.Look(args)
	local ply = LocalPlayer()
	if not IsValid(ply) then return { error = "no local player" } end
	local cur = ply:EyeAngles()

	local target
	if args.relative then
		target = Angle(cur.p + (args.pitch or 0), cur.y + (args.yaw or 0), 0)
	else
		target = Angle(args.pitch or cur.p, args.yaw or cur.y, 0)
	end
	target.p = math.Clamp(target.p, -89, 89)

	local speed = args.instant and 1e9 or (args.speed or 8)
	MCP.state.look = { p = target.p, y = target.y, r = 0, speed = speed }
	return { target = { pitch = target.p, yaw = target.y }, speed = speed }
end

function MCP.Press(args)
	local name = string.lower(tostring(args.button or "attack"))
	local b = BUTTONS[name]
	if not b then return { error = "unknown button: " .. name } end
	local durMs = args.durationMs or 100
	MCP.state.taps[#MCP.state.taps + 1] = { button = b, expire = CurTime() + durMs / 1000 }
	return { pressed = name, durationMs = durMs }
end

-- render.Capture is blocked while the pause menu (GameUI) is up, AND the menu is
-- still drawn on the current frame even after we hide it. So if it's open, close
-- it and signal retry=true — the caller retries a frame later and captures a clean
-- frame with the menu gone. Returns an error/retry table if not ready, else nil.
local function captureGuard()
	if not render or not render.Capture then return { error = "render.Capture unavailable (menu realm?)" } end
	if gui.IsGameUIVisible and gui.IsGameUIVisible() then
		if gui.HideGameUI then gui.HideGameUI() end
		return { error = "pause menu was open; closed it — retrying for a clean frame", retry = true }
	end
	return nil
end

-- Capture a screen rect and return the image envelope (or an error table).
local function captureRegion(x, y, w, h, fmt, quality)
	local ok, data = pcall(render.Capture, {
		format = fmt,
		x = x, y = y, w = w, h = h,
		quality = quality, alpha = false,
	})
	if not ok or not data then
		return { error = "render.Capture failed (not in a render pass): " .. tostring(data) }
	end
	return { format = fmt, width = w, height = h, base64 = util.Base64Encode(data, true) }
end

----------------------------------------------------------------------
-- Marks ("set-of-marks"): numbered labels over entities and clickable panels,
-- drawn into an off-screen copy of the frame so the player never sees them.
----------------------------------------------------------------------
local markFonts = {}
local function markFont()
	local size = math.max(14, math.floor(ScrH() / 60))
	local name = "gmod_mcp_mark_" .. size
	if not markFonts[name] then
		surface.CreateFont(name, { font = "Arial", size = size, weight = 800, antialias = true })
		markFonts[name] = true
	end
	return name
end

-- Screen rect of a panel clipped by all its ancestors (scroll panels etc.), or nil.
local function visibleRect(p)
	local x, y = p:LocalToScreen(0, 0)
	local w, h = p:GetSize()
	local x0, y0, x1, y1 = x, y, x + w, y + h
	local par = p:GetParent()
	while IsValid(par) do
		local px, py = par:LocalToScreen(0, 0)
		local pw, ph = par:GetSize()
		x0, y0 = math.max(x0, px), math.max(y0, py)
		x1, y1 = math.min(x1, px + pw), math.min(y1, py + ph)
		par = par:GetParent()
	end
	x0, y0 = math.max(x0, 0), math.max(y0, 0)
	x1, y1 = math.min(x1, ScrW()), math.min(y1, ScrH())
	if x1 - x0 < 4 or y1 - y0 < 4 then return nil end
	return x0, y0, x1 - x0, y1 - y0
end

local function isInteractivePanel(p)
	if p == vgui.GetWorldPanel() then return false end
	if p.IsMouseInputEnabled and not p:IsMouseInputEnabled() then return false end
	local cls = p:GetClassName() or ""
	return p.DoClick ~= nil or p.DoRightClick ~= nil or p.OnEnter ~= nil
		or cls == "TextEntry" or cls == "RichText"
end

-- The panel a click at a screen point would reach: the deepest visible panel
-- containing it that takes mouse input (later children win, matching draw
-- order). Mouse-disabled panels (labels, HUD overlays) don't intercept clicks,
-- so they're ignored. nil if nothing but the world is there.
local function topPanelAt(x, y)
	local best
	local function walk(p)
		if not IsValid(p) or not p:IsVisible() then return end
		local px, py = p:LocalToScreen(0, 0)
		local pw, ph = p:GetSize()
		if x >= px and y >= py and x < px + pw and y < py + ph then
			if p:IsMouseInputEnabled() then best = p end
			for _, c in ipairs(p:GetChildren()) do walk(c) end
		end
	end
	local world = vgui.GetWorldPanel()
	for _, c in ipairs(world:GetChildren()) do walk(c) end
	return best
end

-- True if a menu (a panel taking mouse input, not a HUD overlay) covers the
-- point — entities there are hidden behind UI and aren't worth marking.
local function coveredByMenu(x, y)
	return topPanelAt(x, y) ~= nil
end

local function collectPanelMarks(limit)
	local out = {}
	-- alpha: product of ancestor alphas (0..1); faded-out panels are skipped.
	local function walk(p, alpha)
		if #out >= limit or not IsValid(p) or not p:IsVisible() then return end
		alpha = alpha * (p:GetAlpha() / 255)
		if alpha < 0.05 then return end
		if isInteractivePanel(p) then
			local x, y, w, h = visibleRect(p)
			if x then
				-- Skip panels covered by another window: the topmost panel at the
				-- centre must be this one or one of its children (e.g. its label).
				local top = topPanelAt(math.floor(x + w / 2), math.floor(y + h / 2))
				if top == p or (IsValid(top) and p:IsOurChild(top)) then
					out[#out + 1] = { panel = p, x = x, y = y, w = w, h = h }
				end
			end
		end
		for _, c in ipairs(p:GetChildren()) do walk(c, alpha) end
	end
	walk(vgui.GetWorldPanel(), 1)
	return out
end

local SKIP_MARK_CLASS = { viewmodel = true, gmod_hands = true, physgun_beam = true, worldspawn = true }

local function collectEntityMarks(limit, radius, includeOccluded)
	local ply = (type(LocalPlayer) == "function") and LocalPlayer() or nil
	if not IsValid(ply) then return {} end
	local eye = ply:EyePos()
	local sw, sh = ScrW(), ScrH()
	local out = {}
	for _, e in ipairs(ents.FindInSphere(eye, radius)) do
		if IsValid(e) and e ~= ply and not SKIP_MARK_CLASS[e:GetClass()] and not e:GetNoDraw()
			and not (e:IsWeapon() and IsValid(e:GetOwner())) and e:GetParent() ~= ply then
			local mdl = e:GetModel()
			if mdl and mdl ~= "" then
				local center = e:WorldSpaceCenter()
				local sp = center:ToScreen()
				if sp.visible and sp.x >= 0 and sp.y >= 0 and sp.x < sw and sp.y < sh
					and not coveredByMenu(math.floor(sp.x), math.floor(sp.y)) then
					local visible = true
					if not includeOccluded then
						local tr = util.TraceLine({ start = eye, endpos = center, filter = ply, mask = MASK_SHOT })
						visible = (tr.Entity == e) or tr.Fraction > 0.98
					end
					if visible then
						out[#out + 1] = { entity = e, x = math.floor(sp.x), y = math.floor(sp.y),
							dist = math.floor(eye:Distance(center)) }
					end
				end
			end
		end
	end
	table.sort(out, function(a, b) return a.dist < b.dist end)
	while #out > limit do table.remove(out) end
	return out
end

-- above=true places the label just above (x, y) so it doesn't cover the panel's
-- own content; clamping keeps it on screen at the top edge. `placed` collects
-- drawn label rects: a label that would overlap one is nudged down until clear.
local function drawLabel(font, text, x, y, bg, fg, above, placed)
	surface.SetFont(font)
	local tw, th = surface.GetTextSize(text)
	local pad = 3
	local lw, lh = tw + pad * 2, th + pad * 2
	if above then y = y - lh end
	local bx = math.Clamp(x, 0, ScrW() - lw)
	local by = math.Clamp(y, 0, ScrH() - lh)
	for _ = 1, 8 do
		local hit = false
		for _, r in ipairs(placed) do
			if bx < r[1] + r[3] and r[1] < bx + lw and by < r[2] + r[4] and r[2] < by + lh then
				by = math.min(r[2] + r[4] + 1, ScrH() - lh); hit = true; break
			end
		end
		if not hit then break end
	end
	placed[#placed + 1] = { bx, by, lw, lh }
	surface.SetDrawColor(bg.r, bg.g, bg.b, 235)
	surface.DrawRect(bx, by, tw + pad * 2, th + pad * 2)
	surface.SetDrawColor(0, 0, 0, 255)
	surface.DrawOutlinedRect(bx, by, tw + pad * 2, th + pad * 2)
	surface.SetTextColor(fg.r, fg.g, fg.b, 255)
	surface.SetTextPos(bx + pad, by + pad)
	surface.DrawText(text)
end

local ENT_BG, PANEL_BG = Color(255, 215, 0), Color(0, 200, 255)
local BLACK = Color(0, 0, 0)

-- Builds marks for `which` ("entities" | "panels" | "both"), draws them into a
-- copy of the current frame, captures the region, and returns the image plus a
-- legend. The real back buffer is left untouched.
local function captureWithMarks(which, x, y, w, h, fmt, quality, args)
	local limit = math.Clamp(math.floor(args.maxMarks or 60), 1, 200)
	local wantPanels = which == "panels" or which == "both"
	local wantEnts = which == "entities" or which == "both"

	local panels = wantPanels and collectPanelMarks(limit) or {}
	local entsM = wantEnts and collectEntityMarks(math.max(0, limit - #panels), args.radius or 2048,
		args.includeOccluded) or {}

	local legend, lookup = {}, {}
	local n = 0
	for _, m in ipairs(panels) do
		n = n + 1; m.n = n
		local p = m.panel
		local item = { mark = n, kind = "panel", class = p.ClassName or p:GetClassName(),
			x = m.x, y = m.y, w = m.w, h = m.h,
			cx = math.floor(m.x + m.w / 2), cy = math.floor(m.y + m.h / 2) }
		local nm = p.GetName and p:GetName()
		if nm and nm ~= "" then item.name = nm end
		local txt = safeText(p)
		if txt then item.text = string.sub(txt, 1, 80) end
		legend[#legend + 1] = item
		lookup[n] = { panel = p }
	end
	for _, m in ipairs(entsM) do
		n = n + 1; m.n = n
		local e = m.entity
		local item = { mark = n, kind = "entity", id = e:EntIndex(), class = e:GetClass(),
			model = e:GetModel(), dist = m.dist, x = m.x, y = m.y }
		if e:IsPlayer() then item.name = e:Nick() end
		legend[#legend + 1] = item
		lookup[n] = { entity = e }
	end

	local sw, sh = ScrW(), ScrH()
	local rt = GetRenderTargetEx("gmod_mcp_marks_" .. sw .. "x" .. sh, sw, sh, RT_SIZE_LITERAL,
		MATERIAL_RT_DEPTH_NONE, 0, 0, IMAGE_FORMAT_RGB888)
	render.CopyRenderTargetToTexture(rt)
	render.PushRenderTarget(rt)
	local res
	local ok, err = pcall(function()
		cam.Start2D()
		local font = markFont()
		local placed = {}
		for _, m in ipairs(panels) do
			surface.SetDrawColor(PANEL_BG.r, PANEL_BG.g, PANEL_BG.b, 255)
			surface.DrawOutlinedRect(m.x, m.y, m.w, m.h, 2)
			drawLabel(font, tostring(m.n), m.x, m.y, PANEL_BG, BLACK, true, placed)
		end
		for _, m in ipairs(entsM) do
			surface.SetDrawColor(ENT_BG.r, ENT_BG.g, ENT_BG.b, 255)
			surface.DrawRect(m.x - 3, m.y - 3, 6, 6)
			drawLabel(font, tostring(m.n), m.x + 5, m.y - 10, ENT_BG, BLACK, false, placed)
		end
		cam.End2D()
		res = captureRegion(x, y, w, h, fmt, quality)
	end)
	render.PopRenderTarget()
	if not ok then return { error = "marked capture failed: " .. tostring(err) } end
	if res.error then return res end

	-- Only list marks whose anchor lies inside the captured region.
	local inRegion = {}
	for _, item in ipairs(legend) do
		local ax, ay = item.cx or item.x, item.cy or item.y
		if ax >= x and ay >= y and ax < x + w and ay < y + h then inRegion[#inRegion + 1] = item end
	end
	MCP.lastMarks = lookup
	res.marks = inRegion
	return res
end

function MCP.Screenshot(args)
	local guard = captureGuard()
	if guard then return guard end

	local fmt = (args.format == "png") and "png" or "jpeg"
	local w = (args.w and args.w > 0) and args.w or ScrW()
	local h = (args.h and args.h > 0) and args.h or ScrH()
	local x, y = args.x or 0, args.y or 0
	local which = args.marks
	if which == "entities" or which == "panels" or which == "both" then
		return captureWithMarks(which, x, y, w, h, fmt, args.quality or 80, args)
	end
	return captureRegion(x, y, w, h, fmt, args.quality or 80)
end

function MCP.DumpVGUI(args)
	local maxDepth = args.maxDepth or 12
	local filter = args.filter and string.lower(args.filter) or nil
	local includeInvisible = args.includeInvisible or false
	local limit = args.limit or 1500
	local count = 0
	local truncated = false

	local function walk(panel, depth)
		if not IsValid(panel) or depth > maxDepth then return nil end
		if count >= limit then truncated = true; return nil end

		local vis = panel:IsVisible()
		local cls = panel:GetClassName() or "?"
		local nm = panel.GetName and panel:GetName() or nil
		local text = safeText(panel)

		local children = {}
		for _, child in ipairs(panel:GetChildren()) do
			local c = walk(child, depth + 1)
			if c then children[#children + 1] = c end
		end

		local selfMatch = true
		if filter then
			selfMatch = (string.find(string.lower(cls), filter, 1, true) and true)
				or (nm and string.find(string.lower(nm), filter, 1, true) and true)
				or (text and string.find(string.lower(text), filter, 1, true) and true)
				or false
		end
		local includeSelf = (includeInvisible or vis) and selfMatch

		if includeSelf or #children > 0 then
			count = count + 1
			local x, y = panel:LocalToScreen(0, 0)
			local w, h = panel:GetSize()
			local node = { class = cls, x = x, y = y, w = w, h = h, visible = vis }
			if nm and nm ~= "" then node.name = nm end
			if text then node.text = text end
			if #children > 0 then node.children = children end
			return node
		end
		return nil
	end

	local tree = walk(vgui.GetWorldPanel(), 0)
	return { tree = tree or {}, count = count, truncated = truncated }
end

-- Topmost visible panel containing a screen point. Deeper/later-drawn panels win.
local function panelAt(x, y)
	local best
	local function walk(p)
		if not IsValid(p) or not p:IsVisible() then return end
		local px, py = p:LocalToScreen(0, 0)
		local pw, ph = p:GetSize()
		if x >= px and y >= py and x < px + pw and y < py + ph then
			best = p
			for _, c in ipairs(p:GetChildren()) do walk(c) end
		end
	end
	walk(vgui.GetWorldPanel())
	return best
end

-- Resolve the target panel from args: by mark number (from the last marked
-- screenshot), by name/class/text substring, or x,y point.
local function resolveTarget(args)
	if args.mark then
		local m = MCP.lastMarks and MCP.lastMarks[tonumber(args.mark)]
		if m and IsValid(m.panel) then return m.panel end
		return nil
	end
	if args.panel then return findPanel(string.lower(tostring(args.panel))) end
	if args.x and args.y then return panelAt(math.floor(args.x), math.floor(args.y)) end
	return nil
end

-- Screenshot a single panel: resolve it (by name/class/text, screen point, or
-- keyboard focus), then crop the captured frame to its screen bounds plus padding.
function MCP.ScreenshotPanel(args)
	local target, how
	if args.focused then
		target, how = vgui.GetKeyboardFocus(), "focused panel"
		if not IsValid(target) then return { error = "no panel currently has keyboard focus" } end
	else
		target = resolveTarget(args)
		how = args.panel or (args.x and args.y and (args.x .. "," .. args.y)) or "(no selector: pass panel, x/y, or focused)"
		if not IsValid(target) then return { error = "no panel found (" .. tostring(how) .. ")" } end
	end

	local guard = captureGuard()
	if guard then return guard end

	local px, py = target:LocalToScreen(0, 0)
	local pw, ph = target:GetSize()
	local pad = math.max(0, math.floor(args.padding or 0))

	-- Expand by padding, then clamp to the screen so render.Capture stays in bounds.
	local sw, sh = ScrW(), ScrH()
	local x0 = math.Clamp(px - pad, 0, sw)
	local y0 = math.Clamp(py - pad, 0, sh)
	local x1 = math.Clamp(px + pw + pad, 0, sw)
	local y1 = math.Clamp(py + ph + pad, 0, sh)
	local w, h = math.floor(x1 - x0), math.floor(y1 - y0)
	if w < 1 or h < 1 then
		return { error = "panel has no on-screen area (offscreen or zero-size): " .. (target:GetClassName() or "?") }
	end

	local fmt = (args.format == "png") and "png" or "jpeg"
	local res = captureRegion(math.floor(x0), math.floor(y0), w, h, fmt, args.quality or 80)
	if not res.error then
		res.panel = target:GetClassName()
		local nm = target.GetName and target:GetName()
		if nm and nm ~= "" then res.name = nm end
		res.region = { x = math.floor(x0), y = math.floor(y0), w = w, h = h }
	end
	return res
end

-- The engine overrides the VGUI cursor to the real mouse each frame, so a
-- positioned click via gui.Internal* lands at the wrong place. Instead we locate
-- the target panel and invoke its action directly — no OS cursor involved.
function MCP.Cursor(args)
	local target = resolveTarget(args)
	if not IsValid(target) then
		local how = args.mark and ("mark " .. tostring(args.mark) .. " - take a new marked screenshot")
			or args.panel or (tostring(args.x) .. "," .. tostring(args.y))
		return { error = "no panel found (" .. how .. ")" }
	end

	local px, py = target:LocalToScreen(0, 0)
	local w, h = target:GetSize()
	local cx, cy = math.floor(px + w / 2), math.floor(py + h / 2)

	-- Best-effort visual cursor move (not relied upon for the click).
	input.SetCursorPos(cx, cy)
	if gui.InternalCursorMoved then gui.InternalCursorMoved(cx, cy) end

	local result = { x = cx, y = cy, target = target:GetClassName() }
	if args.click then
		local right = args.button == "right"
		local key = right and "DoRightClick" or "DoClick"
		-- The hit panel is often a Label/child drawn on top of the real button
		-- (e.g. spawn icons), so climb to the nearest ancestor with the handler.
		local node, found = target, false
		for _ = 1, 10 do
			if not IsValid(node) then break end
			if node[key] then found = true; break end
			node = node:GetParent()
		end
		local mc = right and MOUSE_RIGHT or MOUSE_LEFT
		pcall(function()
			if found then node[key](node)
			else target:OnMousePressed(mc); target:OnMouseReleased(mc) end
		end)
		result.clicked = right and "right" or "left"
		result.invoked = found and node:GetClassName() or (target:GetClassName() .. ":OnMousePressed")
	end
	return result
end

function MCP.Type(args)
	local text = tostring(args.text or "")
	local target
	if args.mark or args.panel then
		target = resolveTarget(args)
	else
		target = vgui.GetKeyboardFocus()
	end
	if not IsValid(target) then return { error = "no target panel (give mark/panel or focus one)" } end
	if not target.SetText then return { error = "target has no SetText: " .. (target:GetClassName() or "?") } end

	local newText = text
	if args.append and target.GetText then newText = (target:GetText() or "") .. text end
	target:SetText(newText)
	if target.SetValue then pcall(target.SetValue, target, newText) end
	if target.SetCaretPos then pcall(target.SetCaretPos, target, #newText) end
	-- Fire the common text-entry change/enter callbacks so filters/searches run.
	for _, cb in ipairs({ "OnTextChanged", "OnChange", "OnValueChange" }) do
		if target[cb] then pcall(target[cb], target, newText) end
	end
	if args.enter and target.OnEnter then pcall(target.OnEnter, target) end

	return { typed = text, into = target:GetClassName() }
end

----------------------------------------------------------------------
-- Epoch: identifies this Lua state. The MCP table survives agent hot-reloads
-- but not map loads (new state), so a changed epoch tells the server that
-- sequence numbers (errors, events) restarted.
----------------------------------------------------------------------
MCP.epoch = MCP.epoch or (tostring(math.floor(SysTime() * 1000)) .. "-" .. tostring(math.random(1, 1e6)))

-- JSON-friendly summary of an arbitrary Lua value (hook arguments etc.).
local function summarize(v, depth)
	depth = depth or 0
	local t = type(v)
	if v == nil then return "nil" end
	if t == "string" then return #v > 300 and (string.sub(v, 1, 300) .. "...") or v end
	if t == "number" or t == "boolean" then return v end
	if isentity(v) then
		if not IsValid(v) then return { entity = "NULL" } end
		local o = { entity = v:EntIndex(), class = v:GetClass() }
		if v:IsPlayer() then o.name = v:Nick() end
		return o
	end
	if isvector(v) then return { x = v.x, y = v.y, z = v.z } end
	if isangle(v) then return { pitch = v.p, yaw = v.y, roll = v.r } end
	if t == "table" and depth < 1 then
		local o, n = {}, 0
		for k, val in pairs(v) do
			n = n + 1
			if n > 12 then o["..."] = "truncated"; break end
			o[tostring(k)] = summarize(val, depth + 1)
		end
		return o
	end
	return tostring(v)
end

----------------------------------------------------------------------
-- Lua errors: structured capture via OnLuaError (client realm), de-duplicated
-- by message + top frame. `seq` bumps on every occurrence so "since" queries
-- also see repeats of a known error.
----------------------------------------------------------------------
MCP.errors = MCP.errors or { seq = 0, list = {}, byKey = {} }
local MAX_ERRORS = 200

local function recordError(err, realm, stack, addonName, addonId)
	local E = MCP.errors
	E.seq = E.seq + 1
	local frames = {}
	if type(stack) == "table" then
		for i, f in ipairs(stack) do
			if i > 16 then break end
			frames[#frames + 1] = { file = f.File, line = f.Line, fn = (f.Function ~= "" and f.Function) or nil }
		end
	end
	local msg = tostring(err)
	-- Location = first Lua frame (skip "[C]" frames such as `error` itself).
	local top
	for _, f in ipairs(frames) do
		if f.file and f.file ~= "[C]" then top = f; break end
	end
	top = top or frames[1]
	local key = msg .. "|" .. (top and (tostring(top.file) .. ":" .. tostring(top.line)) or "")
	local e = E.byKey[key]
	if e then
		e.count = e.count + 1
		e.seq = E.seq
		e.lastTime = os.time()
		return
	end
	e = { seq = E.seq, firstSeq = E.seq, message = msg, realm = realm, stack = frames,
		count = 1, firstTime = os.time(), lastTime = os.time() }
	if top then e.file, e.line = top.file, top.line end
	if addonName then e.addon = addonName end
	if addonId and addonId ~= "" then e.addonId = addonId end
	E.byKey[key] = e
	E.list[#E.list + 1] = e
	if #E.list > MAX_ERRORS then
		local old = table.remove(E.list, 1)
		for k, v in pairs(E.byKey) do if v == old then E.byKey[k] = nil; break end end
	end
end

hook.Add("OnLuaError", "gmod_mcp_errors", function(err, realm, stack, addonName, addonId)
	pcall(recordError, err, realm, stack, addonName, addonId)
end)

-- args: { since?, epoch?, limit?, clear? }. If `epoch` differs from this state's,
-- `since` is ignored (the state was recreated and seqs restarted).
function MCP.LuaErrors(args)
	local E = MCP.errors
	local since = (args.epoch == nil or args.epoch == MCP.epoch) and (args.since or 0) or 0
	local limit = math.Clamp(math.floor(args.limit or 20), 1, MAX_ERRORS)
	local out = {}
	for _, e in ipairs(E.list) do
		if e.seq > since then out[#out + 1] = e end
	end
	table.sort(out, function(a, b) return a.seq < b.seq end)
	local total = #out
	while #out > limit do table.remove(out, 1) end -- keep the most recent
	local res = { epoch = MCP.epoch, nextSeq = E.seq, total = total, errors = out }
	if args.clear then MCP.errors = { seq = E.seq, list = {}, byKey = {} } end
	return res
end

----------------------------------------------------------------------
-- Hook events: record the calls of watched hooks into a ring buffer so the
-- server can wait for them (gmod_wait_for with `hook`).
----------------------------------------------------------------------
MCP.events = MCP.events or { seq = 0, list = {}, watched = {} }
local MAX_EVENTS = 500

local function watchHook(name)
	local EV = MCP.events
	if EV.watched[name] then return end
	EV.watched[name] = true
	hook.Add(name, "gmod_mcp_watch", function(...)
		local n = select("#", ...)
		local a = {}
		for i = 1, math.min(n, 8) do a[i] = summarize((select(i, ...))) end
		EV.seq = EV.seq + 1
		EV.list[#EV.list + 1] = { seq = EV.seq, hook = name, args = a, time = CurTime() }
		if #EV.list > MAX_EVENTS then table.remove(EV.list, 1) end
		-- return nothing: never alter the hook's outcome
	end)
end

-- args: { hook, since?, epoch? } -> { epoch, seq, events[] }. Registers the watch
-- if needed (idempotent), so the first call doubles as "start watching".
function MCP.Events(args)
	local name = tostring(args.hook or "")
	if name == "" then return { error = "hook required" } end
	watchHook(name)
	local EV = MCP.events
	local since = (args.epoch == MCP.epoch) and (args.since or 0) or nil
	local out = {}
	if since then
		for _, ev in ipairs(EV.list) do
			if ev.seq > since and ev.hook == name then out[#out + 1] = ev end
		end
	end
	return { epoch = MCP.epoch, seq = EV.seq, events = out }
end

----------------------------------------------------------------------
-- Condition evaluation for gmod_wait_for: an expression ("IsValid(LocalPlayer())")
-- or a chunk with its own `return`. Compiled once per string.
----------------------------------------------------------------------
local condCache = {}
function MCP.EvalCondition(args)
	local expr = tostring(args.expr or "")
	if expr == "" then return { error = "expr required" } end
	local fn = condCache[expr]
	if not fn then
		local f = CompileString("return (" .. expr .. ")", "gmod_mcp_wait", false)
		if type(f) ~= "function" then f = CompileString(expr, "gmod_mcp_wait", false) end
		if type(f) ~= "function" then return { error = "compile error: " .. tostring(f) } end
		fn = f
		condCache[expr] = fn
	end
	local ok, v = pcall(fn)
	if not ok then return { truthy = false, error = tostring(v) } end
	return { truthy = v ~= nil and v ~= false, value = summarize(v) }
end

----------------------------------------------------------------------
-- Dispatcher (single native entry point)
----------------------------------------------------------------------
-- Wire method name (snake_case, sent by the native dispatcher) -> MCP function.
local METHODS = {
	lua_run = MCP.Run,
	get_state = MCP.GetState,
	input_set = MCP.SetInput,
	input_clear = MCP.ClearInput,
	look = MCP.Look,
	press = MCP.Press,
	dump_vgui = MCP.DumpVGUI,
	cursor = MCP.Cursor,
	type = MCP.Type,
	screenshot = MCP.Screenshot,
	screenshot_panel = MCP.ScreenshotPanel,
	lua_errors = MCP.LuaErrors,
	events = MCP.Events,
	eval_condition = MCP.EvalCondition,
}

function MCP._dispatch(name, argJson)
	local fn = METHODS[name] or MCP[name]
	if type(fn) ~= "function" then
		return util.TableToJSON({ ok = false, error = "no such method: " .. tostring(name) })
	end

	local args = {}
	if argJson and argJson ~= "" then
		local decoded = util.JSONToTable(argJson)
		if type(decoded) == "table" then args = decoded end
	end

	local ok, res = pcall(fn, args)
	if not ok then
		return util.TableToJSON({ ok = false, error = tostring(res) })
	end

	-- Encode the envelope; guard against non-serialisable results.
	local enc_ok, encoded = pcall(util.TableToJSON, { ok = true, result = res or {} })
	if not enc_ok then
		return util.TableToJSON({ ok = false, error = "result not serialisable: " .. tostring(encoded) })
	end
	return encoded
end

MsgN("[gmod-mcp] bootstrap agent loaded (v" .. MCP.version .. ")")
