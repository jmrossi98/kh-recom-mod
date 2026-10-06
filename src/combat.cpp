// Native side of the Re:CoM combat mod. Loaded from combat.lua with
// package.loadlib and driven once per game frame through rc_frame().
// Steam build only: every address below is an RVA into the game exe.
#include "overlay.h"

#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace rva {
constexpr uintptr_t kPlayerPtr   = 0x87B390;  // -> player battle object
constexpr uintptr_t kBattleFlag  = 0x87B858;  // 0 field, 1 battle
constexpr uintptr_t kSavePtr     = 0x87CBF8;  // -> save/status block: HP, max HP, ...
constexpr uintptr_t kWorld       = 0x87B862;  // 0xFF in menus
constexpr uintptr_t kLoading     = 0xAF70CA;  // 0xFF while loading
constexpr uintptr_t kGetObject   = 0x2AD300;  // void* (uint id)
constexpr uintptr_t kSetPlayCard = 0x163830;  // CBTL::setPlayCard
constexpr uintptr_t kReloadDone  = 0x16B2B0;  // finishes a reload for a deck
constexpr uintptr_t kPlayCall    = 0x16D083;  // call setPlayCard inside the play-current-card routine
constexpr uintptr_t kSideUpdate  = 0x15EAC0;  // per-side card battle state machine, (battle, side)
constexpr uintptr_t kSideCall1   = 0x161872;  // the two calls to it, side 1 then side 2
constexpr uintptr_t kSideCall2   = 0x16187F;
constexpr uintptr_t kJudgePlay   = 0x164A50;  // can this card be played over the other side's card
// calls in the player's battle input that work the deck directly
constexpr uintptr_t kInSwitchRow = 0x1F8303;  // main row <-> enemy/friend row
constexpr uintptr_t kInJumpFirst = 0x1F8359;  // shortcut to the first marked card
constexpr uintptr_t kInJumpLast  = 0x1F837B;  // shortcut to the second marked card
constexpr uintptr_t kInScroll    = 0x1F850C;  // rotate the deck one card
constexpr uintptr_t kGetFlags    = 0x1B08C0;  // uint (int group, int id)
constexpr uintptr_t kSetFlags    = 0x1B16C0;  // void (int group, int id, uint value)
constexpr uintptr_t kPlayGate    = 0x16CF3E;  // mov eax,2: the bit that blocks the player's plays
constexpr uintptr_t kMapCardGate = 0x1E4818;  // and eax,8: hide test for the map card panel
constexpr uintptr_t kFindTask    = 0x2AD350;  // void* (int id): the object behind a task id
constexpr uintptr_t kSubField    = 0x87B0B0;  // -> CGameSubField; +0x5A0 flags for what is open over the game
constexpr uintptr_t kHudOff      = 0x87B808;  // int, non-zero while the game keeps its own HUD off
constexpr uintptr_t kVtPlayerGauge = 0x5A6638;  // Gauge::CPlayerGauge
constexpr uintptr_t kVtFriendGauge = 0x5A6788;  // Gauge::CFriendGauge
constexpr uintptr_t kVtSeqCtrl   = 0x607340;  // D2::C2DSeqCtrl, what draws a piece of HUD
constexpr uintptr_t kPlaySe      = 0x2AA360;  // uint (uint id, int volume): a sound effect, -1 for full volume
// vtables of what the field's lock-on marker can offer to act on
constexpr uintptr_t kVtDoor      = 0x572478;
constexpr uintptr_t kVtChest     = 0x58B5B0;
constexpr uintptr_t kVtChestPooh = 0x687C10;
constexpr uintptr_t kVtCarry     = 0x56E128;
constexpr uintptr_t kVtCarryTu   = 0x58C720;
constexpr uintptr_t kVtSavePoint = 0x5846C8;
constexpr uintptr_t kVtWarpPoint = 0x58F7E0;
constexpr uintptr_t kVtAvatar    = 0x520828;
}

// Flag word the card code checks all over the place. Bit 2 hides the player's
// card HUD and normally also stops the player playing cards (cutscenes use it).
constexpr int kFlagGroup = 1;
constexpr int kFlagCards = 0xE2;
constexpr uint32_t kFlagHidePlayerGauge = 0x01;   // portrait and HP ring, top left
constexpr uint32_t kFlagHidePlayerCards = 0x02;   // the deck in the corner
constexpr uint32_t kFlagHidePlayerAll   = 0x20;   // every player card, the one in play too
constexpr uint32_t kFlagHideEnemyAll    = 0x40;   // same for the enemy side
constexpr uint32_t kFlagHideCards = kFlagHidePlayerCards | kFlagHidePlayerAll | kFlagHideEnemyAll;
constexpr uint32_t kFlagHideHud = kFlagHidePlayerGauge | kFlagHideCards;

constexpr uint32_t kObjBattle = 0x16;

// player object
constexpr uintptr_t kPlDeck = 0x3F0;
constexpr uintptr_t kPlHp = 0x42C;
constexpr uintptr_t kPlHpMax = 0x430;

// deck object
constexpr uintptr_t kDkFrameTime = 0x14;  // s32, game time this frame, 0 while paused
constexpr uintptr_t kDkHandle    = 0x58;
constexpr uintptr_t kDkSide      = 0x5C;
constexpr uintptr_t kDkState     = 0x60;  // 1 = idle, 2 = reloading
constexpr uintptr_t kDkRow       = 0x68;  // 0 = main row
constexpr uintptr_t kDkCardsLeft = 0x70;
constexpr uintptr_t kDkCursor    = 0x74;  // s16, index of the selected card in the main row

// battle manager
constexpr uintptr_t kBtlSideState = 0xB8;  // s32 per side: 0 none, 1 card coming out, 2 card in play

// card object
constexpr uintptr_t kCardData = 0x260;

constexpr int32_t kSidePlayer = 1;

// same layout as a deck slot in the save
struct CardData {
    uint8_t id;
    uint8_t premium;
    uint8_t value;
    uint8_t type;   // 0x11 attack, 0x22 magic, 0x44 item
};

constexpr uint8_t kTypeAttack = 1;
constexpr uint8_t kTypeMagic = 2;
constexpr uint8_t kTypeSummon = 3;
constexpr uint8_t kTypeItem = 4;
constexpr CardData kBasicAttack{1, 0, 9, 0x11};  // Kingdom Key 9

// deck object, card list
constexpr uintptr_t kDkCardCount = 0x78;   // s16, slots in the card array
constexpr uintptr_t kDkCards = 0xB8;       // -> card objects
constexpr uintptr_t kCardSize = 0x2E0;

struct CardName {
    uint8_t id;
    const char* name;
};

const CardName kCardNames[] = {
    {26, "Fire"},      {27, "Blizzard"},   {28, "Thunder"},     {29, "Cure"},   {30, "Gravity"},
    {31, "Stop"},      {32, "Aero"},       {35, "Simba"},       {36, "Genie"},  {37, "Bambi"},
    {38, "Dumbo"},     {39, "Tinker Bell"}, {40, "Mushu"},      {41, "Cloud"},  {42, "Potion"},
    {43, "Hi-Potion"}, {44, "Mega-Potion"}, {45, "Ether"},      {46, "Mega-Ether"},
    {47, "Elixir"},    {48, "Megalixir"},     {49, "Donald"},      {50, "Goofy"},
};

const char* card_name(uint8_t id) {
    for (const CardName& c : kCardNames)
        if (c.id == id) return c.name;
    return "???";
}

// MP cost per spell. Cure takes whatever is left, as in KH2.
constexpr float kCostAll = -1.0f;

float spell_cost(uint8_t id) {
    switch (id) {
        case 26: return 12;        // Fire
        case 27: return 15;        // Blizzard
        case 28: return 18;        // Thunder
        case 29: return kCostAll;  // Cure
        case 30: return 20;        // Gravity
        case 31: return 25;        // Stop
        case 32: return 15;        // Aero
        default: return 0;
    }
}

// The command menu. Up and down move the cursor here; confirm is left to the
// game, and the play-card hook decides what the press means from this state.
enum Command { kCmdAttack, kCmdMagic, kCmdItems, kCmdSummon, kCmdCount };

// the friend cards, as the game makes them
constexpr CardData kDonald = {49, 0, 1, 0x44};
constexpr CardData kGoofy = {50, 0, 1, 0x44};

struct Menu {
    int main = kCmdAttack;
    bool sub = false;
    int subSel = 0;
    int subCount = 0;
    CardData subCards[OverlayState::kMaxEntries];
};

using GetObjectFn   = void* (*)(uint32_t id);
using SetPlayCardFn = uint64_t (*)(void* battle, uint32_t handle, int32_t side, void* card,
                                   const CardData* data, int32_t flag);
using ReloadDoneFn  = void (*)(void* deck, int32_t mode);
using SideUpdateFn  = void (*)(void* battle, int32_t side);
using GetFlagsFn    = uint32_t (*)(int32_t group, int32_t id);
using SetFlagsFn    = void (*)(int32_t group, int32_t id, uint32_t value);

static uintptr_t g_base = 0;
static Menu g_menu;

// MP. Re:CoM has none, so it lives here, and only counts while the KH2 HUD
// mod is running to show it. Spells cost MP; when it runs out the gauge
// refills on its own and magic is locked until it is full again.
constexpr float kMpMax = 100.0f;
constexpr float kMpChargeSeconds = 20.0f;
static float g_mp = kMpMax;
static bool g_mpCharging = false;
static bool g_hideCards = true;

// What the combat mod and the KH2 HUD mod tell each other, in a small named
// block of shared memory. Neither needs the other; each stamps its tick every
// frame so the other can tell it is there. When both are, the HUD mod draws
// the command menu in KH2's style from what the combat mod puts here, and
// magic costs MP, which the HUD mod shows.
struct SharedState {
    ULONGLONG hudTick;
    ULONGLONG combatTick;
    float mp;          // 0..1
    LONG mpCharging;
    // the command menu, written by the combat mod
    LONG selected;     // row of the main menu
    LONG subOpen;      // a list is open over it
    LONG subCount;
    LONG subSelected;
    char subNames[16][24];
};
constexpr char kSharedName[] = "Local\\recom_mods_v2";

// Whichever mod loads first makes the block.
static volatile SharedState* shared_state() {
    static volatile SharedState* block = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedState),
                                          kSharedName))
            block = static_cast<SharedState*>(MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedState)));
    }
    return block;
}

static bool kh2_hud_running() {
    volatile SharedState* b = shared_state();
    return b && GetTickCount64() - b->hudTick < 500;
}
static bool g_hideKeyDown = false;
static WORD g_padPrev = 0;
static bool g_keyPrev[4] = {};
static FILE* g_log = nullptr;
static bool g_disabled = false;

template <typename T>
static T& at(uintptr_t addr) { return *reinterpret_cast<T*>(addr); }

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void open_log() {
    char path[MAX_PATH];
    HMODULE self = nullptr;
    // Pin the module. Reloading scripts closes the Lua state, which would unload
    // us and leave the game calling into a hook that is no longer there.
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCSTR>(&open_log), &self);
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n + 4 >= MAX_PATH) return;
    lstrcatA(path, ".log");
    g_log = _fsopen(path, "w", _SH_DENYWR);
}

static uintptr_t player_deck() {
    if (at<uint8_t>(g_base + rva::kBattleFlag) != 1) return 0;
    uintptr_t player = at<uintptr_t>(g_base + rva::kPlayerPtr);
    if (!player) return 0;
    return at<uintptr_t>(player + kPlDeck);
}

static void publish_menu() {
    OverlayState& o = overlay_state();
    o.selected = g_menu.main;
    o.subSelected = g_menu.subSel;
    o.subCount = g_menu.subCount;
    for (int i = 0; i < g_menu.subCount; ++i) o.subNames[i] = card_name(g_menu.subCards[i].id);
    static const char* kTitles[kCmdCount] = {"COMMAND", "MAGIC", "ITEMS", "SUMMON"};
    o.subTitle = kTitles[g_menu.main];
    o.subOpen = g_menu.sub;
}

static void close_submenu() {
    g_menu.sub = false;
    publish_menu();
}

// Lists what the deck holds of one kind, one row per card id, keeping the
// highest value of each.
static void open_submenu(int command) {
    static const uint8_t kKind[kCmdCount] = {kTypeAttack, kTypeMagic, kTypeItem, kTypeSummon};
    g_menu.subCount = 0;
    g_menu.subSel = 0;
    // Donald and Goofy head the Summon list and are always there, instead of
    // being friend cards that have to be picked up off the floor mid-battle.
    if (command == kCmdSummon) {
        g_menu.subCards[g_menu.subCount++] = kDonald;
        g_menu.subCards[g_menu.subCount++] = kGoofy;
    }
    uintptr_t deck = player_deck();
    if (deck) {
        int n = at<int16_t>(deck + kDkCardCount);
        uintptr_t cards = at<uintptr_t>(deck + kDkCards);
        for (int i = 0; i < n && cards; ++i) {
            CardData c = at<CardData>(cards + i * kCardSize + kCardData);
            if (c.id == 0 || (c.type >> 4) != kKind[command]) continue;
            int found = -1;
            for (int j = 0; j < g_menu.subCount; ++j)
                if (g_menu.subCards[j].id == c.id) found = j;
            if (found >= 0) {
                if (c.value > g_menu.subCards[found].value) g_menu.subCards[found] = c;
            } else if (g_menu.subCount < OverlayState::kMaxEntries) {
                g_menu.subCards[g_menu.subCount++] = c;
            }
        }
    }
    g_menu.sub = true;
    publish_menu();
}

// The game still scrolls its own deck on the same buttons that move the menu,
// and confirm on the reload card reloads instead of reaching the play hook.
// Keep the deck's cursor parked on the first ordinary card so confirm always
// comes through to us.
static void park_deck_cursor(uintptr_t deck) {
    if (at<int32_t>(deck + kDkState) != 1 || at<int16_t>(deck + kDkRow) != 0) return;
    int n = at<int16_t>(deck + kDkCardCount);
    uintptr_t cards = at<uintptr_t>(deck + kDkCards);
    if (!cards) return;
    for (int i = 0; i < n; ++i) {
        CardData c = at<CardData>(cards + i * kCardSize + kCardData);
        uint8_t kind = c.type >> 4;
        if (c.id == 0 || kind < kTypeAttack || kind > kTypeItem) continue;
        if (at<int16_t>(deck + kDkCursor) != i) at<int16_t>(deck + kDkCursor) = static_cast<int16_t>(i);
        return;
    }
}

// D-pad or arrow keys move the cursor, B / Backspace / left closes a submenu.
// The game's own menu sounds, the ones its pause menu and shop use. Each call
// starts a new voice, so these go on a press and never on a held state.
enum MenuSound { kSeCursor = 1, kSeConfirm = 2, kSeOpen = 3, kSeCancel = 4, kSeError = 5 };

static void menu_sound(MenuSound id) {
    using PlaySeFn = uint32_t (*)(uint32_t id, int32_t volume);
    reinterpret_cast<PlaySeFn>(g_base + rva::kPlaySe)(id, -1);
}

static void update_menu() {
    XINPUT_STATE pad{};
    WORD buttons = XInputGetState(0, &pad) == ERROR_SUCCESS ? pad.Gamepad.wButtons : 0;
    WORD pressed = buttons & ~g_padPrev;
    g_padPrev = buttons;

    static const int kKeys[4] = {VK_UP, VK_DOWN, VK_BACK, VK_LEFT};
    bool key[4];
    for (int i = 0; i < 4; ++i) {
        bool down = (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
        key[i] = down && !g_keyPrev[i];
        g_keyPrev[i] = down;
    }

    bool up = (pressed & XINPUT_GAMEPAD_DPAD_UP) || key[0];
    bool down = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) || key[1];
    bool back = (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_LEFT)) || key[2] || key[3];

    int step = (down ? 1 : 0) - (up ? 1 : 0);
    if (g_menu.sub) {
        if (back) {
            g_menu.sub = false;
            menu_sound(kSeCancel);
        } else if (step && g_menu.subCount > 1) {
            g_menu.subSel = (g_menu.subSel + step + g_menu.subCount) % g_menu.subCount;
            menu_sound(kSeCursor);
        }
    } else if (step) {
        g_menu.main = (g_menu.main + step + kCmdCount) % kCmdCount;
        menu_sound(kSeCursor);
    }
    if (step || back) publish_menu();
}

// Replaces the setPlayCard call made when a deck plays its current card. For
// the player the deck card is left where it is and a copy is played instead.
// Reporting failure to the caller makes it skip the bookkeeping that would
// take the card out of the hand, so nothing ever leaves the deck.
static uint64_t play_card_hook(void* battle, uint32_t handle, int32_t side, void* card,
                               const CardData* data, int32_t flag) {
    auto real = reinterpret_cast<SetPlayCardFn>(g_base + rva::kSetPlayCard);
    if (side != kSidePlayer || !card) return real(battle, handle, side, card, data, flag);

    // Confirm was pressed. What it does depends on the menu, not on the card
    // the deck happens to have selected.
    CardData copy;
    if (!g_menu.sub) {
        if (g_menu.main != kCmdAttack) {
            open_submenu(g_menu.main);
            menu_sound(kSeOpen);
            return 0;
        }
        copy = kBasicAttack;
    } else {
        if (g_menu.subCount == 0) {
            menu_sound(kSeError);
            return 0;
        }
        copy = g_menu.subCards[g_menu.subSel];
    }

    // magic is free unless the KH2 HUD mod is there to show MP
    float cost = (copy.type >> 4) == kTypeMagic && kh2_hud_running() ? spell_cost(copy.id) : 0;
    if (cost != 0) {
        if (g_mpCharging || g_mp <= 0 || (cost > 0 && g_mp < cost)) {   // not enough MP
            menu_sound(kSeError);
            return 0;
        }
    }

    uint64_t r = real(battle, handle, side, nullptr, &copy, flag);
    logf("play %02X value %d -> %llu", copy.id, copy.value, r);
    if (r && cost != 0) {
        g_mp = cost == kCostAll ? 0 : g_mp - cost;
        if (g_mp <= 0) {
            g_mp = 0;
            g_mpCharging = true;
        }
    }
    if (r && g_menu.sub) {
        menu_sound(kSeConfirm);
        close_submenu();
    }
    return 0;
}

// Finds a free page within rel32 range of addr for a jump stub.
static uint8_t* alloc_near(uintptr_t addr) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity;
    uintptr_t start = addr & ~(step - 1);
    for (uintptr_t off = step; off < 0x70000000; off += step) {
        if (start > off) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(start - off), 0x1000,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        }
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(start + off), 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
            return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

static bool write_code(uint8_t* at_addr, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at_addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at_addr, bytes, n);
    VirtualProtect(at_addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at_addr, n);
    return true;
}

// Points an existing `call rel32` at target, through a stub close enough to reach.
static bool hook_call(const char* name, uintptr_t site_rva, const uint8_t expect[5], void* target) {
    uint8_t* site = reinterpret_cast<uint8_t*>(g_base + site_rva);
    if (memcmp(site, expect, 5) != 0) {
        logf("%s: unexpected bytes at call site, not installing", name);
        return false;
    }
    static uint8_t* page = nullptr;
    static size_t used = 0;
    if (!page) page = alloc_near(reinterpret_cast<uintptr_t>(site));
    if (!page) {
        logf("%s: no memory near the exe", name);
        return false;
    }
    // jmp [rip+0] ; dq target
    uint8_t* stub = page + used;
    used += 16;
    const uint8_t jmp[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(stub, jmp, 6);
    memcpy(stub + 6, &target, 8);

    uint8_t call[5] = {0xE8};
    int32_t rel = static_cast<int32_t>(stub - (site + 5));
    memcpy(call + 1, &rel, 4);
    if (!write_code(site, call, 5)) return false;
    logf("%s: hooked, stub %p", name, stub);
    return true;
}

// Card breaks. Each side runs the state machine once a frame and it compares
// its card against the other side's whenever that side has a card coming out
// or in play. Hide the other side for the length of the call and the two never
// meet, so nothing is broken and nothing is refused.
static void side_update_hook(void* battle, int32_t side) {
    auto real = reinterpret_cast<SideUpdateFn>(g_base + rva::kSideUpdate);
    int32_t other = (side - 1) ^ 1;
    int32_t& state = at<int32_t>(reinterpret_cast<uintptr_t>(battle) + kBtlSideState + other * 4);
    int32_t saved = state;
    bool hide = saved == 1 || saved == 2;
    if (hide) state = 0;
    real(battle, side);
    if (hide && state == 0) state = saved;
}

// Removes a `call rel32` whose result nobody reads.
static bool nop_call(const char* name, uintptr_t site_rva, const uint8_t expect[5]) {
    uint8_t* site = reinterpret_cast<uint8_t*>(g_base + site_rva);
    if (memcmp(site, expect, 5) != 0) {
        logf("%s: unexpected bytes at call site, leaving it", name);
        return false;
    }
    const uint8_t nops[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    return write_code(site, nops, 5);
}

static void install_hooks() {
    const uint8_t play_call[5]  = {0xE8, 0xA8, 0x67, 0xFF, 0xFF};
    const uint8_t side_call1[5] = {0xE8, 0x49, 0xD2, 0xFF, 0xFF};
    const uint8_t side_call2[5] = {0xE8, 0x3C, 0xD2, 0xFF, 0xFF};
    hook_call("play card", rva::kPlayCall, play_call, reinterpret_cast<void*>(&play_card_hook));
    bool a = hook_call("side update 1", rva::kSideCall1, side_call1, reinterpret_cast<void*>(&side_update_hook));
    bool b = hook_call("side update 2", rva::kSideCall2, side_call2, reinterpret_cast<void*>(&side_update_hook));

    // a new card is never refused for being lower than the one already out:
    // mov eax,1 ; ret
    if (a && b) {
        const uint8_t always_ok[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        write_code(reinterpret_cast<uint8_t*>(g_base + rva::kJudgePlay), always_ok, sizeof(always_ok));
        logf("card breaks off");
    }

    // The menu owns up/down and the shoulder buttons now. Without these the
    // deck still turns underneath it and confirm can land on the reload card.
    const uint8_t switch_row[5] = {0xE8, 0x08, 0xF6, 0xF6, 0xFF};
    const uint8_t jump_first[5] = {0xE8, 0x02, 0x45, 0xF7, 0xFF};
    const uint8_t jump_last[5]  = {0xE8, 0xE0, 0x44, 0xF7, 0xFF};
    const uint8_t scroll[5]     = {0xE8, 0x6F, 0x40, 0xF7, 0xFF};
    bool d = nop_call("switch row", rva::kInSwitchRow, switch_row);
    d &= nop_call("jump first", rva::kInJumpFirst, jump_first);
    d &= nop_call("jump last", rva::kInJumpLast, jump_last);
    d &= nop_call("scroll", rva::kInScroll, scroll);
    logf(d ? "deck input off" : "deck input only partly off");

    // The play routine refuses the player while the hide-cards flag is up.
    // Zero the bit it tests for the player's side so the HUD can be hidden
    // and confirm still gets through.  mov eax,2 -> mov eax,0
    uint8_t* gate = reinterpret_cast<uint8_t*>(g_base + rva::kPlayGate);
    const uint8_t gate_expect[5] = {0xB8, 0x02, 0x00, 0x00, 0x00};
    const uint8_t gate_open[5]   = {0xB8, 0x00, 0x00, 0x00, 0x00};
    if (memcmp(gate, gate_expect, 5) == 0 && write_code(gate, gate_open, 5)) logf("play gate open");
    else logf("play gate: unexpected bytes, card hud will stay visible"), g_hideCards = false;

    // Outside battle the game shows the current map card bottom right, where
    // the portrait and HP gauge now go. The room map and this panel share one
    // hide bit; make only the panel's test read it as set.  and eax,8 -> or eax,8
    uint8_t* panel = reinterpret_cast<uint8_t*>(g_base + rva::kMapCardGate);
    const uint8_t panel_expect[3] = {0x83, 0xE0, 0x08};
    const uint8_t panel_hide[3]   = {0x83, 0xC8, 0x08};
    if (memcmp(panel, panel_hide, 3) == 0) logf("map card panel already hidden");
    else if (memcmp(panel, panel_expect, 3) == 0 && write_code(panel, panel_hide, 3)) logf("map card panel hidden");
    else logf("map card panel: unexpected bytes, leaving it");
}

static void set_hud_flags(uint32_t bits) {
    uint32_t f = reinterpret_cast<GetFlagsFn>(g_base + rva::kGetFlags)(kFlagGroup, kFlagCards);
    if ((f & bits) != bits) reinterpret_cast<SetFlagsFn>(g_base + rva::kSetFlags)(kFlagGroup, kFlagCards, f | bits);
}

// Outside battle the game marks the nearest thing in reach, and raises a flag
// on the marker when it is something to act on rather than just look at: a
// door, a chest, a save point, someone to talk to. That is the moment for the
// menu's context command. Returns its label, or null.
static const char* field_context(uintptr_t player) {
    constexpr uintptr_t kPlMarker = 0x3F8;     // -> Gauge::CLockonMarker
    constexpr uintptr_t kPlControl = 0x230;    // bit 0: the player has control
    constexpr uintptr_t kMkTarget = 0xD0;      // task id of what is marked
    constexpr uintptr_t kMkEnabled = 0xF0;
    constexpr uintptr_t kMkFlags = 0xF8;       // 0x10 something to act on, 1 hidden
    if (!(at<uint32_t>(player + kPlControl) & 1)) return nullptr;
    uintptr_t marker = at<uintptr_t>(player + kPlMarker);
    if (!marker || !at<int32_t>(marker + kMkEnabled)) return nullptr;
    int32_t id = at<int32_t>(marker + kMkTarget);
    uint32_t flags = at<uint32_t>(marker + kMkFlags);
    if (id <= 0 || !(flags & 0x10) || (flags & 1)) return nullptr;

    using FindTaskFn = void* (*)(int);
    void* obj = reinterpret_cast<FindTaskFn>(g_base + rva::kFindTask)(id);
    if (!obj) return nullptr;
    switch (at<uintptr_t>(reinterpret_cast<uintptr_t>(obj)) - g_base) {
        case rva::kVtDoor:
        case rva::kVtChest:
        case rva::kVtChestPooh: return "Open";
        case rva::kVtSavePoint: return "Save";
        case rva::kVtWarpPoint: return "Warp";
        case rva::kVtAvatar: return "Talk";
        case rva::kVtCarry:
        case rva::kVtCarryTu: return "Lift";
        default: return "Examine";
    }
}

// The game's portrait and HP gauge sit top left. With the command menu bottom
// left they read better bottom right, where KH2 has them. Everything of the
// gauge hangs off one controller whose position is set once from the layout
// file and then left alone, so it only has to be written over each frame.
// The screen is 512x384 with the origin in the middle; a wide picture adds
// 85 either side, which the game takes off again at draw time.
static void move_player_gauge(uintptr_t player) {
    constexpr uintptr_t kPlGauge = 0x3E0;
    constexpr uintptr_t kGgCtrl = 0x78, kGgWide = 0xA0, kGgMaxHp = 0xA8;
    constexpr float kMargin = 16;    // what the game leaves round it top left
    uintptr_t gauge = at<uintptr_t>(player + kPlGauge);
    if (!gauge || at<uintptr_t>(gauge) != g_base + rva::kVtPlayerGauge) return;
    uintptr_t ctrl = at<uintptr_t>(gauge + kGgCtrl);
    if (!ctrl || at<uintptr_t>(ctrl) != g_base + rva::kVtSeqCtrl) return;

    // The HP bar wraps the portrait and then runs right, further the more max
    // HP there is. Keep its far end inside the picture.
    const float wide = at<float>(gauge + kGgWide);
    int maxHp = at<uint16_t>(gauge + kGgMaxHp);
    if (maxHp > 560) maxHp = 560;
    float reach = maxHp * 470.0f / 560 - 270 + 2;
    if (reach < 44) reach = 44;
    const float x = (256 + wide - kMargin - reach) + wide;
    const float y = 192 - kMargin - 44;
    at<float>(ctrl + 0xF0) = x;
    at<float>(ctrl + 0xF4) = y;
    at<float>(ctrl + 0x40) = x;
    at<float>(ctrl + 0x44) = y;

    // Outside battle the friend cards owned are lined up beside the portrait,
    // top left. With the gauge gone from there they are left on their own, and
    // Donald and Goofy are in the Summon list now anyway. Each card rides on
    // its own controller, three in a row; push those off the picture. The game
    // puts them back whenever the display changes state, hence every frame.
    constexpr uintptr_t kGgFriends = 0x60, kFgCtrls = 0x78, kCtrlSize = 0x170;
    uintptr_t friends = at<uintptr_t>(gauge + kGgFriends);
    if (!friends || at<uintptr_t>(friends) != g_base + rva::kVtFriendGauge) return;
    uintptr_t ctrls = at<uintptr_t>(friends + kFgCtrls);
    if (!ctrls) return;
    for (int i = 0; i < 3; ++i) {
        uintptr_t c = ctrls + i * kCtrlSize;
        if (at<uintptr_t>(c) != g_base + rva::kVtSeqCtrl) break;
        at<float>(c + 0xF0) = at<float>(c + 0x40) = -4000;
        at<float>(c + 0xF4) = at<float>(c + 0x44) = -4000;
    }
}

// True while something of the game's own is up over play: the start menu, the
// pause screen, the save menu. The game asks the same question before it lets
// a menu open, with the same mask.
static bool game_menu_open() {
    constexpr uintptr_t kSfFlags = 0x5A0;
    constexpr uint32_t kBusy = 0x80201C01;
    uintptr_t sub = at<uintptr_t>(g_base + rva::kSubField);
    uint32_t flags = sub ? at<uint32_t>(sub + kSfFlags) : 0;
    int32_t hudOff = at<int32_t>(g_base + rva::kHudOff);
    static uint32_t lastFlags = 0;
    static int32_t lastOff = 0;
    if (flags != lastFlags || hudOff != lastOff) logf("game state: flags %08X, hud off %d", flags, hudOff);
    lastFlags = flags;
    lastOff = hudOff;
    return (flags & kBusy) != 0 || hudOff != 0;
}

// Where Sora is and how he is doing. Returns his deck, or 0 outside battle.
static uintptr_t publish_status() {
    uintptr_t deck = player_deck();
    OverlayState& hud = overlay_state();
    // with the KH2 HUD mod up the menu is its to draw, in KH2's style
    hud.kh2Hud = kh2_hud_running();
    hud.menu = !hud.kh2Hud;
    hud.world = at<uint8_t>(g_base + rva::kWorld);
    if (!deck) {
        // Walking around: HP from the save block, since the battle object is
        // not there.
        hud.battle = false;
        uintptr_t fieldPlayer = at<uintptr_t>(g_base + rva::kPlayerPtr);
        uintptr_t save = at<uintptr_t>(g_base + rva::kSavePtr);
        hud.visible = fieldPlayer && save && at<uint8_t>(g_base + rva::kWorld) != 0xFF &&
                      at<uint8_t>(g_base + rva::kLoading) != 0xFF && !game_menu_open();
        hud.context = nullptr;
        if (hud.visible) {
            hud.hp = at<int32_t>(save);
            hud.hpMax = at<int32_t>(save + 4);
            hud.context = field_context(fieldPlayer);
            if (!hud.kh2Hud) move_player_gauge(fieldPlayer);
            static const char* lastContext = nullptr;
            if (hud.context != lastContext) logf("context: %s", hud.context ? hud.context : "none");
            lastContext = hud.context;
        }
        return 0;
    }
    hud.visible = !game_menu_open();
    hud.battle = true;
    hud.context = nullptr;
    uintptr_t player = at<uintptr_t>(g_base + rva::kPlayerPtr);
    hud.hp = at<int32_t>(player + kPlHp);
    hud.hpMax = at<int32_t>(player + kPlHpMax);
    if (!hud.kh2Hud) move_player_gauge(player);
    return deck;
}

static void frame() {
    uintptr_t deck = publish_status();
    if (volatile SharedState* b = shared_state()) {
        b->combatTick = GetTickCount64();
        b->mp = g_mp / kMpMax;
        b->mpCharging = g_mpCharging;
        b->selected = g_menu.main;
        b->subOpen = g_menu.sub;
        b->subCount = g_menu.subCount;
        b->subSelected = g_menu.subSel;
        for (int i = 0; i < g_menu.subCount && i < 16; ++i) {
            const char* name = card_name(g_menu.subCards[i].id);
            char* to = const_cast<char*>(b->subNames[i]);
            strncpy_s(to, 24, name, _TRUNCATE);
        }
    }
    if (!deck) {
        // every battle starts on Attack with nothing open
        if (g_menu.sub || g_menu.main != kCmdAttack) {
            g_menu = Menu{};
            publish_menu();
        }
        return;
    }
    update_menu();
    park_deck_cursor(deck);

    // the game's own frame time, so nothing here runs while it is paused
    // (measured: 2500 of its units to the second)
    float dt = at<int32_t>(deck + kDkFrameTime) / 2500.0f;
    if (dt < 0 || dt > 0.25f) dt = 0;
    if (g_mpCharging) {
        g_mp += kMpMax * dt / kMpChargeSeconds;
        if (g_mp >= kMpMax) {
            g_mp = kMpMax;
            g_mpCharging = false;
        }
    }

    // F6 shows the game's own HUD again, for comparing against the original
    bool hideKey = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (hideKey && !g_hideKeyDown) {
        g_hideCards = !g_hideCards;
        logf("game hud %s", g_hideCards ? "hidden" : "shown");
        if (!g_hideCards) {
            uint32_t f = reinterpret_cast<GetFlagsFn>(g_base + rva::kGetFlags)(kFlagGroup, kFlagCards);
            reinterpret_cast<SetFlagsFn>(g_base + rva::kSetFlags)(kFlagGroup, kFlagCards, f & ~kFlagHideHud);
        }
    }
    g_hideKeyDown = hideKey;
    // the cards go; the portrait and HP gauge stay unless the KH2 HUD mod takes them
    if (g_hideCards) set_hud_flags(kFlagHideCards);

    // out of cards: finish a reload straight away
    if (at<int32_t>(deck + kDkState) == 1 && at<int16_t>(deck + kDkCardsLeft) == 0) {
        reinterpret_cast<ReloadDoneFn>(g_base + rva::kReloadDone)(reinterpret_cast<void*>(deck), 0);
        logf("auto reload");
    }
}

// Safety net for a hang at start-up, which in exclusive fullscreen takes the
// whole desktop with it: for the first few minutes, if the game stops calling
// in for 20 seconds, note how far things got and end the process.
static volatile LONG g_beat = 0;
static const char* volatile g_stage = "start";

static DWORD WINAPI watchdog(void*) {
    const ULONGLONG began = GetTickCount64();
    LONG seen = g_beat;
    ULONGLONG since = began;
    while (GetTickCount64() - began < 180000) {
        Sleep(1000);
        if (g_beat != seen) {
            seen = g_beat;
            since = GetTickCount64();
        } else if (GetTickCount64() - since > 20000) {
            logf("watchdog: no frame for 20 s after frame %ld, last stage: %s. ending the game", seen, g_stage);
            TerminateProcess(GetCurrentProcess(), 1);
        }
    }
    logf("watchdog: start-up went fine (%ld frames), standing down", g_beat);
    return 0;
}

static int guarded_frame() {
    __try {
        frame();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

extern "C" {

// lua_CFunction. Takes no arguments and returns nothing, so it never touches the Lua state.
__declspec(dllexport) int rc_frame(void*) {
    if (g_disabled) return 0;
    if (!g_base) {
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        open_log();
        logf("loaded, exe base %p", reinterpret_cast<void*>(g_base));
        CloseHandle(CreateThread(nullptr, 0, &watchdog, nullptr, 0, nullptr));
        g_stage = "installing";
        install_hooks();
        overlay_install(&logf);
    }
    // the first frames are logged one by one, so a hang shows where it stopped
    const LONG n = InterlockedIncrement(&g_beat);
    if (n <= 5 || n == 60 || n == 600) logf("frame %ld", n);
    g_stage = "frame";
    if (int code = guarded_frame()) {
        logf("exception %08X, disabling", code);
        g_disabled = true;
    }
    g_stage = "between frames";
    return 0;
}

}
