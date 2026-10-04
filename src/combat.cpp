// Native side of the Re:CoM combat mod. Loaded from battle.lua with
// package.loadlib and driven once per game frame through rc_frame().
// Steam build only: every address below is an RVA into the game exe.
#include "overlay.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace rva {
constexpr uintptr_t kPlayerPtr   = 0x87B390;  // -> player battle object
constexpr uintptr_t kBattleFlag  = 0x87B858;  // 0 field, 1 battle
constexpr uintptr_t kGetObject   = 0x2AD300;  // void* (uint id)
constexpr uintptr_t kSetPlayCard = 0x163830;  // CBTL::setPlayCard
constexpr uintptr_t kReloadDone  = 0x16B2B0;  // finishes a reload for a deck
constexpr uintptr_t kPlayCall    = 0x16D083;  // call setPlayCard inside the play-current-card routine
constexpr uintptr_t kSideUpdate  = 0x15EAC0;  // per-side card battle state machine, (battle, side)
constexpr uintptr_t kSideCall1   = 0x161872;  // the two calls to it, side 1 then side 2
constexpr uintptr_t kSideCall2   = 0x16187F;
constexpr uintptr_t kJudgePlay   = 0x164A50;  // can this card be played over the other side's card
}

constexpr uint32_t kObjBattle = 0x16;

// player object
constexpr uintptr_t kPlDeck = 0x3F0;

// deck object
constexpr uintptr_t kDkHandle    = 0x58;
constexpr uintptr_t kDkSide      = 0x5C;
constexpr uintptr_t kDkState     = 0x60;  // 1 = idle, 2 = reloading
constexpr uintptr_t kDkRow       = 0x68;  // 0 = main row
constexpr uintptr_t kDkCardsLeft = 0x70;

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
constexpr CardData kBasicAttack{1, 0, 9, 0x11};  // Kingdom Key 9

using GetObjectFn   = void* (*)(uint32_t id);
using SetPlayCardFn = uint64_t (*)(void* battle, uint32_t handle, int32_t side, void* card,
                                   const CardData* data, int32_t flag);
using ReloadDoneFn  = void (*)(void* deck, int32_t mode);
using SideUpdateFn  = void (*)(void* battle, int32_t side);

static uintptr_t g_base = 0;
static FILE* g_log = nullptr;
static bool g_playKeyDown = false;
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

// Replaces the setPlayCard call made when a deck plays its current card. For
// the player the deck card is left where it is and a copy is played instead.
// Reporting failure to the caller makes it skip the bookkeeping that would
// take the card out of the hand, so nothing ever leaves the deck.
static uint64_t play_card_hook(void* battle, uint32_t handle, int32_t side, void* card,
                               const CardData* data, int32_t flag) {
    auto real = reinterpret_cast<SetPlayCardFn>(g_base + rva::kSetPlayCard);
    if (side != kSidePlayer || !card) return real(battle, handle, side, card, data, flag);

    CardData copy = at<CardData>(reinterpret_cast<uintptr_t>(card) + kCardData);
    // any attack card swings the same Kingdom Key; magic and items play as themselves
    if ((copy.type >> 4) == kTypeAttack) copy = kBasicAttack;
    uint64_t r = real(battle, handle, side, nullptr, &copy, flag);
    logf("play %02X value %d -> %llu", copy.id, copy.value, r);
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
}

static void frame() {
    uintptr_t deck = player_deck();
    overlay_state().visible = deck != 0;
    if (!deck) return;

    // out of cards: finish a reload straight away
    if (at<int32_t>(deck + kDkState) == 1 && at<int16_t>(deck + kDkCardsLeft) == 0) {
        reinterpret_cast<ReloadDoneFn>(g_base + rva::kReloadDone)(reinterpret_cast<void*>(deck), 0);
        logf("auto reload");
    }

    // F5: play a Kingdom Key 9 that is not in the deck
    bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (down && !g_playKeyDown && at<int32_t>(deck + kDkState) == 1) {
        void* battle = reinterpret_cast<GetObjectFn>(g_base + rva::kGetObject)(kObjBattle);
        if (battle) {
            CardData card{1, 0, 9, 0x11};
            uint64_t r = reinterpret_cast<SetPlayCardFn>(g_base + rva::kSetPlayCard)(
                battle, at<uint32_t>(deck + kDkHandle), at<int32_t>(deck + kDkSide), nullptr, &card, 0);
            logf("virtual card -> %llu", r);
        }
    }
    g_playKeyDown = down;
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
        install_hooks();
        overlay_install(&logf);
    }
    if (int code = guarded_frame()) {
        logf("exception %08X, disabling", code);
        g_disabled = true;
    }
    return 0;
}

}
