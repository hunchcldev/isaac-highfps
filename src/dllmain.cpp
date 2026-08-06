// Isaac high-FPS — milestone A: decouple the render loop from the logic cadence.
//
// Vanilla: main() paces itself to exactly 1/60 s and calls the update wrapper once per
// iteration. The wrapper drives a phase machine that alternates logic tick / render
// preview, so the frame counter advances 2 per 1/30 s tick.
//
// Here we (1) remove the pacing so the loop free-runs at the display rate, and
// (2) gate the update wrapper back to its vanilla 60 Hz cadence. Gameplay speed is
// therefore unchanged while the render loop runs as fast as the display allows.
// Motion is still only 60 Hz at this stage — intermediate previews come in milestone B.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include "offsets.h"

namespace {

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

uintptr_t g_base = 0;
HMODULE g_self = nullptr;

// ---------------------------------------------------------------- config
struct Config {
    bool enabled = true;
    bool interpolate = true;
    int  maxFps = 0;        // 0 = as fast as the display allows
    bool log = true;
    bool profile = false;   // sampling profiler; costs real time, for diagnosis only
    bool cacheFileProbes = true;
    bool luaVanillaCadence = true;   // pin mod callbacks to the vanilla 60 Hz
    bool luaOverlay = true;          // and composite what they draw onto every frame
} g_cfg;

void LoadConfig() {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(g_self, path, MAX_PATH)) return;
    if (char* slash = strrchr(path, '\\')) slash[1] = 0;
    strcat(path, "isaac-highfps.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;   // defaults are fine

    g_cfg.enabled     = GetPrivateProfileIntA("highfps", "Enabled", 1, path) != 0;
    g_cfg.interpolate = GetPrivateProfileIntA("highfps", "Interpolate", 1, path) != 0;
    g_cfg.maxFps      = GetPrivateProfileIntA("highfps", "MaxFps", 0, path);
    g_cfg.log         = GetPrivateProfileIntA("highfps", "Log", 1, path) != 0;
    g_cfg.profile     = GetPrivateProfileIntA("highfps", "Profile", 0, path) != 0;
    g_cfg.cacheFileProbes = GetPrivateProfileIntA("highfps", "CacheFileProbes", 1, path) != 0;
    g_cfg.luaVanillaCadence = GetPrivateProfileIntA("highfps", "LuaVanillaCadence", 1, path) != 0;
    g_cfg.luaOverlay        = GetPrivateProfileIntA("highfps", "LuaOverlay", 1, path) != 0;
}

inline uint8_t* Addr(uintptr_t staticVa) {
    return reinterpret_cast<uint8_t*>(g_base + (staticVa - isaac::kImageBase));
}

// Refuse to patch anything we do not recognise. The offsets are pinned to build
// v1.9.7.17; on any other build these checks are the only thing between us and a
// crash in someone else's save file.
bool Expect(uintptr_t va, const uint8_t* want, size_t n, const char* what) {
    uint8_t* p = Addr(va);
    if (memcmp(p, want, n) == 0) return true;
    char got[128] = {0};
    for (size_t i = 0; i < n && i < 16; ++i)
        sprintf(got + strlen(got), "%02X ", p[i]);
    Log("[FATAL] %s @ 0x%08X: unexpected bytes (%s) - wrong game build?", what, va, got);
    return false;
}

// Freeze every other thread in the process for the duration of a code write.
//
// Without this, Poke overwrites several bytes of a function the game thread is running
// right now (the update wrapper, the limiter jumps - both in the hot loop). If the CPU is
// decoding at that address while our memcpy is halfway through, it executes a torn
// instruction, computes a garbage jump target and lands in unpaged memory. That is a
// c0000005 in module "unknown" at offset 0, and because it depends on hitting a
// sub-microsecond window it shows up as "crashes on maybe one launch in ten" on a fast
// machine and never at all on a slower one. The exact report that led here.
constexpr int kMaxFrozen = 128;
HANDLE g_frozen[kMaxFrozen];
int    g_frozenCount = 0;

void FreezeOtherThreads() {
    g_frozenCount = 0;
    const DWORD me  = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof te };
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            // SuspendThread is asynchronous; a register read forces it to have actually
            // stopped before we touch the code it might be sitting in.
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(th, &ctx);
            if (g_frozenCount < kMaxFrozen) g_frozen[g_frozenCount++] = th;
            else { ResumeThread(th); CloseHandle(th); }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void ThawOtherThreads() {
    for (int i = 0; i < g_frozenCount; ++i) {
        ResumeThread(g_frozen[i]);
        CloseHandle(g_frozen[i]);
    }
    g_frozenCount = 0;
}

bool Poke(uintptr_t va, const uint8_t* bytes, size_t n) {
    uint8_t* p = Addr(va);
    DWORD old = 0;
    // Page protection is changed BEFORE the freeze on purpose. VirtualProtect can block on
    // the process VM lock, and if a frozen thread were holding it we would deadlock. The
    // freeze wraps only the raw write and the icache flush, neither of which needs a lock
    // a suspended thread could be holding.
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    FreezeOtherThreads();
    memcpy(p, bytes, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    ThawOtherThreads();
    VirtualProtect(p, n, old, &old);
    return true;
}

void WriteRel32Jmp(uint8_t* at, const void* target) {
    at[0] = 0xE9;
    *reinterpret_cast<int32_t*>(at + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (reinterpret_cast<uintptr_t>(at) + 5));
}

// ---------------------------------------------------------------- pacing
double g_qpcToSeconds = 0.0;
int64_t g_qpcStart = 0;

double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart - g_qpcStart) * g_qpcToSeconds;
}

// Vanilla drives the wrapper 60 times a second. We reproduce exactly that, so the
// frame counter still advances 2 per 1/30 s tick and every one of the ~52 parity
// consumers keeps seeing what it always saw.
constexpr double kWrapperPeriod = 1.0 / 60.0;
constexpr double kTickPeriod    = 1.0 / 30.0;

double g_nextWrapper = 0.0;
double g_lastWrapperTime = 0.0, g_lastTickTime = 0.0;
bool g_previewEnabled = false;
uint64_t g_loopFrames = 0, g_wrapperCalls = 0;
double g_statWindow = 0.0;

bool g_worldAdvancing = false;
uint32_t g_lastGameFrame = 0;
double g_frozenSince = 0.0;   // when the gameplay clock last stopped, 0 while running

// True while the render that follows this loop iteration shows an intermediate frame,
// i.e. one the vanilla engine would never have drawn. The Lua gate reads it.
volatile LONG g_luaFrameIntermediate = 0;
volatile LONG g_luaSuppressed = 0;
volatile LONG g_probeHits = 0, g_probeMisses = 0;

// Overlay state, declared here because the frame loop and the statistics line below both
// touch it; everything that acts on it lives further down under "lua overlay".
volatile LONG g_inRenderPhase = 0;
volatile LONG g_inModRenderPass = 0;   // set while LuaEngine::PostRender is on the stack
volatile LONG g_pushRate = 0;          // last measured fps, handed to Lua for the companion
LONG g_overlayScopes = 0, g_overlayBlits = 0;
bool g_overlayFailed = false;
void PrepareOverlay();
volatile LONG g_dirShortcuts = 0;   // probes answered by the missing-directory shortcut
volatile LONG g_listShortcuts = 0;  // probes answered from a cached directory listing

// Cached negatives are retired wholesale by bumping a generation counter. The game does
// not create files under resources/ or mods/ while running, but "does not" is not
// "cannot", and an unbounded cache turns any surprise into a permanent one.
volatile LONG g_probeGen = 1;
double g_probeGenAt = 0.0;
constexpr double kProbeGenSeconds = 5.0;

uint32_t FrameCounter() {
    uintptr_t mgr = *reinterpret_cast<uintptr_t*>(Addr(isaac::kGManagerPtr));
    return mgr >= 0x10000 ? *reinterpret_cast<uint32_t*>(mgr + isaac::kMgrFrameCount) : 0;
}

using WrapperFn = int(__cdecl*)();
WrapperFn g_wrapperTrampoline = nullptr;

// ------------------------------------------------- intermediate frames
// The engine draws a state between two logic ticks by stashing Position, advancing it by
// 0.5 * TimeScale * Friction * Velocity and marking the entity so the next logic tick can
// roll it back. We reuse that storage and that marker, but compute the step ourselves for
// an arbitrary fraction of the tick.
uintptr_t GameObj() { return *reinterpret_cast<uintptr_t*>(Addr(0x00C71678)); }

// The gameplay clock, distinct from the render frame counter: it only advances inside
// the full 30 Hz logic pass, so it stands still on the pause menu, during room
// transitions and in cutscenes.
uint32_t GameFrame() {
    uintptr_t game = GameObj();
    return game >= 0x10000 ? *reinterpret_cast<uint32_t*>(game + 0x264F8) : 0;
}

// Game+0x18300 -> the active entity list; +0x125C is Entity*[], +0x1264 the count.
uintptr_t EntityList() {
    uintptr_t game = GameObj();
    if (game < 0x10000) return 0;
    return *reinterpret_cast<uintptr_t*>(game + 0x18300);
}

template <typename F>
void ForEachEntity(F&& fn) {
    uintptr_t list = EntityList();
    if (list < 0x10000) return;
    uint32_t count = *reinterpret_cast<uint32_t*>(list + 0x1264);
    auto** arr = *reinterpret_cast<uintptr_t***>(list + 0x125C);
    if (!arr || count > 8192) return;   // the engine's own cap is far below this
    for (uint32_t i = 0; i < count; ++i) {
        auto* e = reinterpret_cast<uintptr_t*>(arr[i]);
        if (reinterpret_cast<uintptr_t>(e) >= 0x10000) fn(reinterpret_cast<uintptr_t>(e));
    }
}

// Where each entity stood at the previous authoritative tick.
//
// Extrapolating (predicting position + alpha*Velocity) is what the engine itself does for
// its single half-step, and it is fine over 1/60 s. Stretched across a whole tick at 180 Hz
// it is not: Isaac's AI changes direction constantly, so the prediction is wrong and the
// position snaps back when the real tick lands. That snap is the stutter.
//
// Interpolating between the previous and the current authoritative position cannot be wrong,
// because both ends are real. It costs one tick of latency, which is unnoticeable on things
// you do not control - so the player keeps extrapolation and everyone else gets this.
// Isaac pools entities: a dying enemy's memory is handed straight to the gore effects it
// spawns. Keying on the pointer alone therefore hands a brand-new effect the dead enemy's
// history, and it gets drawn streaking across the room for one frame. The type/variant
// fingerprint plus the tick stamp is what tells a continuing entity from a recycled slot.
struct Track {
    uintptr_t entity;
    uint32_t fingerprint;   // Type and Variant packed together
    float prev[2];
    float cur[2];
    uint32_t stamp;
};

uint32_t Fingerprint(uintptr_t e) {
    const uint32_t type    = *reinterpret_cast<uint32_t*>(e + isaac::kEntType);
    const uint32_t variant = *reinterpret_cast<uint32_t*>(e + isaac::kEntType + 4);
    return (type * 2654435761u) ^ variant;
}

// One tick of movement is small. Anything larger is a teleport, a room change or a slot
// that was recycled behind our back - none of which may be smeared across the screen.
constexpr float kMaxInterpDistanceSq = 150.0f * 150.0f;

constexpr uint32_t kTrackSlots = 4096;   // rooms hold well under a hundred entities
Track g_track[kTrackSlots] = {};
uint32_t g_trackStamp = 0;

Track* FindTrack(uintptr_t e, bool create) {
    uint32_t i = uint32_t((e >> 4) * 2654435761u) & (kTrackSlots - 1);
    for (uint32_t probe = 0; probe < 32; ++probe, i = (i + 1) & (kTrackSlots - 1)) {
        if (g_track[i].entity == e) return &g_track[i];
        if (!g_track[i].entity || g_trackStamp - g_track[i].stamp > 4) {
            if (!create) return nullptr;
            g_track[i].entity = e;
            g_track[i].stamp = g_trackStamp;
            return &g_track[i];
        }
    }
    return nullptr;   // pathological clustering: skip rather than stall
}

// Byte-for-byte what the engine does at 0x9551F0 before an authoritative tick.
void RollbackPreviews() {
    ForEachEntity([](uintptr_t e) {
        if (*reinterpret_cast<uint8_t*>(e + isaac::kEntPreviewFlag)) {
            *reinterpret_cast<uint32_t*>(e + isaac::kEntPosition) =
                *reinterpret_cast<uint32_t*>(e + isaac::kEntPosBackup);
            *reinterpret_cast<uint32_t*>(e + isaac::kEntPosition + 4) =
                *reinterpret_cast<uint32_t*>(e + isaac::kEntPosBackup + 4);
            *reinterpret_cast<uint8_t*>(e + isaac::kEntPreviewFlag) = 0;
        }
    });
}

// We do NOT call the engine's Interpolate for these frames. Its per-type overrides are
// not pure: Entity_Tear::Interpolate (sub_679400) is 2678 bytes, rewrites Velocity at
// 0x67966B/0x679679, forces the height field 0x350, and calls 27 external functions.
// Running that several times per tick corrupts real state — which is exactly what made
// projectiles misbehave. The position maths itself is trivial and documented by
// Entity::Update (sub_6AE820), so we just do it ourselves and stay side-effect free.
//
// Velocity means different things per type: players are integrated twice per tick, so
// theirs is a per-1/60 displacement, while everything else is per-1/30. Each therefore
// gets its own share of its own interval.
void PreviewPass(float worldAlpha, float playerAlpha) {
    ForEachEntity([&](uintptr_t e) {
        if (*reinterpret_cast<uint64_t*>(e + isaac::kEntFlags) & isaac::kFlagNoInterpolate)
            return;

        auto* pos     = reinterpret_cast<float*>(e + isaac::kEntPosition);
        auto* backup  = reinterpret_cast<float*>(e + isaac::kEntPosBackup);
        auto* applied = reinterpret_cast<uint8_t*>(e + isaac::kEntPreviewFlag);

        // First preview since the last authoritative tick owns the backup.
        if (!*applied) { backup[0] = pos[0]; backup[1] = pos[1]; *applied = 1; }

        if (*reinterpret_cast<uint32_t*>(e + isaac::kEntType) == 1) {
            // The player: predict, so that input stays immediate. His own second
            // integrator pass means the window here is 1/60 s, not a whole tick.
            const float step = playerAlpha
                             * *reinterpret_cast<float*>(e + isaac::kEntTimeScale)
                             * *reinterpret_cast<float*>(e + isaac::kEntFriction);
            const auto* vel = reinterpret_cast<float*>(e + isaac::kEntVelocity);
            pos[0] = backup[0] + step * vel[0];
            pos[1] = backup[1] + step * vel[1];
            return;
        }

        // Everyone else: slide between two positions that both actually happened.
        const Track* t = FindTrack(e, false);
        // Only a history from the most recent tick, belonging to this very entity, may be
        // used. Anything else means the entity is new or the slot was recycled, and the
        // right answer is to leave it exactly where the engine put it.
        if (!t || t->stamp != g_trackStamp || t->fingerprint != Fingerprint(e)) return;
        pos[0] = t->prev[0] + worldAlpha * (t->cur[0] - t->prev[0]);
        pos[1] = t->prev[1] + worldAlpha * (t->cur[1] - t->prev[1]);
    });
}

// Called right after an authoritative tick, when positions are real and no preview is
// applied. Today's position becomes the far end of the next interval; yesterday's becomes
// the near end.
void SampleTracks() {
    ++g_trackStamp;
    ForEachEntity([](uintptr_t e) {
        Track* t = FindTrack(e, true);
        if (!t) return;
        const auto* pos = reinterpret_cast<float*>(e + isaac::kEntPosition);
        const uint32_t fp = Fingerprint(e);

        // Continuous only if this exact entity was here last tick.
        bool fresh = (t->stamp != g_trackStamp - 1) || (t->fingerprint != fp);
        if (!fresh) {
            const float dx = pos[0] - t->cur[0], dy = pos[1] - t->cur[1];
            if (dx * dx + dy * dy > kMaxInterpDistanceSq) fresh = true;
        }

        t->prev[0] = fresh ? pos[0] : t->cur[0];
        t->prev[1] = fresh ? pos[1] : t->cur[1];
        t->cur[0] = pos[0];
        t->cur[1] = pos[1];
        t->fingerprint = fp;
        t->stamp = g_trackStamp;
    });
}

double g_nextPresent = 0.0;

// ------------------------------------------------------------- profiling
// Our hook runs once per loop iteration, which is enough to attribute all of it:
// the gap between two entries is the whole iteration, the time inside the trampoline is
// the engine's update, our own passes we time directly, and whatever is left is the
// render. No second hook needed - and Manager::Render is a bad detour target anyway,
// since it takes arguments in ecx AND ebp and rewrites the caller's stack frame.
struct Profile {
    double iterSum = 0, iterMax = 0;
    double updSum  = 0, updMax  = 0;
    double ourSum  = 0, ourMax  = 0;
    uint32_t iters = 0, updates = 0, longFrames = 0;
    uint32_t entPeak = 0;
} g_prof;

double g_prevEntry = 0.0;
constexpr double kLongFrame = 0.020;   // 20 ms: a visible hitch at any refresh rate

// ------------------------------------------------- sampling profiler
// The measurements say every hitch is engine time inside one update call - up to 265 ms
// for a floor generation. To find out *which* code that is, a second thread parks the game
// thread for a moment, reads its instruction pointer and counts it. A few hundred samples
// over one long update is plenty to see where the time goes.
//
// Addresses are recorded as RVAs so they survive ASLR and can be resolved against a
// disassembly afterwards. Off unless Profile=1 in the ini: suspending a thread thousands
// of times a second is not something to inflict on players.
volatile LONG g_inUpdate = 0;
DWORD g_gameThreadId = 0;

constexpr uint32_t kSampleSlots = 8192;
struct Sample { uint32_t addr; uint32_t count; };
Sample g_samples[kSampleSlots] = {};
volatile LONG g_sampleTotal = 0;

// Full addresses, not RVAs into the exe. A stall on a machine that is barely working is
// most likely spent *waiting* - file reads, heap growth, a lock - and all of that lives in
// ntdll or kernel32. Recording exe-relative addresses only would have hidden precisely the
// answer we are looking for.
void RecordSample(uint32_t addr) {
    uint32_t i = (addr * 2654435761u) & (kSampleSlots - 1);
    for (uint32_t probe = 0; probe < 24; ++probe, i = (i + 1) & (kSampleSlots - 1)) {
        if (g_samples[i].count == 0) { g_samples[i].addr = addr; g_samples[i].count = 1; return; }
        if (g_samples[i].addr == addr) { ++g_samples[i].count; return; }
    }
}

// "ntdll.dll+0x1234" — and for the game itself, the static VA so it can be looked up in a
// disassembly directly.
void DescribeAddress(uint32_t addr, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
        char file[MAX_PATH] = {0};
        GetModuleFileNameA(mod, file, MAX_PATH);
        const char* base = strrchr(file, '\\');
        base = base ? base + 1 : file;
        if (reinterpret_cast<uintptr_t>(mod) == g_base)
            _snprintf(out, n, "isaac-ng.exe  static VA 0x%08X",
                      unsigned(addr - g_base + isaac::kImageBase));
        else
            _snprintf(out, n, "%s+0x%X", base, unsigned(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        _snprintf(out, n, "0x%08X (no module - JIT or stack?)", addr);
    }
    out[n - 1] = 0;
}

DWORD WINAPI SamplerThread(LPVOID) {
    HANDLE th = nullptr;
    for (;;) {
        if (!InterlockedCompareExchange(&g_inUpdate, 0, 0)) { Sleep(1); continue; }
        if (!th) {
            if (!g_gameThreadId) { Sleep(1); continue; }
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_gameThreadId);
            if (!th) { Sleep(50); continue; }
        }
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx) && ctx.Eip) RecordSample(uint32_t(ctx.Eip));
            ResumeThread(th);
            InterlockedIncrement(&g_sampleTotal);
        }
        Sleep(1);
    }
}

// Dump the hottest addresses and start over. Called after a hitch.
void DumpSamples(double updateMs) {
    struct Top { uint32_t addr, count; } top[20] = {};
    // Per-module totals answer the first question on their own: is the game computing,
    // or is it waiting on the operating system?
    struct ModTotal { HMODULE mod; uint32_t count; } mods[16] = {};
    uint32_t total = 0;

    for (uint32_t i = 0; i < kSampleSlots; ++i) {
        if (!g_samples[i].count) continue;
        total += g_samples[i].count;

        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(g_samples[i].addr), &m);
        for (int k = 0; k < 16; ++k) {
            if (mods[k].count == 0 || mods[k].mod == m) {
                mods[k].mod = m;
                mods[k].count += g_samples[i].count;
                break;
            }
        }
        for (int k = 0; k < 20; ++k) {
            if (g_samples[i].count > top[k].count) {
                for (int m2 = 19; m2 > k; --m2) top[m2] = top[m2 - 1];
                top[k] = {g_samples[i].addr, g_samples[i].count};
                break;
            }
        }
    }
    if (!total) return;

    Log("[profile] %.1f ms update, %u samples", updateMs, total);
    Log("  -- by module --");
    for (int k = 0; k < 16 && mods[k].count; ++k) {
        char file[MAX_PATH] = "(unknown)";
        if (mods[k].mod) {
            GetModuleFileNameA(mods[k].mod, file, MAX_PATH);
            const char* b = strrchr(file, '\\');
            if (b) memmove(file, b + 1, strlen(b));
        }
        Log("     %-28s %5u  %5.1f%%", file, mods[k].count, 100.0 * mods[k].count / total);
    }
    Log("  -- hottest addresses --");
    char desc[160];
    for (int k = 0; k < 20 && top[k].count; ++k) {
        DescribeAddress(top[k].addr, desc, sizeof desc);
        Log("     %5u  %5.1f%%  %s", top[k].count, 100.0 * top[k].count / total, desc);
    }
    memset(g_samples, 0, sizeof g_samples);
}

uint32_t EntityCount() {
    uintptr_t list = EntityList();
    return list >= 0x10000 ? *reinterpret_cast<uint32_t*>(list + 0x1264) : 0;
}

int __cdecl HookedUpdateWrapper() {
    if (!g_gameThreadId) g_gameThreadId = GetCurrentThreadId();

    // Closes the render window from the other side. If the composite hook never fires —
    // a present path we did not anticipate — this keeps update-phase Lua from being
    // redirected into a layer that nothing would ever draw to the screen.
    g_inRenderPhase = 0;

    // Pacing. Normally we want the display's full rate — that is the whole point — but
    // there are two reasons to hold back.
    //
    // The configured cap is one. The other is measured: room generation costs the engine
    // over 30 ms, and drawing 180 frames per second of an unchanging screen steals CPU
    // and GPU from exactly that work. Once the gameplay clock has stood still for a while
    // (a load, a menu, a long cutscene) there is nothing new to show anyway, so we drop
    // to 60. The delay before it kicks in keeps short room-slide transitions at full rate.
    int targetFps = g_cfg.maxFps;
    if (!g_worldAdvancing && g_frozenSince > 0.0 && Now() - g_frozenSince > 0.25)
        if (targetFps == 0 || targetFps > 60) targetFps = 60;

    if (targetFps > 0) {
        const double period = 1.0 / targetFps;
        double now = Now();
        if (g_nextPresent == 0.0) g_nextPresent = now;
        while (now < g_nextPresent) {
            const double left = g_nextPresent - now;
            if (left > 0.002) Sleep(DWORD(left * 1000.0) - 1);
            now = Now();
        }
        g_nextPresent += period;
        if (now - g_nextPresent > 0.25) g_nextPresent = now + period;
    }

    const double now = Now();
    ++g_loopFrames;

    if (g_prevEntry > 0.0) {
        const double iter = now - g_prevEntry;
        g_prof.iterSum += iter;
        if (iter > g_prof.iterMax) g_prof.iterMax = iter;
        if (iter > kLongFrame) {
            ++g_prof.longFrames;
            // A hitch is worth naming individually - this is what room entry looks like.
            Log("[hitch] %.1f ms  (last engine update %.1f ms, our passes %.2f ms, %u entities)",
                iter * 1000.0, g_prof.updMax * 1000.0, g_prof.ourMax * 1000.0, EntityCount());
        }
        ++g_prof.iters;
    }
    g_prevEntry = now;

    if (now >= g_statWindow + 1.0) {
        // EnableInterpolation is logged here rather than at init: options.ini is parsed
        // some time after g_Manager exists, so an init-time read reports a zero that
        // means "not loaded yet", not "disabled".
        const double iterAvg = g_prof.iters   ? g_prof.iterSum / g_prof.iters   : 0.0;
        const double updAvg  = g_prof.updates ? g_prof.updSum  / g_prof.updates : 0.0;
        const double ourAvg  = g_prof.iters   ? g_prof.ourSum  / g_prof.iters   : 0.0;
        // Whatever the iteration cost that was neither the engine's update nor ours is
        // the render pass, which runs every iteration.
        const double renderAvg = iterAvg - ourAvg - (updAvg * g_prof.updates) / (g_prof.iters ? g_prof.iters : 1);

        const LONG hits = InterlockedExchange(&g_probeHits, 0);
        const LONG misses = InterlockedExchange(&g_probeMisses, 0);
        if (hits || misses)
            Log("[files] %ld answered without a syscall (%ld missing dir, %ld from listing), "
                "%ld went to the kernel (%.0f%% saved)",
                hits, InterlockedExchange(&g_dirShortcuts, 0),
                InterlockedExchange(&g_listShortcuts, 0), misses,
                100.0 * hits / (hits + misses));

        const LONG luaSup = InterlockedExchange(&g_luaSuppressed, 0);
        const LONG ovScopes = InterlockedExchange(&g_overlayScopes, 0);
        const LONG ovBlits  = InterlockedExchange(&g_overlayBlits, 0);
        if (luaSup || ovScopes)
            Log("[lua] %ld dispatches suppressed on intermediate frames, %ld drawn into the "
                "overlay, %ld composites%s", luaSup, ovScopes, ovBlits,
                g_overlayFailed ? "  [overlay DISABLED]" : "");

        // vsync is in here because it is the one thing that silently undoes the whole mod:
        // we remove the engine's limiter but never touch the GL swap interval, so with vsync
        // on the loop is pinned to the refresh rate no matter what we patched. When it is on,
        // loop/s IS the refresh rate, which is the other half of the same diagnosis.
        const bool vsync = (*reinterpret_cast<uint32_t*>(Addr(isaac::kGWindowFlags)) & 0x200) != 0;

        Log("[stat] loop=%llu/s wrapper=%llu/s world=%s vsync=%s | iter avg %.2f max %.1f ms |"
            " engine-upd avg %.2f max %.1f | render~%.2f | ours avg %.3f max %.2f |"
            " ents<=%u | hitches %u",
            (unsigned long long)g_loopFrames, (unsigned long long)g_wrapperCalls,
            g_worldAdvancing ? "moving" : "frozen", vsync ? "ON" : "off",
            iterAvg * 1000.0, g_prof.iterMax * 1000.0,
            updAvg * 1000.0, g_prof.updMax * 1000.0,
            renderAvg * 1000.0,
            ourAvg * 1000.0, g_prof.ourMax * 1000.0,
            g_prof.entPeak, g_prof.longFrames);

        // Hand the measured rate to Lua. Mods cannot count frames for themselves any more,
        // because the callback they would count is exactly the one we pin to 60.
        g_pushRate = LONG(g_loopFrames);

        g_loopFrames = g_wrapperCalls = 0;
        g_prof = Profile{};
        g_statWindow = now;

        // Retire the cached negatives every few seconds. Load bursts finish inside a
        // second, so the win is already banked by the time an entry expires, and nothing
        // we cached can outlive its usefulness by more than kProbeGenSeconds.
        if (now - g_probeGenAt >= kProbeGenSeconds) {
            InterlockedIncrement(&g_probeGen);
            g_probeGenAt = now;
        }
    }

    if (now < g_nextWrapper) {
        // The render following this return shows a frame vanilla never drew, so the
        // Lua gate swallows every engine->Lua dispatch in it.
        g_luaFrameIntermediate = 1;
        // Intermediate frame: no logic runs, but we can still show a state the engine
        // never draws — provided the world is actually moving.
        if (g_previewEnabled) {
            // Never extrapolate past the tick we are waiting for. A late tick would
            // otherwise send everything shooting ahead of where it is about to be.
            auto frac = [](double elapsed, double period) {
                const double a = elapsed / period;
                return float(a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a));
            };
            // No rollback needed here: the preview recomputes from the backup, which
            // still holds the authoritative position.
            // Interpolation is safe even when nothing is moving: with prev == cur the
            // lerp simply yields a constant, so a paused game cannot shudder. Only the
            // player is predicted from velocity, and a frozen world keeps stale velocity
            // around — so his prediction, and only his, is switched off there.
            __try {
                PreviewPass(frac(now - g_lastTickTime, kTickPeriod),
                            g_worldAdvancing ? frac(now - g_lastWrapperTime, kWrapperPeriod)
                                             : 0.0f);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_previewEnabled = false;
                Log("[warn] preview pass faulted - intermediate frames disabled, "
                    "game continues at the vanilla cadence");
            }
            const double ours = Now() - now;
            g_prof.ourSum += ours;
            if (ours > g_prof.ourMax) g_prof.ourMax = ours;
        }
        // The render that follows shows the mod layer from the last real frame. Nothing
        // to clear and nothing for Lua to add, so the flag only matters to the compositor.
        g_inRenderPhase = 1;
        return 0;
    }

    // Real frame from here on: the engine update below fires the logic callbacks and
    // the render that follows fires the render callbacks — both exactly on the vanilla
    // 60 Hz cadence, so Lua must run.
    g_luaFrameIntermediate = 0;

    g_nextWrapper += kWrapperPeriod;
    // After a stall (loading, alt-tab) do not try to replay the missed ticks.
    if (now - g_nextWrapper > 0.25) g_nextWrapper = now + kWrapperPeriod;

    // Hand the engine authoritative positions, always. It only rolls previews back on
    // the logic phase (0x9551D8); on the interpolation phase it goes straight to the
    // half-step. So if our preview were still applied here, two things would break:
    // the engine's own preview would overwrite the backup with an already-displaced
    // position (drift baked in every tick - enemies run away), and the player's real
    // second integrator pass would be built on top of a preview and then undone by the
    // next rollback (his actual movement thrown away - he crawls).
    if (g_previewEnabled) {
        __try {
            RollbackPreviews();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_previewEnabled = false;
            Log("[warn] rollback faulted - intermediate frames disabled");
        }
    }

    g_lastWrapperTime = now;

    // An authoritative tick is every second wrapper call: the one that rolls previews
    // back and runs the real 30 Hz logic. Only there does it make sense to ask whether
    // the world actually moved, since the gameplay clock ticks once per logic pass.
    const uint32_t counter = FrameCounter();
    const bool logicPhase = (counter & 1) != 0;     // this call lands on even -> logic
    if (logicPhase) {
        g_lastTickTime = now;
        const uint32_t gameFrame = GameFrame();
        g_worldAdvancing = (gameFrame != g_lastGameFrame);
        g_lastGameFrame = gameFrame;
        if (g_worldAdvancing) g_frozenSince = 0.0;
        else if (g_frozenSince == 0.0) g_frozenSince = now;
    }

    ++g_wrapperCalls;
    const uint32_t ents = EntityCount();
    if (ents > g_prof.entPeak && ents < 8192) g_prof.entPeak = ents;

    const double updStart = Now();
    if (g_cfg.profile) InterlockedExchange(&g_inUpdate, 1);
    const int result = g_wrapperTrampoline();
    if (g_cfg.profile) InterlockedExchange(&g_inUpdate, 0);
    const double upd = Now() - updStart;
    if (g_cfg.profile && upd > kLongFrame) DumpSamples(upd * 1000.0);
    g_prof.updSum += upd;
    ++g_prof.updates;
    if (upd > g_prof.updMax) g_prof.updMax = upd;

    const double oursStart = Now();

    if (g_previewEnabled) {
        __try {
            // Positions are authoritative only here: right after the logic pass, before
            // anything previews them again. Sampled unconditionally — during a cutscene
            // or transition the gameplay clock stands still while sprites and entities
            // may well keep moving, and skipping the sample would freeze them solid.
            if (logicPhase) SampleTracks();

            // This frame gets rendered too, so it has to follow the same rule as the
            // intermediate ones. Left alone it would show the engine's own half-step
            // extrapolation - one frame in three drawn by a different rule, which reads
            // as a hitch twice per tick.
            const double a = (now - g_lastTickTime) / kTickPeriod;
            PreviewPass(float(a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a)), 0.0f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_previewEnabled = false;
            Log("[warn] track sampling faulted - intermediate frames disabled");
        }
        const double ours = Now() - oursStart;
        g_prof.ourSum += ours;
        if (ours > g_prof.ourMax) g_prof.ourMax = ours;
    }

    // A real frame: the render that follows is the one Lua is allowed to draw in, so the
    // layer it draws into has to exist by the time we get there.
    PrepareOverlay();
    g_inRenderPhase = 1;
    return result;
}

bool InstallWrapperHook() {
    // The prologue is `68 10 C5 C5 00` = push offset off_C5C510. That operand is an
    // absolute address, so the loader rebases it: comparing against the on-disk bytes
    // fails by exactly the ASLR delta. Check the opcode and the *relocated* operand.
    uint8_t* site = Addr(isaac::kManagerUpdateWrp);
    const uint32_t wantOperand = 0x00C5C510u + (g_base - isaac::kImageBase);
    if (site[0] != 0x68 || *reinterpret_cast<uint32_t*>(site + 1) != wantOperand) {
        Log("[FATAL] update wrapper prologue @ 0x%08X: got %02X %08X, expected 68 %08X"
            " - wrong game build?",
            isaac::kManagerUpdateWrp, site[0], *reinterpret_cast<uint32_t*>(site + 1), wantOperand);
        return false;
    }

    uint8_t* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;

    // Copy the live (already relocated) bytes, not the on-disk ones.
    constexpr size_t kPrologueLen = 5;
    memcpy(tramp, site, kPrologueLen);
    WriteRel32Jmp(tramp + kPrologueLen, site + kPrologueLen);
    g_wrapperTrampoline = reinterpret_cast<WrapperFn>(tramp);

    // The displacement is relative to the patch site, not to our staging buffer.
    uint8_t patch[5] = {0xE9};
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&HookedUpdateWrapper) - (reinterpret_cast<uintptr_t>(site) + 5));
    return Poke(isaac::kManagerUpdateWrp, patch, sizeof patch);
}

// ------------------------------------------------------------- limiter
// 0x93134D  84 DB              test bl,bl          ; forceLimiterFlag
// 0x93134F  75 10              jnz  0x931361       ; -> Sleep + spin
// 0x931351  F7 05 .. 00 02 00 00   test vsyncFlags,200h
// 0x93135B  0F 85 D0 FE FF FF  jnz  0x931231       ; vsync on -> straight to loop head
//
// Neutralising the flag test and making the second jump unconditional takes the
// "no pacing" path every iteration. The window-should-close check sits *before*
// this block, so the game still exits cleanly.
bool RemoveFrameLimiter() {
    static const uint8_t kFlagTest[] = {0x84, 0xDB, 0x75, 0x10};
    static const uint8_t kVsyncJnz[] = {0x0F, 0x85, 0xD0, 0xFE, 0xFF, 0xFF};
    if (!Expect(0x0093134D, kFlagTest, sizeof kFlagTest, "limiter flag test")) return false;
    if (!Expect(0x0093135B, kVsyncJnz, sizeof kVsyncJnz, "limiter vsync branch")) return false;

    static const uint8_t kNops[] = {0x90, 0x90, 0x90, 0x90};
    if (!Poke(0x0093134D, kNops, sizeof kNops)) return false;

    // jmp 0x931231 : rel32 = 0x931231 - (0x93135B + 5) = -0x12F
    static const uint8_t kJmpLoopHead[] = {0xE9, 0xD1, 0xFE, 0xFF, 0xFF, 0x90};
    return Poke(0x0093135B, kJmpLoopHead, sizeof kJmpLoopHead);
}

// ------------------------------------------------ file probe cache
// Profiling a room load shows a steady stream of NtQueryFullAttributesFile - the game
// asking "does this file exist?". With a stack of mods installed it walks every mod's
// folder for every single resource, and almost all of those answers are "no". Each one is
// a full kernel round trip.
//
// We cache the negative answers. Only the negatives, and only for paths under the game's
// own resource and mod folders: save data lives elsewhere and must never be cached, or a
// freshly written save would look missing.
//
// The hook goes in through the import table, so not one byte of game code is modified.
using GetFileAttrExW_t = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
GetFileAttrExW_t g_realGetFileAttrExW = nullptr;

// --- directory cache: the one thing the measurement actually supports ---
//
// Logging the paths showed what is really happening: for every single resource the game
// asks all 14 installed mods whether they override it, under both resources/ and
// resources-dlc3/. That is ~28 probes per resource and virtually every answer is "missing".
//
// Every path is distinct, so caching *files* cannot help - and that is exactly why two
// attempts at it returned 0%. But the directories repeat endlessly: if
// mods/<X>/resources/sfx/ does not exist, then no file beneath it can, and most mods have
// no sfx/ and no resources-dlc3/ at all. One directory probe therefore answers thousands
// of file probes.
struct DirEntry { uint32_t hash; BOOL exists; };
constexpr uint32_t kDirSlots = 4096;
DirEntry g_dirs[kDirSlots] = {};

// Hash the parent directory of a path, i.e. everything before the last separator.
template <typename CH>
uint32_t HashParentDir(const CH* path, bool* hasParent) {
    const CH* lastSep = nullptr;
    for (const CH* p = path; *p; ++p)
        if (*p == CH('\\') || *p == CH('/')) lastSep = p;
    *hasParent = lastSep != nullptr;
    if (!lastSep) return 0;

    uint32_t h = 2166136261u;
    for (const CH* p = path; p < lastSep; ++p) {
        CH c = *p;
        if (c >= CH('A') && c <= CH('Z')) c += 32;
        if (c == CH('/')) c = CH('\\');
        h = (h ^ uint32_t(uint8_t(c))) * 16777619u;
    }
    return h ? h : 1;
}

// Returns true when the parent directory is known not to exist.
bool ParentDirKnownMissing(uint32_t h) {
    uint32_t i = h & (kDirSlots - 1);
    for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kDirSlots - 1)) {
        if (g_dirs[i].hash == h) return !g_dirs[i].exists;
        if (g_dirs[i].hash == 0) return false;
    }
    return false;
}

bool ParentDirKnown(uint32_t h) {
    uint32_t i = h & (kDirSlots - 1);
    for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kDirSlots - 1)) {
        if (g_dirs[i].hash == h) return true;
        if (g_dirs[i].hash == 0) return false;
    }
    return false;
}

void RememberDir(uint32_t h, BOOL exists) {
    uint32_t i = h & (kDirSlots - 1);
    for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kDirSlots - 1)) {
        if (g_dirs[i].hash == 0 || g_dirs[i].hash == h) {
            g_dirs[i].hash = h;
            g_dirs[i].exists = exists;
            return;
        }
    }
}

// Probe the parent directory once, wide path. Cheap: it happens at most once per folder.
void LearnParentDirW(const wchar_t* path, uint32_t h) {
    const wchar_t* lastSep = nullptr;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') lastSep = p;
    if (!lastSep) return;
    size_t len = size_t(lastSep - path);
    if (len == 0 || len >= MAX_PATH) return;

    wchar_t dir[MAX_PATH];
    memcpy(dir, path, len * sizeof(wchar_t));
    dir[len] = 0;

    WIN32_FILE_ATTRIBUTE_DATA d = {};
    const BOOL ok = g_realGetFileAttrExW(dir, GetFileExInfoStandard, &d);
    RememberDir(h, ok && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
}


uint32_t HashPathW(const wchar_t* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        wchar_t c = *s;
        if (c >= L'A' && c <= L'Z') c += 32;
        if (c == L'/') c = L'\\';
        h = (h ^ uint32_t(c)) * 16777619u;
    }
    return h ? h : 1;
}

// The directory shortcut answers ~45% of probes. The rest ask about files inside folders
// that DO exist, and for those the only way to avoid a syscall each is to read the folder
// once and answer from the listing. Resources do not appear while the game runs, so a
// listing stays valid for the session.
constexpr uint32_t kListedSlots = 2048;
constexpr uint32_t kFileSlots   = 262144;   // hashes only, 1 MB
uint32_t g_listedDirs[kListedSlots] = {};
uint32_t g_knownFiles[kFileSlots] = {};

bool SetHas(uint32_t* table, uint32_t slots, uint32_t h) {
    uint32_t i = h & (slots - 1);
    for (uint32_t probe = 0; probe < 12; ++probe, i = (i + 1) & (slots - 1)) {
        if (table[i] == h) return true;
        if (table[i] == 0) return false;
    }
    return false;
}

void SetAdd(uint32_t* table, uint32_t slots, uint32_t h) {
    uint32_t i = h & (slots - 1);
    for (uint32_t probe = 0; probe < 12; ++probe, i = (i + 1) & (slots - 1)) {
        if (table[i] == 0 || table[i] == h) { table[i] = h; return; }
    }
}

// Enumerate one directory and remember every name in it. Returns false if the folder is
// implausibly large, in which case we simply keep asking the kernel for it.
bool ListDirectoryW(const wchar_t* path, uint32_t dirHash) {
    const wchar_t* lastSep = nullptr;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') lastSep = p;
    if (!lastSep) return false;
    size_t len = size_t(lastSep - path);
    if (len == 0 || len + 3 >= MAX_PATH) return false;

    wchar_t pattern[MAX_PATH];
    memcpy(pattern, path, len * sizeof(wchar_t));
    pattern[len] = L'\\'; pattern[len + 1] = L'*'; pattern[len + 2] = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    uint32_t count = 0;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        wchar_t full[MAX_PATH];
        memcpy(full, path, len * sizeof(wchar_t));
        full[len] = L'\\';
        size_t n = wcslen(fd.cFileName);
        if (len + 1 + n >= MAX_PATH) continue;
        memcpy(full + len + 1, fd.cFileName, (n + 1) * sizeof(wchar_t));
        SetAdd(g_knownFiles, kFileSlots, HashPathW(full));
        if (++count > 8192) { FindClose(h); return false; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    SetAdd(g_listedDirs, kListedSlots, dirHash);
    return true;
}

using AccessFn = int(__cdecl*)(const char*, int);
AccessFn g_realAccess = nullptr;

constexpr uint32_t kProbeSlots = 16384;
// Two independent hashes, not one. A single 32-bit key collides often enough across a few
// thousand resource paths to matter, and a collision here does not cost performance - it
// reports an existing file as missing, which surfaces as a silently absent sprite.
struct Probe { uint32_t hash; uint32_t check; uint32_t stamp; };
Probe g_probes[kProbeSlots] = {};

void HashPathLower(const char* s, uint32_t* h1, uint32_t* h2) {
    uint32_t a = 2166136261u, b = 2246822519u;
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '/') c = '\\';
        a = (a ^ uint8_t(c)) * 16777619u;
        b = (b + uint8_t(c)) * 2654435761u;
    }
    *h1 = a ? a : 1;
    *h2 = b;
}

// The game's CRT is a different instance from ours, so assigning our own errno would be
// invisible to it. Poke the one the caller actually reads.
int* (__cdecl* g_gameErrno)() = nullptr;

void SetGameErrnoNoEnt() {
    if (g_gameErrno) {
        if (int* e = g_gameErrno()) *e = ENOENT;
    }
}

bool Cacheable(const char* path) {
    // Cheap case-insensitive substring test; only resource lookups qualify.
    for (const char* p = path; *p; ++p) {
        if ((p[0] == 'r' || p[0] == 'R') && _strnicmp(p, "resources", 9) == 0) return true;
        if ((p[0] == 'm' || p[0] == 'M') && _strnicmp(p, "mods", 4) == 0) return true;
    }
    return false;
}

int __cdecl HookedAccess(const char* path, int mode) {
    if (!path || !Cacheable(path)) return g_realAccess(path, mode);

    uint32_t h, check;
    HashPathLower(path, &h, &check);
    const uint32_t gen = uint32_t(g_probeGen);

    uint32_t i = h & (kProbeSlots - 1);
    for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kProbeSlots - 1)) {
        const Probe p = g_probes[i];        // one read, so a concurrent write cannot tear
        if (p.hash == h && p.check == check && p.stamp == gen) {
            InterlockedIncrement(&g_probeHits);
            SetGameErrnoNoEnt();
            return -1;                      // known-missing, no syscall
        }
        if (p.hash == 0) break;
    }

    const int result = g_realAccess(path, mode);
    const LONG n = InterlockedIncrement(&g_probeMisses);
    // Two cache attempts have now come back at 0%. Either these paths really are all
    // distinct - in which case caching is simply the wrong idea and I should stop - or
    // the table saturates. Sampling the actual strings is the only way to tell, so log a
    // handful rather than guess a third time.
    if (n <= 12 || (n % 20000) == 0)
        Log("[probe#%ld] _access \"%s\" -> %s", n, path, result == 0 ? "exists" : "missing");
    if (result != 0) {                      // remember misses only
        i = h & (kProbeSlots - 1);
        for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kProbeSlots - 1)) {
            if (g_probes[i].hash == 0 || g_probes[i].stamp != gen) {
                // Stamp last: until it matches the current generation the entry is not
                // live, so a reader can never see a half-written key.
                g_probes[i].hash = h;
                g_probes[i].check = check;
                g_probes[i].stamp = gen;
                break;
            }
        }
    }
    return result;
}

// Swap one entry in the import table. Returns the original.
void* HookImport(const char* dllName, const char* funcName, void* replacement) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(g_base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(g_base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* mod = reinterpret_cast<const char*>(g_base + desc->Name);
        if (_stricmp(mod, dllName) != 0) continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            g_base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(g_base + desc->FirstThunk);
        for (; thunk->u1.AddressOfData; ++thunk, ++iat) {
            if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(g_base + thunk->u1.AddressOfData);
            if (strcmp(byName->Name, funcName) != 0) continue;

            void* original = reinterpret_cast<void*>(iat->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(iat, sizeof *iat, PAGE_READWRITE, &old)) return nullptr;
            iat->u1.Function = reinterpret_cast<DWORD>(replacement);
            VirtualProtect(iat, sizeof *iat, old, &old);
            return original;
        }
    }
    return nullptr;
}

// The measurement said _access was the wrong door: 76k calls, zero repeats, while
// NtQueryFullAttributesFile sat at 15% of a 290 ms stall. That syscall belongs to
// GetFileAttributesExW, which the C++ standard library uses for "does this path exist" -
// and MSVCP140 imports it, not the exe. So the hook has to go into *every* loaded
// module's import table, not just the game's.

struct AttrEntry {
    uint32_t hash;
    BOOL ok;
    WIN32_FILE_ATTRIBUTE_DATA data;
};
constexpr uint32_t kAttrSlots = 32768;
AttrEntry g_attrs[kAttrSlots] = {};


bool CacheableW(const wchar_t* p) {
    for (; *p; ++p)
        if ((*p == L'r' || *p == L'R') && _wcsnicmp(p, L"resources", 9) == 0) return true;
        else if ((*p == L'm' || *p == L'M') && _wcsnicmp(p, L"mods", 4) == 0) return true;
    return false;
}

BOOL WINAPI HookedGetFileAttrExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    if (!path || level != GetFileExInfoStandard || !CacheableW(path))
        return g_realGetFileAttrExW(path, level, out);

    // Directory shortcut first: if the containing folder does not exist, neither does
    // the file, and we can answer without touching the kernel at all.
    bool hasParent = false;
    const uint32_t dh = HashParentDir(path, &hasParent);
    if (hasParent) {
        if (!ParentDirKnown(dh)) LearnParentDirW(path, dh);
        if (ParentDirKnownMissing(dh)) {
            InterlockedIncrement(&g_dirShortcuts);
            InterlockedIncrement(&g_probeHits);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
        // The folder exists: read it once, then every "is this file in here" question
        // is a lookup instead of a kernel round trip.
        if (!SetHas(g_listedDirs, kListedSlots, dh)) ListDirectoryW(path, dh);
        if (SetHas(g_listedDirs, kListedSlots, dh) &&
            !SetHas(g_knownFiles, kFileSlots, HashPathW(path))) {
            InterlockedIncrement(&g_listShortcuts);
            InterlockedIncrement(&g_probeHits);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
    }

    const uint32_t h = HashPathW(path);
    uint32_t i = h & (kAttrSlots - 1);
    for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kAttrSlots - 1)) {
        if (g_attrs[i].hash == h) {
            InterlockedIncrement(&g_probeHits);
            if (!g_attrs[i].ok) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
            *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(out) = g_attrs[i].data;
            return TRUE;
        }
        if (g_attrs[i].hash == 0) break;
    }

    const BOOL ok = g_realGetFileAttrExW(path, level, out);
    const LONG n = InterlockedIncrement(&g_probeMisses);
    if (n <= 12 || (n % 20000) == 0)
        Log("[probe#%ld] GetFileAttributesExW \"%S\" -> %s", n, path, ok ? "exists" : "missing");
    // Game resources do not change while the game runs, so both answers are safe to keep.
    i = h & (kAttrSlots - 1);
    for (uint32_t probe = 0; probe < 8; ++probe, i = (i + 1) & (kAttrSlots - 1)) {
        if (g_attrs[i].hash == 0) {
            g_attrs[i].hash = h;
            g_attrs[i].ok = ok;
            if (ok) g_attrs[i].data = *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(out);
            break;
        }
    }
    return ok;
}

// Patch one import across every module currently loaded in the process.
int HookImportEverywhere(const char* funcName, void* replacement, void** originalOut) {
    int patched = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = {sizeof me};
    for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me)) {
        auto modBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        if (modBase == reinterpret_cast<uintptr_t>(g_self)) continue;   // never ourselves
        __try {
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(modBase + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) continue;

            for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(modBase + dir.VirtualAddress);
                 desc->Name; ++desc) {
                auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(
                    modBase + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
                auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(modBase + desc->FirstThunk);
                for (; thunk->u1.AddressOfData; ++thunk, ++iat) {
                    if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
                    auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(modBase + thunk->u1.AddressOfData);
                    if (strcmp(byName->Name, funcName) != 0) continue;
                    if (reinterpret_cast<void*>(iat->u1.Function) == replacement) continue;
                    if (originalOut && !*originalOut)
                        *originalOut = reinterpret_cast<void*>(iat->u1.Function);
                    DWORD old = 0;
                    if (VirtualProtect(iat, sizeof *iat, PAGE_READWRITE, &old)) {
                        iat->u1.Function = reinterpret_cast<DWORD>(replacement);
                        VirtualProtect(iat, sizeof *iat, old, &old);
                        ++patched;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip malformed module */ }
    }
    CloseHandle(snap);
    return patched;
}

bool InstallFileCache() {
    // Our CRT is a separate instance from the game's, so setting our own errno would be
    // invisible to the caller. Resolve theirs. Every _access caller we know of checks the
    // return value, so this is belt-and-braces rather than load-bearing.
    if (HMODULE crt = GetModuleHandleA("api-ms-win-crt-runtime-l1-1-0.dll"))
        g_gameErrno = reinterpret_cast<int*(__cdecl*)()>(GetProcAddress(crt, "_errno"));
    if (!g_gameErrno)
        if (HMODULE crt = GetModuleHandleA("ucrtbase.dll"))
            g_gameErrno = reinterpret_cast<int*(__cdecl*)()>(GetProcAddress(crt, "_errno"));

    g_realAccess = reinterpret_cast<AccessFn>(
        HookImport("api-ms-win-crt-filesystem-l1-1-0.dll", "_access",
                   reinterpret_cast<void*>(&HookedAccess)));

    void* orig = nullptr;
    const int n = HookImportEverywhere("GetFileAttributesExW",
                                       reinterpret_cast<void*>(&HookedGetFileAttrExW), &orig);
    if (orig) g_realGetFileAttrExW = reinterpret_cast<GetFileAttrExW_t>(orig);
    if (!g_realGetFileAttrExW)   // nobody imported it by name; go straight to the export
        g_realGetFileAttrExW = reinterpret_cast<GetFileAttrExW_t>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetFileAttributesExW"));

    Log("file cache: GetFileAttributesExW patched in %d import slots", n);
    return g_realGetFileAttrExW != nullptr;
}

// ----------------------------------------------------------- lua overlay
// Suppressing Lua on intermediate frames keeps mod code at the vanilla cadence, but it
// also means everything mods DRAW exists on one frame in three. At 180 Hz that reads as
// flicker across every mod HUD in the game.
//
// The fix is to stop treating mod graphics as part of the frame and give them a surface
// of their own. On a real frame each Lua dispatch is bracketed: flush whatever the engine
// has queued, bind our own render target, let Lua draw into it, flush again, unbind. At
// the end of every frame — real or intermediate — that surface is composited over the
// finished image. Mod graphics are therefore present on all 180 frames while their Lua
// still runs exactly 60 times a second.
//
// None of this is our own rendering. It is the engine's own post-processing recipe,
// copied from the "Colormod Surface" path in sub_6FBC10, pointed at one more surface.
struct KColor   { float r, g, b, a; int32_t unk; };                       // 0x14
struct SrcQuad  { float tl[2], tr[2], bl[2], br[2]; int32_t space; };     // 0x24
struct DestQuad { float p[8]; KColor c[4]; };                             // 0x70
struct SmartImage { void* image; void* counter; };

using PushRtFn   = void(__cdecl*)();
using PopRtFn    = void(__cdecl*)();
using SetRtFn    = void(__fastcall*)(void* mgr, void* edx, void* target, int screenSized);
using ClearFn    = void(__fastcall*)(void* mgr, void* edx);
using FlushFn    = void(__fastcall*)(void* mgr, void* edx, int one);
using SelShaderFn= void(__stdcall*)(int);
using CreateRtFn = SmartImage*(__stdcall*)(SmartImage*, uint32_t, uint32_t, const char*, KColor*);
using ImgRenderFn= void(__fastcall*)(void* img, void* edx, SrcQuad*, DestQuad*, KColor*);
using ImgFilterFn= void(__fastcall*)(void* img, void* edx, int minF, int magF);

PushRtFn    g_pushRt   = nullptr;
PopRtFn     g_popRt    = nullptr;
SetRtFn     g_setRt    = nullptr;
ClearFn     g_clearRt  = nullptr;
FlushFn     g_flushRt  = nullptr;
CreateRtFn  g_createRt = nullptr;

void*     g_overlay = nullptr;     // the ImageBase* we draw mod output into
SmartImage g_overlayRef = {};      // held for the process lifetime, deliberately never released
uint32_t  g_overlayW = 0, g_overlayH = 0;

inline void* Mgr() { return Addr(isaac::kGraphicsManager); }

// A fault in here is a wrong address or a wrong assumption about engine state, and the
// only useful thing to know is which of the two and where. Naming the faulting
// instruction as a static VA makes it a lookup in the disassembly rather than a guess.
int OverlayFault(const char* where, EXCEPTION_POINTERS* ep) {
    char desc[160] = "(no context)";
    DWORD code = 0;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        DescribeAddress(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            ep->ExceptionRecord->ExceptionAddress)), desc, sizeof desc);
    }
    Log("[overlay] fault in %s: code 0x%08X at %s - overlay disabled for this session",
        where, code, desc);
    g_overlayFailed = true;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Clear() resets the manager's blend state and picks its own shader. The engine does that
// at every target switch of its own, but we switch in the middle of someone else's frame,
// so we put back exactly what we found.
struct GraphicsState {
    uint32_t blend[5];
    uint32_t shader;
    void Save() {
        memcpy(blend, Addr(isaac::kGraphicsManager + isaac::kMgrBlendState), sizeof blend);
        shader = *reinterpret_cast<uint32_t*>(Addr(isaac::kCurrentShader));
    }
    void Restore() const {
        memcpy(Addr(isaac::kGraphicsManager + isaac::kMgrBlendState), blend, sizeof blend);
        *reinterpret_cast<uint32_t*>(Addr(isaac::kCurrentShader)) = shader;
    }
};

// GPU resources may only be created on the thread that owns the GL context, so every
// caller of this sits on the game thread. Called again after a resolution change, which
// is why it compares against the size it last built for.
bool EnsureOverlay() {
    if (g_overlayFailed || !g_createRt) return false;
    const uint32_t w = *reinterpret_cast<uint32_t*>(Addr(isaac::kScreenWidth));
    const uint32_t h = *reinterpret_cast<uint32_t*>(Addr(isaac::kScreenHeight));
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return false;   // graphics not up yet
    if (g_overlay && w == g_overlayW && h == g_overlayH) return true;

    KColor transparent = {0, 0, 0, 0, 0};
    SmartImage created = {};
    g_createRt(&created, w, h, "isaac-highfps Lua overlay", &transparent);
    if (!created.image) {
        Log("[overlay] render target creation failed at %ux%u", w, h);
        return false;
    }
    // The previous surface (if this is a resize) keeps its reference and is dropped by the
    // engine's own accounting. One surface per resolution change is not a leak worth code.
    g_overlayRef = created;
    g_overlay = created.image;

    auto** vt = *reinterpret_cast<void***>(g_overlay);
    reinterpret_cast<ImgFilterFn>(vt[isaac::kVtImageSetFilter])(g_overlay, nullptr, 1, 1);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_overlay) + isaac::kImgFilterCacheMin) = 1;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_overlay) + isaac::kImgFilterCacheMag) = 1;

    g_overlayW = w; g_overlayH = h;
    Log("[overlay] surface ready: %ux%u at 0x%08X", w, h, (unsigned)(uintptr_t)g_overlay);
    return true;
}

// Binds the overlay for the duration of the mod render pass. The flush on the way in
// matters as much as the one on the way out: batches the engine queued for ITS target must
// be drained before we rebind, or they would land in ours.
//
// Exactly one of these per frame. The first version wrapped every single Lua dispatch,
// which cost the frame rate the whole point of this mod and made the game's own UI
// flicker — a flush forces the engine to emit its queued geometry early, so doing it
// dozens of times a frame both destroys batching and reorders the draws.
class OverlayScope {
public:
    OverlayScope() {
        if (!g_cfg.luaOverlay || g_overlayFailed || !g_inRenderPhase) return;
        if (g_luaFrameIntermediate) return;   // the gate holds Lua back, nothing will draw
        if (GetCurrentThreadId() != g_gameThreadId) return;   // only the GL-owning thread
        __try {
            if (!EnsureOverlay()) return;
            m_state.Save();
            // The one flush that is genuinely required: a batch is drawn into whatever
            // target is bound when it is FLUSHED, not when it was queued, so anything the
            // engine still has pending has to be emitted before we rebind.
            g_flushRt(Mgr(), nullptr, 1);
            g_pushRt();
            g_setRt(Mgr(), nullptr, g_overlay, 1);
            // Wipe last frame's mod output while we are already bound, rather than paying
            // for a second push/bind/pop earlier in the frame just to do this.
            g_clearRt(Mgr(), nullptr);
            m_active = true;
            InterlockedIncrement(&g_overlayScopes);
        } __except (OverlayFault("bind", GetExceptionInformation())) {}
    }
    ~OverlayScope() {
        if (!m_active) return;
        __try {
            g_flushRt(Mgr(), nullptr, 1);
            g_popRt();
            m_state.Restore();
        } __except (OverlayFault("unbind", GetExceptionInformation())) {}
    }
    OverlayScope(const OverlayScope&) = delete;
    OverlayScope& operator=(const OverlayScope&) = delete;
private:
    GraphicsState m_state = {};
    bool m_active = false;
};

// The surface has to exist before the gate is allowed to hold Lua back, and creating it
// is only legal on the thread that owns the GL context. Doing it here, once per real
// frame from the game thread, keeps that guarantee without any work in the steady state.
void PrepareOverlay() {
    if (!g_cfg.luaOverlay || g_overlayFailed || g_overlay) return;
    __try {
        EnsureOverlay();
    } __except (OverlayFault("create", GetExceptionInformation())) {}
}

// Composite the mod layer onto whatever the engine currently has bound. Same call the
// engine uses to put its own Colormod surface on the screen: a 0..1 source quad, a
// full-screen destination, and the blend state it uses for its own composites.
void BlitOverlay() {
    if (!g_overlay) return;
    const float w = *reinterpret_cast<float*>(Addr(isaac::kOrthoWidth));
    const float h = *reinterpret_cast<float*>(Addr(isaac::kOrthoHeight));
    if (!(w > 0.0f) || !(h > 0.0f)) return;

    SrcQuad src = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, 0};
    KColor white = {1, 1, 1, 1, 0};
    DestQuad dst = {};
    dst.p[0] = 0; dst.p[1] = 0;   // top left
    dst.p[2] = w; dst.p[3] = 0;   // top right
    dst.p[4] = 0; dst.p[5] = h;   // bottom left
    dst.p[6] = w; dst.p[7] = h;   // bottom right
    for (int i = 0; i < 4; ++i) dst.c[i] = white;

    // No flush around this one. A state change does not reach back into batches that are
    // already queued — the engine starts a new batch when the state differs — so our quad
    // simply joins the stream and goes out with everything else at the engine's own flush,
    // while the target it belongs to is still bound.
    GraphicsState st; st.Save();
    auto* blend = reinterpret_cast<uint32_t*>(Addr(isaac::kGraphicsManager + isaac::kMgrBlendState));
    blend[0] = 0;
    blend[1] = (*reinterpret_cast<uint32_t*>(Addr(isaac::kGWindowFlags)) & 4) ? 1u : 6u;
    blend[2] = 7;
    blend[3] = 1;
    blend[4] = 7;

    // Once, so the geometry is on record rather than assumed: if the quad we draw is not
    // the size of the target it lands in, the mod layer is scaled or clipped and no amount
    // of staring at the composite logic will show it.
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("[overlay] first composite: layer %ux%u, quad %.0fx%.0f, target 0x%08X",
            g_overlayW, g_overlayH, w, h,
            *reinterpret_cast<uint32_t*>(Addr(isaac::kGraphicsManager + isaac::kMgrCurRenderTgt)));
    }

    auto** vt = *reinterpret_cast<void***>(g_overlay);
    reinterpret_cast<ImgRenderFn>(vt[isaac::kVtImageRender])(g_overlay, nullptr, &src, &dst, &white);
    st.Restore();
    InterlockedIncrement(&g_overlayBlits);
}

// The composite point.
//
// The first attempt did this at SwapBuffers and faulted on the very first frame: by then
// the engine has finished with the frame's render batches, so a fresh draw has nothing to
// attach to. LuaEngine::PostRender is the opposite — it is the moment the engine hands the
// finished world to mods precisely so they can draw on it. Drawing here cannot be
// ill-timed, and it inherits the z-order mod graphics have always had, colormod included.
//
// On a real frame the Lua inside draws into our layer (the gate is open, so OverlayScope
// redirects it); on an intermediate frame the gate swallows it and the layer still holds
// what the last real frame produced. Either way the same composite follows, so mod
// graphics are on all 180 frames while their Lua ran 60 times.
using LuaPostRenderFn = int(__cdecl*)();
LuaPostRenderFn g_luaPostRenderTramp = nullptr;

void CompositeOverlay() {
    if (!g_cfg.luaOverlay || g_overlayFailed || !g_overlay) return;
    __try {
        BlitOverlay();
    } __except (OverlayFault("composite", GetExceptionInformation())) {}
}

int __cdecl HookedLuaPostRender() {
    int result;
    {
        OverlayScope draws;                 // inactive on intermediate frames
        g_inModRenderPass = 1;              // the gate only bites inside this pass
        result = g_luaPostRenderTramp();
        g_inModRenderPass = 0;
    }
    CompositeOverlay();
    return result;
}

bool InstallOverlay() {
    // We call these rather than patch them, but calling the wrong address is worse than
    // patching it, so each one has to identify itself first. Only the leading bytes are
    // compared: anything further in tends to be an absolute address the loader rebases.
    static const uint8_t kPush[]   = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
    static const uint8_t kPop[]    = {0x51, 0x83, 0x3D};
    static const uint8_t kSetRt[]  = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x56};
    // `cmp dword [esi+68h], 0` — also proves currentRenderTarget still sits at +0x68.
    static const uint8_t kClear[]  = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x68, 0x00, 0x74};
    static const uint8_t kFlush[]  = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4};
    static const uint8_t kCreate[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    if (!Expect(isaac::kPushRenderTarget, kPush,  sizeof kPush,  "PushRenderTarget")  ||
        !Expect(isaac::kPopRenderTarget,  kPop,   sizeof kPop,   "PopRenderTarget")   ||
        !Expect(isaac::kSetRenderTarget,  kSetRt, sizeof kSetRt, "SetRenderTarget")   ||
        !Expect(isaac::kClearTarget,      kClear, sizeof kClear, "ClearRenderTarget") ||
        !Expect(isaac::kFlushBatches,     kFlush, sizeof kFlush, "FlushRenderBatches")||
        !Expect(isaac::kCreateRenderTgt,  kCreate,sizeof kCreate,"CreateRenderTarget"))
        return false;

    g_pushRt   = reinterpret_cast<PushRtFn  >(Addr(isaac::kPushRenderTarget));
    g_popRt    = reinterpret_cast<PopRtFn   >(Addr(isaac::kPopRenderTarget));
    g_setRt    = reinterpret_cast<SetRtFn   >(Addr(isaac::kSetRenderTarget));
    g_clearRt  = reinterpret_cast<ClearFn   >(Addr(isaac::kClearTarget));
    g_flushRt  = reinterpret_cast<FlushFn   >(Addr(isaac::kFlushBatches));
    g_createRt = reinterpret_cast<CreateRtFn>(Addr(isaac::kCreateRenderTgt));

    // Detour LuaEngine::PostRender for the composite. Its prologue is push ebp / mov ebp,
    // esp / push -1, which is exactly five bytes and ends on an instruction boundary.
    static const uint8_t kPostRender[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    if (!Expect(isaac::kLuaPostRender, kPostRender, sizeof kPostRender, "LuaEngine::PostRender"))
        return false;

    uint8_t* site = Addr(isaac::kLuaPostRender);
    auto* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, site, sizeof kPostRender);
    WriteRel32Jmp(tramp + sizeof kPostRender, site + sizeof kPostRender);
    g_luaPostRenderTramp = reinterpret_cast<LuaPostRenderFn>(tramp);

    uint8_t patch[5] = {0xE9};
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&HookedLuaPostRender) - (reinterpret_cast<uintptr_t>(site) + 5));
    return Poke(isaac::kLuaPostRender, patch, sizeof patch);
}

// -------------------------------------------------------------- lua gate
// Vanilla fires every Lua render callback once per 60 Hz frame. Our free-running loop
// renders three times as often, which without this gate fires MC_POST_RENDER and friends
// three times as often too — tripling every installed mod's Lua render cost, and silently
// breaking mods that keep frame counters or game logic inside render callbacks (a common
// workaround, since render callbacks were long the only per-frame hook available).
//
// Every engine->Lua dispatch — all 51 call sites of the callback surface — funnels
// through two imports: lua_pcallk and lua_callk. On intermediate frames we emulate the
// call instead of making it: drop the callee and its arguments from the Lua stack, push
// the requested number of nils, report LUA_OK. To the caller that is indistinguishable
// from "no mod registered anything", a case every call site already handles. Real frames
// pass through untouched. Mod Lua therefore runs exactly 60 times per second, in vanilla
// order, against vanilla game state — the invariant is restored, not worked around.
//
// Known cost until the overlay layer lands: what mods DRAW inside those callbacks exists
// only on real frames, so mod graphics flicker at high fps. LuaVanillaCadence=0 trades
// back: smooth mod graphics, triple Lua cost, render-callback logic runs fast again.
//
// Not covered: lua_resume (coroutines). The engine's callback dispatch does not use it.
using LuaGettopFn  = int (__cdecl*)(void*);
using LuaSettopFn  = void(__cdecl*)(void*, int);
using LuaPushnilFn = void(__cdecl*)(void*);
using LuaPushIntFn = void(__cdecl*)(void*, intptr_t);
using LuaSetGlobalFn = void(__cdecl*)(void*, const char*);
using LuaPcallkFn  = int (__cdecl*)(void*, int, int, int, intptr_t, void*);
using LuaCallkFn   = void(__cdecl*)(void*, int, int, intptr_t, void*);

LuaGettopFn  g_luaGettop  = nullptr;
LuaSettopFn  g_luaSettop  = nullptr;
LuaPushnilFn g_luaPushnil = nullptr;
LuaPushIntFn g_luaPushInt = nullptr;
LuaSetGlobalFn g_luaSetGlobal = nullptr;
LuaPcallkFn  g_realPcallk = nullptr;
LuaCallkFn   g_realCallk  = nullptr;

// Lua runs on the game thread only; anything else calling in (there should be nothing)
// passes through untouched rather than getting its stack rearranged mid-flight.
//
// Only the mod render pass is held back, not every callback in the frame. That pass is
// where the expensive HUD work lives (item descriptions, stat trackers) and where mods
// have historically parked logic for want of a better hook, so it is the one that has to
// keep the vanilla 60 Hz — and it is the one whose output we can composite.
//
// The per-entity render callbacks are deliberately left alone. They are cheap, they draw
// relative to an entity whose position we are already interpolating, and letting them run
// per frame means they track that movement instead of being pinned to 60 Hz. Bracketing
// them was the first attempt and it cost more than it bought: they are interleaved with
// the engine's own geometry, so redirecting them means flushing the batch queue dozens of
// times a frame, which took the frame rate down with it.
// Holding the pass back is only ever correct while the layer that carries its output is
// actually live. If the overlay is off or has faulted, letting Lua run on every frame
// costs performance but shows the right picture, and that is the better failure.
bool SuppressLuaNow() {
    return g_luaFrameIntermediate && g_inModRenderPass &&
           g_cfg.luaOverlay && !g_overlayFailed && g_overlay != nullptr &&
           GetCurrentThreadId() == g_gameThreadId;
}

// Balance the stack exactly like a successful call that returned nothing: the callee
// and its nargs arguments come off, nresults nils go on. LUA_MULTRET (-1) callers
// accept zero results as-is, so the loop simply does not run for them.
void EmulateLuaCall(void* L, int nargs, int nresults) {
    g_luaSettop(L, g_luaGettop(L) - nargs - 1);
    for (int i = 0; i < nresults; ++i) g_luaPushnil(L);
    InterlockedIncrement(&g_luaSuppressed);
}

// Announce ourselves inside the Lua VM.
//
// The Workshop companion used to detect us by counting MC_POST_RENDER: more than 60 a
// second meant the native part was live. The gate above deliberately ends that — the
// callback is pinned to 60 now — so counting frames would report "not installed" on a
// perfectly working install. Two globals replace it, and they also let the companion show
// the real frame rate, which it can no longer measure for the same reason.
//
// Both are set from inside a dispatch, where a valid lua_State is in hand. Each push is
// balanced by the setglobal that pops it, so the stack the engine is mid-call on is
// untouched. Keyed on the state pointer, so a reloaded VM gets them again.
constexpr intptr_t kNativeApiVersion = 2;

void* g_lastLuaState = nullptr;
LONG g_publishedRate = 0;

void PublishPresence(void* L) {
    if (!g_luaPushInt || !g_luaSetGlobal || !L) return;
    if (L != g_lastLuaState) {
        g_lastLuaState = L;
        g_publishedRate = -1;               // force the rate out again for the new VM
        g_luaPushInt(L, kNativeApiVersion);
        g_luaSetGlobal(L, "HIGH_FPS_NATIVE");
    }
    const LONG rate = g_pushRate;
    if (rate != g_publishedRate) {
        g_publishedRate = rate;
        g_luaPushInt(L, rate);
        g_luaSetGlobal(L, "HIGH_FPS_RATE");
    }
}

int __cdecl HookedPcallk(void* L, int nargs, int nresults, int msgh, intptr_t ctx, void* k) {
    if (SuppressLuaNow()) { EmulateLuaCall(L, nargs, nresults); return 0; /* LUA_OK */ }
    PublishPresence(L);
    return g_realPcallk(L, nargs, nresults, msgh, ctx, k);
}

void __cdecl HookedCallk(void* L, int nargs, int nresults, intptr_t ctx, void* k) {
    if (SuppressLuaNow()) { EmulateLuaCall(L, nargs, nresults); return; }
    PublishPresence(L);
    g_realCallk(L, nargs, nresults, ctx, k);
}

bool InstallLuaGate() {
    HMODULE lua = GetModuleHandleA("Lua5.3.3r.dll");
    if (!lua) return false;
    g_luaGettop  = reinterpret_cast<LuaGettopFn >(GetProcAddress(lua, "lua_gettop"));
    g_luaSettop  = reinterpret_cast<LuaSettopFn >(GetProcAddress(lua, "lua_settop"));
    g_luaPushnil = reinterpret_cast<LuaPushnilFn>(GetProcAddress(lua, "lua_pushnil"));
    if (!g_luaGettop || !g_luaSettop || !g_luaPushnil) return false;
    // Optional: without these the gate still works, mods just cannot see that we are here.
    g_luaPushInt   = reinterpret_cast<LuaPushIntFn  >(GetProcAddress(lua, "lua_pushinteger"));
    g_luaSetGlobal = reinterpret_cast<LuaSetGlobalFn>(GetProcAddress(lua, "lua_setglobal"));
    if (!g_luaPushInt || !g_luaSetGlobal)
        Log("[warn] lua_pushinteger/lua_setglobal missing - mods cannot detect the native part");

    g_realPcallk = reinterpret_cast<LuaPcallkFn>(
        HookImport("Lua5.3.3r.dll", "lua_pcallk", reinterpret_cast<void*>(&HookedPcallk)));
    if (!g_realPcallk) return false;
    g_realCallk = reinterpret_cast<LuaCallkFn>(
        HookImport("Lua5.3.3r.dll", "lua_callk", reinterpret_cast<void*>(&HookedCallk)));
    if (!g_realCallk) {
        // Half a gate is worse than none: put the first hook back.
        HookImport("Lua5.3.3r.dll", "lua_pcallk", reinterpret_cast<void*>(g_realPcallk));
        g_realPcallk = nullptr;
        return false;
    }
    return true;
}

DWORD WINAPI Init(LPVOID) {
    LoadConfig();
    if (!g_cfg.enabled) return 0;

    // The mod ships under two names so that a loader which will not pick up one of them
    // can be given the other. Somebody will inevitably install both "to be safe", and two
    // copies patching the same sites means the second one detours our own trampoline. The
    // first instance to get here wins; the other returns before touching anything.
    if (CreateMutexA(nullptr, TRUE, "isaac-highfps-single-instance") == nullptr ||
        GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    if (g_cfg.log) {
        // Wide paths throughout, and a fallback next to the DLL.
        //
        // Both halves of this are the same bug report. The ANSI versions silently fail when
        // the user's profile or install path holds characters their codepage cannot carry,
        // which is easy to have on a non-English Windows and impossible to have on ours. A
        // failed fopen leaves g_log null, and that switches off every diagnostic we have,
        // including the FATAL lines that would say why nothing was patched. The symptom is
        // "no log at all", which is also what a DLL that never loaded looks like.
        wchar_t logPath[MAX_PATH * 2];
        if (GetTempPathW(MAX_PATH, logPath)) {
            wcscat(logPath, L"isaac-highfps.log");
            g_log = _wfopen(logPath, L"w");
        }
        if (!g_log && GetModuleFileNameW(g_self, logPath, MAX_PATH)) {
            if (wchar_t* slash = wcsrchr(logPath, L'\\')) slash[1] = 0;
            wcscat(logPath, L"isaac-highfps.log");
            g_log = _wfopen(logPath, L"w");
        }
    }

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToSeconds = 1.0 / double(f.QuadPart);
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&g_qpcStart));

    Log("isaac-highfps: module base 0x%08X (ASLR delta 0x%08X)",
        (unsigned)g_base, (unsigned)(g_base - isaac::kImageBase));

    // We are loaded as a static import, so DllMain runs before the executable's own
    // entry point — and that entry point is the Steam DRM stub in .bind, which is what
    // decrypts .text. Patching now would write into ciphertext and be overwritten (or
    // worse) moments later. Waiting for g_Manager to be populated puts us safely past
    // both the stub and engine construction.
    uintptr_t mgr = 0;
    for (int waited = 0; waited < 1200; ++waited) {   // up to ~2 minutes of loading
        mgr = *reinterpret_cast<uintptr_t*>(Addr(isaac::kGManagerPtr));
        if (mgr >= 0x10000) break;
        Sleep(100);
    }
    if (mgr < 0x10000) {
        Log("[FATAL] g_Manager never initialised (0x%08X) - not patching", (unsigned)mgr);
        return 0;
    }
    Log("g_Manager = 0x%08X, frameCount = %u", (unsigned)mgr,
        *reinterpret_cast<uint32_t*>(mgr + isaac::kMgrFrameCount));

    g_nextWrapper = Now();
    g_statWindow = Now();

    // Order matters: the hook is what keeps gameplay at its normal speed, so it goes in
    // first. Removing the limiter without it leaves the game running at display rate,
    // i.e. several times too fast.
    if (!InstallWrapperHook()) { Log("wrapper hook FAILED - leaving the game untouched"); return 0; }
    Log("wrapper hook installed, logic cadence pinned to %.2f Hz", 1.0 / kWrapperPeriod);

    if (!RemoveFrameLimiter()) { Log("limiter patch FAILED - hook is live, game stays at 60"); return 0; }
    Log("limiter removed - render loop now free-running");

    if (g_cfg.profile) {
        CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr);
        Log("sampling profiler armed - hitches will dump their hottest addresses");
    }

    if (g_cfg.cacheFileProbes)
        Log(InstallFileCache() ? "file-probe cache installed (_access via import table)"
                               : "[warn] could not hook _access - file-probe cache off");

    if (g_cfg.luaVanillaCadence) {
        const bool gate = InstallLuaGate();
        Log(gate ? "lua gate installed (lua_pcallk/lua_callk via import table) - "
                   "mod callbacks pinned to the vanilla 60 Hz cadence"
                 : "[warn] lua gate failed - mod callbacks will fire per rendered frame");

        // The overlay only has a job while the gate is holding Lua back. Without the gate
        // mods already draw on every frame and there is nothing to carry over.
        if (gate && g_cfg.luaOverlay) {
            if (InstallOverlay()) {
                Log("lua overlay armed - mod graphics will be composited onto every frame");
            } else {
                g_overlayFailed = true;
                Log("[warn] lua overlay could not install - mod graphics will only appear on "
                    "real frames (set LuaVanillaCadence=0 if that flickers)");
            }
        } else if (gate) {
            g_overlayFailed = true;
            Log("lua overlay off by config - mod graphics update at 60 Hz and may flicker");
        }
    }

    g_previewEnabled = g_cfg.interpolate;
    Log("intermediate frames %s, MaxFps=%d",
        g_previewEnabled ? "ON (self-computed, no engine calls)" : "OFF (config)", g_cfg.maxFps);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_self = mod;
        // Everything real happens on its own thread: DllMain runs under the loader lock,
        // and the game's own entry point (the Steam DRM stub that decrypts .text) has not
        // even run yet.
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
