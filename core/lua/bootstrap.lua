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
	local ply = LocalPlayer()
	if not IsValid(ply) then return { error = "no local player (in menu?)" } end

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

function MCP.Cursor(args)
	local x, y = args.x, args.y
	if args.panel then
		local found = findPanel(string.lower(tostring(args.panel)))
		if not found then return { error = "panel not found: " .. tostring(args.panel) } end
		local px, py = found:LocalToScreen(0, 0)
		local w, h = found:GetSize()
		x, y = px + w / 2, py + h / 2
	end
	if not x or not y then return { error = "need x,y or panel" } end
	x, y = math.floor(x), math.floor(y)

	input.SetCursorPos(x, y)
	if gui.InternalCursorMoved then gui.InternalCursorMoved(x, y) end

	local result = { x = x, y = y, cursorVisible = vgui.CursorVisible() }
	if args.click then
		local mb = (args.button == "right") and MOUSE_RIGHT or MOUSE_LEFT
		gui.InternalMousePressed(mb)
		gui.InternalMouseReleased(mb)
		result.clicked = args.button or "left"
	end
	return result
end

function MCP.Type(args)
	local text = tostring(args.text or "")
	local target
	if args.panel then
		target = findPanel(string.lower(tostring(args.panel)))
	else
		target = vgui.GetKeyboardFocus()
	end
	if not IsValid(target) then return { error = "no target panel (nothing focused?)" } end
	if not target.SetText then return { error = "target has no SetText: " .. (target:GetClassName() or "?") } end

	local newText = text
	if args.append and target.GetText then newText = (target:GetText() or "") .. text end
	target:SetText(newText)
	if target.SetCaretPos then pcall(target.SetCaretPos, target, #newText) end
	if target.OnTextChanged then pcall(target.OnTextChanged, target) end
	if args.enter and target.OnEnter then pcall(target.OnEnter, target) end

	return { typed = text, into = target:GetClassName() }
end

function MCP.ConCommand(args)
	local cmd = tostring(args.command or "")
	if cmd == "" then return { error = "empty command" } end
	local ply = LocalPlayer()
	if IsValid(ply) then ply:ConCommand(cmd) else RunConsoleCommand(unpack(string.Explode(" ", cmd))) end
	return { ran = cmd }
end

----------------------------------------------------------------------
-- Dispatcher (single native entry point)
----------------------------------------------------------------------
function MCP._dispatch(name, argJson)
	local fn = MCP[name]
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
