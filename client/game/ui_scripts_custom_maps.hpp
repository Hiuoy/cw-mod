#pragma once
// The built-in CUSTOM MAPS tab script (ui_scripts.hpp): Zombies Private gets a tab listing cw-mod/maps.
// A cw-mod/ui_scripts/custom_maps.lua replaces it, which is how it is edited live: copy the text between
// the delimiters there, edit until it works, then paste it back here.

namespace Client::Game::UiScripts {
	inline constexpr char kCustomMapsLua[] = R"lua(-- cw-mod: a CUSTOM MAPS tab in Zombies Private, beside CORE and ONSLAUGHT, listing the maps in cw-mod/maps.
--
-- The tab reuses the CORE page (a playlist list) with its own list: one entry per custom map, a copy of its
-- base map's private playlist card with the map's title and description. Picking one selects that playlist,
-- so the lobby is the stock one for the base map, and tells the DLL which custom map to serve when the base
-- map loads. Picking any stock card, or anything else setting another playlist, tells it "none": stock maps
-- stay stock.
--
-- To the DLL through Engine.PrintInfo: "cw-mod map <id> <playlist>" ("cw-mod map" = none), "cw-mod log <text>".
-- From the DLL (its prelude, run first): CWMOD.maps = { { id, title, base, description, playlist }, ... },
-- CWMOD.active and CWMOD.activePlaylist (the pick it holds).
-- Safe to run again (live reload): the originals it wraps are kept in CWMOD.

local dsu = CoD[@"0x20D1815E1D95F30"]             -- the datasource list utility
local director = CoD[@"0xA568193B170CB53"]        -- DirectorUtility
local CORE_PAGE = @"0xA0B1B80B3BE3F08"            -- the CORE page widget
if type(dsu) ~= "table" or type(director) ~= "table" or type(CoD[CORE_PAGE]) ~= "table" then
	return "this LUI state has no Zombies Private menu: tab not installed"
end

-- LUI's _G refuses new globals (a metatable guard), so CWMOD is created raw.
rawset(_G, "CWMOD", rawget(_G, "CWMOD") or {})
local S = rawget(_G, "CWMOD")
S.maps = S.maps or {}
S.active = S.active or ""

local TAB_LIST = @"0x82722A096F4F71"              -- DirectorPrivateZM's tab bar
local CORE_LIST = @"0xDC6DDB5449131CF"            -- the CORE page's playlists (private, party size > 1)
local OUR_LIST = @"CwModCustomMapsList"
local OUR_PAGE = @"CwModCustomMapsPage"
local TAB_NAME = @"0x55F116BF695C8F6"             -- tab item models: label, page widget, tab id
local TAB_FRAME = @"0xF4363F5180BD83D"
local TAB_ID = @"0x1567AAA3FA1099E"
local SELECT_PLAYLIST = @"0x8776F086512EAA0"      -- DirectorUtility: the focused card's playlist -> the lobby
local SET_PLAYLIST = @"0xCE25A90DC553200"         -- Engine: set the lobby playlist
local MODEL_VALUE = @"0x11E8C80610BAA5E"          -- CoD helper: (model, name) -> that submodel's value

-- The private playlist a custom map starts through when map.json names none, per base map (TU35 playlists).
local BASE_PLAYLIST = { zm_silver = 151 }

-- Engine natives are keyed by name hash: the game's bytecode calls Engine[0x28C5711DAACC99F4] (fnv1a64 of
-- "printinfo"), the native the DLL hooks. Engine.PrintInfo by string finds some other function, and a
-- text sent through it never reaches the DLL.
local PRINT_INFO = Engine[@"PrintInfo"] or Engine.PrintInfo

local function say(text)
	PRINT_INFO(0, "cw-mod " .. text)
end

local function copy(t)
	local c = {}
	for k, v in pairs(t) do
		c[k] = v
	end
	return c
end

-- The tab: whatever the stock filter returns, plus ours while there is a map to list.
local filters = dsu[@"0x7212BB34F998752"]
S.tabFilter = S.tabFilter or filters[TAB_LIST]
filters[TAB_LIST] = function(list, controller, element, extra)
	local out = list
	if S.tabFilter then
		local ok, result = pcall(S.tabFilter, list, controller, element, extra)
		if ok and type(result) == "table" then
			out = result
		elseif not ok then
			say("log the stock tab filter raised: " .. tostring(result))
		end
	end
	if #S.maps > 0 then
		table.insert(out, { models = { [TAB_NAME] = "Custom Maps", [TAB_FRAME] = OUR_PAGE, [TAB_ID] = "custom" } })
	end
	return out
end

-- The page: the CORE page, listing ours.
CoD[OUR_PAGE] = CoD[OUR_PAGE] or {}
CoD[OUR_PAGE].new = function(menu, controller, ...)
	local page = CoD[CORE_PAGE].new(menu, controller, ...)
	page.FeaturedGametypes:setDataSource(OUR_LIST)
	return page
end

-- The list: per custom map, its base playlist's card under the map's own title and description.
dsu[@"0x7610F2B168689A5"](OUR_LIST, function(controller, element)
	local items = {}
	local ok, err = pcall(function()
		local core = dsu[@"0xDFFA896249A893C"][CORE_LIST](controller, element)
		for _, map in ipairs(S.maps) do
			local playlist = map.playlist or BASE_PLAYLIST[map.base]
			local card
			for _, item in ipairs(core) do
				if item.models and item.models.playlist == playlist then
					card = item
					break
				end
			end
			if card then
				local models = copy(card.models)
				models.name = map.title
				models.mapsString = map.title
				models.playlistDesc = map.description or ""
				models.cwmodMap = map.id
				table.insert(items, { models = models, properties = { action = models.action, actionParam = models } })
			else
				say("log " .. tostring(map.id) .. ": no private playlist " .. tostring(playlist) .. " for base map "
					.. tostring(map.base) .. " in this lobby")
			end
		end
	end)
	if not ok then
		say("log the custom maps list raised: " .. tostring(err))
	end
	return items
end)

-- The pick. A card of ours names its map in its model; a stock card names none. The playlist it sets says
-- which, then: set from a card, it reports the card's map; set any other way, it reports "none" only when it
-- is another playlist than the pick's (the lobby may set the same one again).
local function report(id, playlist)
	if id ~= S.active then
		S.active = id
		S.activePlaylist = playlist
		say(id == "" and "map" or ("map " .. id .. " " .. tostring(playlist)))
	end
end

local function pickedMap(element)
	local getModel = element[@"GetModel"] or element.GetModel
	local model = getModel and getModel(element)
	local id = model and CoD[MODEL_VALUE](model, "cwmodMap")
	return type(id) == "string" and id or ""
end

S.selectPlaylist = S.selectPlaylist or director[SELECT_PLAYLIST]
director[SELECT_PLAYLIST] = function(menu, element, controller, ...)
	local ok, id = pcall(pickedMap, element)
	if not ok then
		say("log could not read the picked card: " .. tostring(id))
	end
	S.picked = ok and id or ""
	return S.selectPlaylist(menu, element, controller, ...)
end

-- Engine ignores plain assignment (a proxy); a raw key on it is found before the proxy's own lookup.
S.setPlaylist = S.setPlaylist or Engine[SET_PLAYLIST]
rawset(Engine, SET_PLAYLIST, function(playlist, ...)
	if S.picked then
		report(S.picked, playlist)
		S.picked = nil
	elseif S.active ~= "" and playlist ~= S.activePlaylist then
		report("", nil)
	end
	return S.setPlaylist(playlist, ...)
end)

return "tab ready: " .. #S.maps .. " map(s), active: " .. (S.active ~= "" and S.active or "none")
	.. (Engine[SET_PLAYLIST] == S.setPlaylist and ", but Engine is read-only" or "")
)lua";
}
