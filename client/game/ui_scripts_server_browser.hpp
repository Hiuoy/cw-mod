#pragma once
// The built-in SERVER BROWSER script (ui_scripts.hpp): the Zombies main screen gets a button that opens a
// menu of our own listing the LAN games lan_browser.cpp hears, the overlay's Server Browser tab as a game
// menu. A cw-mod/ui_scripts/server_browser.lua replaces it, which is how it is edited live: copy the text
// between the delimiters there, edit until it works, then paste it back here.
//
// Every name it calls was read from the game's own bytecode (tools/lua_disasm.py), not from the decompiled
// text: the decompiler prints some hashed keys by name, so its text cannot tell `element:GetModel()` (a
// hashed native) from `element:setClass()` (a plain string key). The templates are DirectorPrivateZM (the
// menu), LocalServerRowList and LocalServerRow (the game's own, unfed, LAN list and its row).
//
// In pieces because MSVC caps one string literal at 16380 bytes (C2026); adjacent literals are joined.

namespace Client::Game::UiScripts {
	inline constexpr char kServerBrowserLua[] = R"lua(-- cw-mod: a SERVER BROWSER button on the Zombies main screen, below SOLO. It opens a menu of our own that
-- lists the LAN games this PC hears, the overlay's Server Browser tab as a game menu: host, map, mode,
-- players, state and ping per game, the details of the one under the selection, and Join.
--
-- The DLL owns the data and the join; this script only draws. While the menu is open the DLL runs a chunk
-- that sets CWMOD.browser.hosts / status / advert / join* and bumps CWMOD.browser.serial, whenever any of
-- it changed. The menu's own timer picks that up, so its elements only ever change inside LUI's update.
--
-- To the DLL through Engine.PrintInfo: "cw-mod browser open", "cw-mod browser close",
-- "cw-mod browser join <host xuid, hex>", "cw-mod log <text>".
-- Safe to run again (live reload): the list function it wraps is kept in CWMOD.browser.

local dsu = CoD[@"DataSourceUtility"]
local base = CoD[@"BaseUtility"]
local Menu = CoD[@"Menu"]
local ZM_MODES = @"0x3A8E53F912118C0E"             -- the Zombies main screen's mode buttons (a list function)
local PRIVATE_MENU = @"0x39E283FFCDB06CE1"         -- DirectorPrivateZM, what the PRIVATE button opens
local OUR_MENU = @"CwModServerBrowser"
local OUR_ROW = @"CwModServerBrowserRow"
local OUR_LIST = @"CwModServerBrowserList"
local DEFAULT_STATE = @"DefaultState"
local lists = type(dsu) == "table" and dsu[@"0x2DFFA896249A893C"] or nil

-- LUI's _G refuses new globals (a metatable guard), so CWMOD is created raw.
rawset(_G, "CWMOD", rawget(_G, "CWMOD") or {})
local S = rawget(_G, "CWMOD")
S.browser = S.browser or {}
local B = S.browser
B.hosts = B.hosts or {}
B.models = B.models or {}
B.serial = B.serial or 0

if type(base) ~= "table" or type(Menu) ~= "table" or type(lists) ~= "table"
	or type(B.modes or lists[ZM_MODES]) ~= "function" then
	return "this LUI state has no Zombies main screen: server browser not installed"
end

local okEnums, A_BUTTON, B_BUTTON, ALIGN = pcall(function()
	local buttons = Enum[@"LUIButton"]
	return buttons[@"LUI_KEY_XBA_PSCROSS"], buttons[@"LUI_KEY_XBB_PSCIRCLE"], Enum[@"LUIAlignment"]
end)
if not okEnums or A_BUTTON == nil or B_BUTTON == nil or ALIGN == nil then
	return "this LUI state has no button or alignment enums: server browser not installed"
end

-- Engine natives are keyed by name hash; Engine.PrintInfo by string is another function (custom_maps.lua).
local PRINT_INFO = Engine[@"PrintInfo"] or Engine.PrintInfo

local function say(text)
	PRINT_INFO(0, "cw-mod " .. text)
end

-- Runs fn. A Lua error in it costs one log line, not the menu: an error LUI itself catches ends the process
-- on a LAN or offline boot.
local failures = {}
local function guard(what, fn, ...)
	local ok, result = pcall(fn, ...)
	if ok then
		return true, result
	end
	local count = (failures[what] or 0) + 1
	failures[what] = count
	if count == 1 or count == 10 or count == 100 then
		say("log server browser, " .. what .. " (x" .. count .. "): " .. tostring(result))
	end
	return false
end

-- fn as a function to hand to stock code: its first result, or nothing when it raised.
local function guarded(what, fn)
	return function(...)
		local _, result = guard(what, fn, ...)
		return result
	end
end

-- One game's fields: the name the DLL sends each under, and the model it is shown from.
local K = {
	xuid = @"cwmod_xuid", host = @"cwmod_host", map = @"cwmod_map", mode = @"cwmod_mode",
	players = @"cwmod_players", state = @"cwmod_state", ping = @"cwmod_ping",
	address = @"cwmod_address", pingLong = @"cwmod_ping_long", mapLong = @"cwmod_map_long",
	gametype = @"cwmod_gametype", modeLong = @"cwmod_mode_long", playerList = @"cwmod_player_list",
	hostId = @"cwmod_host_id", build = @"cwmod_build", mod = @"cwmod_mod",
}

local function value(model, key)
	local child = model and model[key]
	if child then
		return child:get()
	end
end

-- One model per game for as long as this UI lives, updated in place: a row redraws only the text that
-- changed, and the list is rebuilt only when the set of games did.
local function hostModel(xuid)
	local model = B.models[xuid]
	if not model then
		B.root = B.root or Engine[@"GetGlobalModel"]():create(@"cwmod_server_browser")
		model = B.root:create(xuid)
		B.models[xuid] = model
	end
	return model
end

local function fill(model, host)
	for name, key in pairs(K) do
		model:create(key):set(host[name] or "")
	end
end

-- The list: the games in the DLL's order (by host name). The game that had the selection keeps it.
dsu[@"0x77610F2B168689A5"](OUR_LIST, function(controller, element)
	local items = {}
	guard("the list", function()
		for _, host in ipairs(B.hosts) do
			local model = hostModel(host.xuid)
			fill(model, host)
			table.insert(items, { model = model, properties = { selectIndex = host.xuid == B.selected } })
		end
	end)
	return items
end)

local WHITE = { 0.92, 0.91, 0.87 }
local DIM = { 0.6, 0.6, 0.58 }
local DARK = { 0.06, 0.07, 0.09 }
local AMBER = { 1, 0.6, 0.2 }
local FONT = "helveticaneueltpro_roman"
local FONT_BOLD = "notosans_bold"

local LIST_LEFT, LIST_TOP = 96, 214
local ROW_WIDTH, ROW_HEIGHT, ROW_GAP, ROWS = 1100, 37, 7, 12
local DETAILS_LEFT, DETAILS_TOP, DETAILS_WIDTH = 1246, 172, 578

-- A row's cells: field, left, right, font, column title.
local COLUMNS = {
	{ "host", 18, 330, FONT_BOLD, "HOST" },
	{ "map", 340, 560, FONT, "MAP" },
	{ "mode", 570, 790, FONT, "MODE" },
	{ "players", 800, 890, FONT, "PLAYERS" },
	{ "state", 900, 1000, FONT, "STATE" },
	{ "ping", 1010, 1090, FONT, "PING" },
}

-- The details of the game under the selection: caption, field.
local DETAILS = {
	{ "ADDRESS", "address" }, { "PING", "pingLong" }, { "MAP", "mapLong" }, { "GAMETYPE", "gametype" },
	{ "MODE", "modeLong" }, { "PLAYERS", "playerList" }, { "HOST", "hostId" }, { "GAME", "build" },
	{ "CW-MOD", "mod" },
}

-- LocalServerRow's four alignment calls for a cell: left, vertically centred, and two it gives by hash only.
local TEXT_ALIGN = { @"LUI_ALIGNMENT_LEFT", @"LUI_ALIGNMENT_MIDDLE", @"0x5A2DDA596FE68788", @"0x5778D9182440C257" }

local function tint(element, color)
	element[@"setRGB"](element, color[1], color[2], color[3])
end

-- One line of text, placed from its parent's top left corner.
local function newText(left, right, top, bottom, height, font)
	local text = LUI.UIText.new(0, 0, left, right, 0, 0, top, bottom)
	text[@"setFontHeight"](text, height)
	text[@"setTTF"](text, font)
	for _, name in ipairs(TEXT_ALIGN) do
		local alignment = ALIGN[name]
		if alignment ~= nil then
			text[@"setAlignment"](text, alignment)
		end
	end
	return text
end

local function setText(element, text)
	if element then
		element[@"setText"](element, text or "")
	end
end

local function show(element, visible)
	if element then
		element[@"setAlpha"](element, visible and 1 or 0)
	end
end

)lua" R"lua(-- The row: six cells shown from the game's model, light on dark, inverted under the selection. A list
-- only selects a widget that has a Focus clip (LUI.UIList.isWidgetSelectable).
CoD[OUR_ROW] = InheritFrom(LUI.UIElement)
local Row = CoD[OUR_ROW]
Row.__defaultWidth = ROW_WIDTH
Row.__defaultHeight = ROW_HEIGHT

Row.new = function(menu, controller, leftAnchor, rightAnchor, left, right, topAnchor, bottomAnchor, top, bottom)
	local self = LUI.UIElement.new(leftAnchor, rightAnchor, left, right, topAnchor, bottomAnchor, top, bottom)
	self:setClass(CoD[OUR_ROW])
	self.soundSet = "default"
	self.cells = {}
	guard("a row", function()
		local highlight = LUI.UIImage.new(0, 1, 0, 0, 0, 1, 0, 0)
		tint(highlight, WHITE)
		highlight[@"setAlpha"](highlight, 0)
		self[@"addElement"](self, highlight)
		self.Highlight = highlight
		for _, column in ipairs(COLUMNS) do
			local text = newText(column[2], column[3], 2, ROW_HEIGHT - 2, 22, column[4])
			tint(text, WHITE)
			self[@"addElement"](self, text)
			table.insert(self.cells, text)
			self:linkToElementModel(self, K[column[1]], true, guarded("a row's text", function(model)
				local v = model:get()
				if v ~= nil then
					text[@"setText"](text, v)
				end
			end), false)
		end
	end)
	menu:addElementToPendingUpdateStateList(self)
	return self
end

Row.__resetProperties = function(self)
	if self.Highlight then
		self.Highlight[@"completeAnimation"](self.Highlight)
		self.Highlight[@"setAlpha"](self.Highlight, 0)
	end
	for _, text in ipairs(self.cells or {}) do
		text[@"completeAnimation"](text)
		tint(text, WHITE)
	end
end

Row.__clipsPerState = {
	[DEFAULT_STATE] = {
		DefaultClip = guarded("a row's default clip", function(self, controller)
			self:__resetProperties()
			self:setupElementClipCounter(0)
		end),
		Focus = guarded("a row's focus clip", function(self, controller)
			self:__resetProperties()
			self:setupElementClipCounter(0)
			if self.Highlight then
				self.Highlight[@"setAlpha"](self.Highlight, 1)
			end
			for _, text in ipairs(self.cells or {}) do
				tint(text, DARK)
			end
		end),
	},
}
setmetatable(Row.__clipsPerState, LUI.UIElement.__tempStateMetaTable)

-- What the DLL last sent, onto the open menu. Runs from the menu's own timer.
local function refresh(menu, controller)
	if menu.cwmodJoinBusy then
		menu.cwmodJoinBusy = menu.cwmodJoinBusy > 1 and menu.cwmodJoinBusy - 1 or nil
	end
	if menu.cwmodSerial == B.serial then
		return
	end
	menu.cwmodSerial = B.serial

	setText(menu.Status, B.status)
	setText(menu.Advert, B.advert)

	local order = {}
	for index, host in ipairs(B.hosts) do
		fill(hostModel(host.xuid), host)
		order[index] = host.xuid
	end
	order = table.concat(order, " ")
	local list = menu.Servers
	if list and order ~= menu.cwmodOrder then
		menu.cwmodOrder = order
		local active = list.activeWidget
		B.selected = active and value(active[@"GetModel"](active), K.xuid) or nil
		list:updateDataSource()
	end
	show(menu.Empty, #B.hosts == 0)
	show(menu.Details, #B.hosts > 0)

	if B.joinSerial ~= menu.cwmodJoinSerial then
		menu.cwmodJoinSerial = B.joinSerial
		menu.cwmodJoinBusy = nil
		if B.joinOk then
			-- The join is under way: back to the main screen, where the game shows the lobby it lands in.
			-- This runs inside the timer's own tick, which re-arms the timer afterwards: not a closed one.
			if menu.RefreshTimer then
				menu.RefreshTimer.reset = function() end
			end
			base[@"goBack"](menu, controller)
			return
		end
		setText(menu.JoinStatus, B.joinText)
	end
end

)lua" R"lua(-- The menu, after DirectorPrivateZM: a CoD.Menu opened as an overlay over the main screen, which the
-- game hides meanwhile. Each part is built on its own: one that fails is a log line and a gap.
CoD[OUR_MENU] = InheritFrom(Menu)

LUI.createMenu[OUR_MENU] = function(controller, params)
	local self = Menu.NewForUIEditor(OUR_MENU, controller, params)
	self:setOwner(controller)
	self[@"setLeftRight"](self, 0, 1, 0, 0)
	self[@"setTopBottom"](self, 0, 1, 0, 0)
	self:setClass(CoD[OUR_MENU])
	self.soundSet = "default"
	self.anyChildUsesUpdateState = true
	self:playSound("menu_open", controller)

	-- From here the DLL sends the list, and a Lua error in this menu no longer ends the process.
	say("browser open")
	B.hosts = {}
	B.status = ""
	B.advert = ""
	B.selected = nil
	B.joinOk = false                 -- an earlier menu's join is not this one's
	B.joinText = ""
	B.serial = B.serial + 1
	self.cwmodJoinSerial = B.joinSerial

	-- Back first: whatever else fails, the menu can be left.
	guard("the back button", function()
		self:AddGamepadButtonCallbackFunction(self, controller, B_BUTTON, guarded("back", function(element, menu, controller_)
			base[@"goBack"](self, controller_)
			return true
		end), guarded("the back prompt", function(element, menu, controller_)
			Menu.SetGamepadButtonLabel(menu, B_BUTTON, @"MENU/BACK", nil)
			return true
		end), false)
	end)

	local function label(name, left, right, top, bottom, height, font, color, text)
		local element = newText(left, right, top, bottom, height, font)
		tint(element, color)
		element[@"setText"](element, text)
		self[@"addElement"](self, element)
		self[name] = element
		return element
	end

	guard("the backdrop", function()
		local backdrop = LUI.UIImage.new(0, 1, 0, 0, 0, 1, 0, 0)
		backdrop[@"setRGB"](backdrop, 0.05, 0.055, 0.065)
		backdrop[@"setAlpha"](backdrop, 0.94)
		self[@"addElement"](self, backdrop)
		self.Backdrop = backdrop
	end)

	guard("the titles", function()
		label("Title", LIST_LEFT, 1200, 52, 114, 58, "maxima_bol", WHITE, "SERVER BROWSER")
		label("Subtitle", LIST_LEFT, 1200, 116, 146, 22, FONT, DIM, "LAN games heard on this network")
		for _, column in ipairs(COLUMNS) do
			label("Column_" .. column[1], LIST_LEFT + column[2], LIST_LEFT + column[3], LIST_TOP - 38, LIST_TOP - 8, 18, FONT,
				DIM, column[5])
		end
		label("Empty", LIST_LEFT + 18, LIST_LEFT + ROW_WIDTH, LIST_TOP + 2, LIST_TOP + 35, 22, FONT, DIM,
			"No LAN games heard yet.")
		local bottom = LIST_TOP + ROWS * (ROW_HEIGHT + ROW_GAP)
		label("Status", LIST_LEFT + 18, LIST_LEFT + ROW_WIDTH, bottom + 24, bottom + 52, 20, FONT, DIM, "")
		label("Advert", LIST_LEFT + 18, LIST_LEFT + ROW_WIDTH, bottom + 56, bottom + 84, 20, FONT, DIM, "")
		label("JoinStatus", LIST_LEFT + 18, 1824, bottom + 88, bottom + 116, 20, FONT, AMBER, "")
	end)

	-- The list, after LocalServerRowList.
	guard("the list", function()
		local list = LUI.UIList.new(self, controller, ROW_GAP, ROW_GAP, 0, nil, false, false, false, false)
		list[@"setLeftRight"](list, 0, 0, LIST_LEFT, LIST_LEFT + ROW_WIDTH)
		list[@"setTopBottom"](list, 0, 0, LIST_TOP, LIST_TOP + ROWS * (ROW_HEIGHT + ROW_GAP))
		list:setWidgetType(OUR_ROW)
		list:setVerticalCount(ROWS)
		list:setWSpacing(ROW_GAP)
		list:setHSpacing(ROW_GAP)
		list[@"setAlignment"](list, ALIGN[@"LUI_ALIGNMENT_LEFT"])
		list[@"setAlignment"](list, ALIGN[@"LUI_ALIGNMENT_TOP"])
		list:setDataSource(OUR_LIST)

		-- This replaces the list's own gain_focus handler, so it runs that first, the way the stock one does.
		list:registerEventHandler("gain_focus", function(element, event)
			local result = nil
			if element.gainFocus then
				result = element.gainFocus(element, event)
			elseif element.super.gainFocus then
				result = element.super.gainFocus(element, event)
			end
			pcall(Menu.UpdateButtonShownState, element, self, controller, A_BUTTON)
			return result
		end)

		-- A, Enter or a click on a row: element is that row. Its prompt reads SELECT: the footer localizes the
		-- label, a hash without a localize entry ends the process, and this one the game's own menus use.
		local join = guarded("join", function(element, menu, controller_)
			local getModel = element and element[@"GetModel"]
			local model = getModel and getModel(element)
			local xuid = value(model, K.xuid)
			if type(xuid) == "string" and xuid ~= "" and not self.cwmodJoinBusy then
				self.cwmodJoinBusy = 90      -- timer ticks: about three seconds without an answer frees it
				setText(self.JoinStatus, "Joining " .. tostring(value(model, K.host) or "the game") .. "...")
				say("browser join " .. xuid)
			end
			return true
		end)
		self:AddGamepadButtonCallbackFunction(list, controller, A_BUTTON, join, guarded("the join prompt", function(element, menu, controller_)
			Menu.SetGamepadButtonLabel(menu, A_BUTTON, @"MENU/SELECT", nil)
			return true
		end), false)
		self:AddPCKeyCallbackFunction(list, controller, "ui_confirm", A_BUTTON, join, guarded("the join key prompt", function(element, menu, controller_)
			Menu.SetPCKeyLabel(menu, "ui_confirm", A_BUTTON, @"MENU/SELECT", nil)
			return true
		end), false)

		self[@"addElement"](self, list)
		self.Servers = list
		list.id = "Servers"
		self.__defaultFocus = list
	end)

	-- The details follow the list's own model, which a list keeps at its selected item's.
	guard("the details", function()
		local list = self.Servers
		if not list then
			return
		end
		local details = LUI.UIElement.new(0, 0, DETAILS_LEFT, DETAILS_LEFT + DETAILS_WIDTH, 0, 0, DETAILS_TOP,
			DETAILS_TOP + 60 + #DETAILS * 32)
		details[@"setAlpha"](details, 0)
		self[@"addElement"](self, details)
		self.Details = details

		local function linked(left, right, top, bottom, height, font, key)
			local text = newText(left, right, top, bottom, height, font)
			tint(text, WHITE)
			details[@"addElement"](details, text)
			text:linkToElementModel(list, key, true, guarded("a detail", function(model)
				local v = model:get()
				if v ~= nil then
					text[@"setText"](text, v)
				end
			end), false)
		end

		linked(0, DETAILS_WIDTH, 0, 40, 30, FONT_BOLD, K.host)
		for index, row in ipairs(DETAILS) do
			local top = 22 + index * 32
			local caption = newText(0, 130, top, top + 28, 18, FONT)
			tint(caption, DIM)
			caption[@"setText"](caption, row[1])
			details[@"addElement"](details, caption)
			linked(140, DETAILS_WIDTH, top, top + 28, 20, FONT, K[row[2]])
		end
	end)

	guard("the timer", function()
		local timer = LUI.UITimer.newElementTimer(33, false, guarded("the refresh", function()
			refresh(self, controller)
		end))
		self[@"addElement"](self, timer)
		self.RefreshTimer = timer
	end)

	guard("the close hooks", function()
		LUI.OverrideFunction_CallOriginalFirst(self, "close", guarded("after close", function()
			self:clearSavedState()
		end))
		LUI.OverrideFunction_CallOriginalFirst(self, "setState", guarded("after setState", function(element, controller_)
			self:UpdateAllButtonPrompts(controller_)
		end))
		LUI.OverrideFunction_CallOriginalSecond(self, "close", guarded("before close", function()
			say("browser close")
			if self.RefreshTimer then
				self.RefreshTimer:close()
				self.RefreshTimer = nil
			end
			if self.Servers then
				self.Servers:close()
			end
		end))
	end)

	guard("menu_loaded", function()
		self:processEvent({ name = "menu_loaded", controller = controller })
	end)
	self._showFooter = true

	-- As DirectorPrivateZM does for its own backdrop: the whole screen, not only the 16:9 middle.
	guard("the backdrop's size", function()
		if self.Backdrop then
			base[@"0x569D8EB11060ED48"](self.Backdrop, controller)
		end
	end)
	return self
end

-- The button: the mode list's PRIVATE item under another name, opening our menu instead of DirectorPrivateZM.
-- Its action is PRIVATE's own (nothing while a public match search runs, else the overlay opener).
--
-- The name is in the form Engine.LocalizeHash gives its own results: byte 21, the text, byte 20. The button's
-- widget passes it through that call once more, which returns a string that starts with byte 21 untouched.
-- Any other string it takes for the name of a localize entry, and a missing entry ends the process: UI Error
-- 100004, then the asset lookup's fatal error, which no pcall sees. PRIVATE's two names are the same string.
local BUTTON_NAME = "\021Server Browser\020"
if string.byte(BUTTON_NAME, 1) ~= 21 or string.byte(BUTTON_NAME, #BUTTON_NAME) ~= 20 then
	return "this Lua does not read \\021 as byte 21: no main screen button (it would end the process)"
end
B.modes = B.modes or lists[ZM_MODES]
lists[ZM_MODES] = function(...)
	local items = B.modes(...)
	guard("the main screen button", function()
		if type(items) ~= "table" then
			return
		end
		local private = nil
		for _, item in ipairs(items) do
			local properties = type(item) == "table" and item.properties
			if properties and properties.actionParam == OUR_MENU then
				return
			elseif properties and properties.actionParam == PRIVATE_MENU then
				private = item
			end
		end
		if not private or type(private.models) ~= "table" then
			if not B.noPrivate then
				B.noPrivate = true
				say("log server browser: the main screen lists no PRIVATE button to model ours on; none added")
			end
			return
		end
		local models = {}
		for key, v in pairs(private.models) do
			models[key] = v
		end
		models.displayName = BUTTON_NAME
		models.hashIdentifier = BUTTON_NAME
		table.insert(items, { models = models, properties = { action = private.properties.action, actionParam = OUR_MENU } })
	end)
	return items
end

return "main screen button and menu ready"
)lua";
}
