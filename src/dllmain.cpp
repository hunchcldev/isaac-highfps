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

bool Poke(uintptr_t va, const uint8_t* bytes, size_t n) {
    uint8_t* p = Addr(va);
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
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

        Log("[stat] loop=%llu/s wrapper=%llu/s world=%s | iter avg %.2f max %.1f ms |"
            " engine-upd avg %.2f max %.1f | render~%.2f | ours avg %.3f max %.2f |"
            " ents<=%u | hitches %u",
            (unsigned long long)g_loopFrames, (unsigned long long)g_wrapperCalls,
            g_worldAdvancing ? "moving" : "frozen",
            iterAvg * 1000.0, g_prof.iterMax * 1000.0,
            updAvg * 1000.0, g_prof.updMax * 1000.0,
            renderAvg * 1000.0,
            ourAvg * 1000.0, g_prof.ourMax * 1000.0,
            g_prof.entPeak, g_prof.longFrames);

        g_loopFrames = g_wrapperCalls = 0;
        g_prof = Profile{};
        g_statWindow = now;
    }

    if (now < g_nextWrapper) {
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
        return 0;
    }

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

DWORD WINAPI Init(LPVOID) {
    LoadConfig();
    if (!g_cfg.enabled) return 0;

    if (g_cfg.log) {
        char logPath[MAX_PATH];
        GetTempPathA(MAX_PATH, logPath);
        strcat(logPath, "isaac-highfps.log");
        g_log = fopen(logPath, "w");
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
