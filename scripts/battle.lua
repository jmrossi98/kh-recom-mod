LUAGUI_NAME = "recom battle"
LUAGUI_AUTH = "jake"
LUAGUI_DESC = "Battle tweaks: instant reload, reload cost never grows"

-- Steam build only. All addresses are relative to the exe base.
local PLAYER_PTR  = 0x87B390   -- -> player battle object (only valid in battle)
local BATTLE_FLAG = 0x87B858   -- 0 field, 1 battle
local RELOAD_CODE = 0x167B76   -- add [rbx+84],edx  (reload charge), used as a build check

-- player object
local PL_DECK   = 0x3F0        -- -> deck object
local PL_HP     = 0x42C
local PL_MAXHP  = 0x430

-- deck object
local DK_CARDS_TOTAL = 0x6A    -- s16, cards in the deck (without the reload card)
local DK_CARDS_LEFT  = 0x70    -- s16, what the HUD shows
local DK_CUR_INDEX   = 0x74    -- s16
local DK_STATE       = 0x60    -- s32, 1 idle, 2 reloading
local DK_ACTIVE      = 0x90    -- s32, non-zero while a card is selectable
local DK_RELOAD_LVL  = 0x80    -- s16, 1..3, how many "charges" a reload costs
local DK_CHARGE      = 0x84    -- s32, reload charge, 3000 per level
local DK_RELOAD_SPD  = 0x8C    -- s32, 100 by default
local DK_CARD_ARRAY  = 0xB8    -- -> card records, 0x2E0 bytes each, card data at +0x260

local FAST_RELOAD_SPD = 30000

-- Experiment: playing a card does not use it up and does not move to the next
-- card. Patches the tail of the play-card routine, so it applies to enemy decks
-- too. Set to false and press F1 to put the original code back.
local KEEP_CARD = false
local PATCHES = {
    -- dec word [rdi+rax*2+70]   (cards left)
    { addr = 0x16D151, orig = {0x66, 0xFF, 0x4C, 0x47, 0x70} },
    -- call 14016c580            (advance to the next card, main row)
    { addr = 0x16D1B2, orig = {0xE8, 0xC9, 0xF3, 0xFF, 0xFF} },
}

local function bytes_match(addr, want)
    for i = 1, #want do
        if ReadByte(addr + i - 1) ~= want[i] then return false end
    end
    return true
end

local function apply_patches()
    for _, p in ipairs(PATCHES) do
        local nops = {}
        for i = 1, #p.orig do nops[i] = 0x90 end
        if KEEP_CARD then
            if bytes_match(p.addr, p.orig) then
                WriteArray(p.addr, nops)
            elseif not bytes_match(p.addr, nops) then
                ConsolePrint(string.format("battle: unexpected code at %X, not patching", p.addr))
            end
        elseif bytes_match(p.addr, nops) then
            WriteArray(p.addr, p.orig)
        end
    end
    ConsolePrint("battle: keep-card " .. (KEEP_CARD and "on" or "off"))
end

-- native half of the mod, built by build.bat
local DLL_PATH = "C:/Users/jakma/Documents/projects/kh-recom-mod/build/recom_combat.dll"
local native_frame = nil

local ok = false
local frames = 0
local dead_frames = 0          -- frames spent idle with no selectable card

function _OnInit()
    if ReadByte(RELOAD_CODE) == 0x01 and ReadByte(RELOAD_CODE + 1) == 0x93 and ReadByte(RELOAD_CODE + 2) == 0x84 then
        ok = true
        ConsolePrint("battle: steam build recognised")
        apply_patches()
        local fn, err = package.loadlib(DLL_PATH, "rc_frame")
        if fn then
            native_frame = fn
            ConsolePrint("battle: native dll loaded")
        else
            ConsolePrint("battle: native dll not loaded: " .. tostring(err))
        end
    else
        ConsolePrint("battle: unknown build, script disabled")
    end
end

local function deck()
    if ReadByte(BATTLE_FLAG) ~= 1 then return nil end
    local player = ReadLong(PLAYER_PTR)
    if player == 0 then return nil end
    local dk = ReadLong(player + PL_DECK, true)
    if dk == 0 then return nil end
    return dk, player
end

function _OnFrame()
    if not ok then return end
    frames = frames + 1
    if native_frame then native_frame() end
    local dk, player = deck()
    if not dk then return end

    if ReadInt(dk + DK_RELOAD_SPD, true) ~= FAST_RELOAD_SPD then
        WriteInt(dk + DK_RELOAD_SPD, FAST_RELOAD_SPD, true)
    end
    if ReadShort(dk + DK_RELOAD_LVL, true) > 1 then
        WriteShort(dk + DK_RELOAD_LVL, 1, true)
    end

    -- A deck that sits idle with nothing selectable is wedged. Zero the count so
    -- the native side runs a reload, which deals the hand again.
    if ReadInt(dk + DK_STATE, true) == 1 and ReadInt(dk + DK_ACTIVE, true) == 0 then
        dead_frames = dead_frames + 1
        if dead_frames == 180 and native_frame then
            WriteShort(dk + DK_CARDS_LEFT, 0, true)
            ConsolePrint("battle: deck was stuck, forcing a reload")
        end
    else
        dead_frames = 0
    end

    if frames % 300 == 0 then
        ConsolePrint(string.format("battle: hp %d/%d  cards %d/%d",
            ReadInt(player + PL_HP, true), ReadInt(player + PL_MAXHP, true),
            ReadShort(dk + DK_CARDS_LEFT, true), ReadShort(dk + DK_CARDS_TOTAL, true)))
    end
end
