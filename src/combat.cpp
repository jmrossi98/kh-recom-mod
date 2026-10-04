// Native side of the Re:CoM combat mod. Loaded from battle.lua with
// package.loadlib and driven once per game frame through rc_frame().
// Steam build only: every address below is an RVA into the game exe.
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

static bool install_play_hook() {
    uint8_t* site = reinterpret_cast<uint8_t*>(g_base + rva::kPlayCall);
    const uint8_t expect[5] = {0xE8, 0xA8, 0x67, 0xFF, 0xFF};
    if (memcmp(site, expect, 5) != 0) {
        logf("play hook: unexpected bytes at call site, not installing");
        return false;
    }
    uint8_t* stub = alloc_near(reinterpret_cast<uintptr_t>(site));
    if (!stub) {
        logf("play hook: no memory near the exe");
        return false;
    }
    // jmp [rip+0] ; dq target
    const uint8_t jmp[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(stub, jmp, 6);
    void* target = reinterpret_cast<void*>(&play_card_hook);
    memcpy(stub + 6, &target, 8);

    int32_t rel = static_cast<int32_t>(stub - (site + 5));
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    logf("play hook installed, stub %p", stub);
    return true;
}

static void frame() {
    uintptr_t deck = player_deck();
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
        install_play_hook();
    }
    if (int code = guarded_frame()) {
        logf("exception %08X, disabling", code);
        g_disabled = true;
    }
    return 0;
}

}
