// Isaac high-FPS mod — verified addresses for The Binding of Isaac: Rebirth
// Repentance+ build v1.9.7.17 ("J460"), PE32 x86, image base 0x400000.
//
// Confidence tags:
//   [LIVE]  read out of the running process and confirmed
//   [VER]   static finding that an independent adversarial pass re-read byte-for-byte
//   [SIG]   recovered from a REPENTOGON ZHL signature that matched our binary exactly once
//           AND was cross-checked structurally (a unique match alone is NOT enough:
//           the published Manager::Render pattern matched a resize handler here)
//   [OPEN]  not yet proven — do not patch against this
#pragma once
#include <cstdint>

namespace isaac {

inline constexpr uintptr_t kImageBase = 0x00400000;

// ---------------------------------------------------------------- globals
// g_Manager: the global Manager*. [SIG] captured from the g_Manager ZHL pattern,
// [LIVE] dereferenced in the running game and all member offsets below read back sane.
inline constexpr uintptr_t kGManagerPtr   = 0x00C7169C;
inline constexpr uintptr_t kGMenuManager  = 0x00C72A20;  // [SIG]
inline constexpr uintptr_t kGWindowFlags  = 0x00C798E4;  // [VER] bit 0x200 = vsync enabled

// ------------------------------------------------------- Manager members
// Offsets from *g_Manager. Note REPENTOGON publishes 0x4A8B8 for _framecount and
// 0x2A1FB for _enableInterpolation on their 1.9.7.12 build — both differ from ours,
// so these were derived against THIS binary, not ported.
inline constexpr uintptr_t kMgrFrameCount        = 0x4ABBC;  // [LIVE] +1 per main-loop iteration
inline constexpr uintptr_t kMgrFps               = 0x4B12C;  // [LIVE] engine's own 1/dt
inline constexpr uintptr_t kMgrOptionsConfig     = 0x2A33C;  // [VER]
inline constexpr uintptr_t kMgrEnableInterp      = 0x2A3C0;  // [LIVE] optionsConfig + 0x84

// -------------------------------------------------------- Entity members
// [VER] derived from the Lua accessor thunks (Position getter 0x417290,
// Velocity getter 0x4172D0), not guessed. _flags/_velocity independently agree
// with REPENTOGON's published member table.
inline constexpr uintptr_t kEntType        = 0x028;
inline constexpr uintptr_t kEntFlags       = 0x168;  // uint64, lo dword here, hi at 0x16C
inline constexpr uintptr_t kEntPreviewFlag = 0x175;  // byte: "Position currently holds a preview"
inline constexpr uintptr_t kEntPosition    = 0x33C;  // float2
inline constexpr uintptr_t kEntPosBackup   = 0x344;  // float2, written by Interpolate()
inline constexpr uintptr_t kEntVelocity    = 0x360;  // float2
inline constexpr uintptr_t kEntFrictionBase= 0x368;
inline constexpr uintptr_t kEntFriction    = 0x36C;
inline constexpr uintptr_t kEntTimeScale   = 0x39C;  // float, ctor-init 1.0 at 0x6A94F0

inline constexpr uint64_t kFlagNoInterpolate      = 1ull << 1;
inline constexpr uint64_t kFlagInterpolationUpdate= 1ull << 14;

// Entity vtable layout. Slot 4 is the render-preview step; REPENTOGON's own
// source marks it `skip; // Interpolate`.
inline constexpr int kVtUpdate      = 3;   // +0x0C
inline constexpr int kVtInterpolate = 4;   // +0x10
inline constexpr int kVtRender      = 5;   // +0x14

// ------------------------------------------------------------- functions
inline constexpr uintptr_t kIsaacMain        = 0x00931050;  // [SIG]+[VER] confirmed as main()
inline constexpr uintptr_t kManagerRender    = 0x009555C0;  // [SIG] anchor 0x955C8A lands inside
inline constexpr uintptr_t kManagerUpdateWrp = 0x009AB6D0;  // [VER] thin wrapper -> phase machine
inline constexpr uintptr_t kPhaseMachine     = 0x00954CD0;  // [VER] takes a bool; re-entrant!
inline constexpr uintptr_t kInterpStep       = 0x006FD3F0;  // [VER] the odd-frame substep driver
inline constexpr uintptr_t kEntityUpdateBase = 0x006AE820;  // [VER] the ONE gameplay integrator
inline constexpr uintptr_t kEntityInterpBase = 0x006B0EB0;  // [VER] base Interpolate (vt[4])
inline constexpr uintptr_t kRoomGetTimeScale = 0x007EA3E0;  // [SIG]+[VER] shared TimeScale source
inline constexpr uintptr_t kGameIsPaused     = 0x006FD350;  // [SIG]
inline constexpr uintptr_t kGetRenderPosition= 0x0067F310;  // [SIG]

// ------------------------------------------------------------ patch sites
// The 1/60 limiter constant. THREE readers in main(), not two — the third is a
// watchdog comparing (1/60)/dt against 1.1 and force-enabling the limiter after
// 30 consecutive fast frames. Missing it means the cap silently comes back.
inline constexpr uintptr_t kConst1Over60   = 0x00BAA498;  // [VER] double 0.016666666666666666
inline constexpr uintptr_t kLimiterMain[]  = {0x0093136E, 0x00931445};  // [VER]
inline constexpr uintptr_t kLimiterWatchdog= 0x0093129F;                // [VER] the one we'd have missed
inline constexpr uintptr_t kLimiterLoading[]= {0x008FC3B1, 0x008FC450}; // [VER] loading-screen loop

// The integrator's two TimeScale reads. 8-byte instructions, so a 5-byte jmp fits.
// Scale HERE, never the stored +0x39C field: that same field feeds the per-tick timer
// accumulator at 0x6AE867-0x6AE88E, which must keep counting at 30 Hz.
inline constexpr uintptr_t kIntegratorTsRead[] = {0x006AEB95, 0x006AEBF9};  // [VER]

// The hardcoded 0.5 of the render preview. The constant at 0xBAA2D0 is shared by
// 2810 instructions binary-wide — NEVER patch it in place. Patch the abs32 operand
// of each mulss instead (operand sits at instruction+4).
inline constexpr uintptr_t kConstHalfShared = 0x00BAA2D0;  // [VER] float 0.5, 2810 users
inline constexpr uintptr_t kPreviewHalfOperand[] = {      // [VER] instruction+4
    0x006B0F43, 0x006B0F4B,  // base Interpolate (Player, Bomb, Slot, + delegated)
    0x0065C583, 0x0065C5A7,  // Projectile
    0x00967F85, 0x00967FA5,  // Knife
    0x0097E761, 0x0097E781,  // Laser
    0x0067989A, 0x00679BE8,  // Tear
};

// The rollback loop that undoes previews before the authoritative tick.
// Self-contained; iterates Game+0x125C (array) / +0x1264 (count).
inline constexpr uintptr_t kRollbackLoopBegin = 0x009551D8;  // [VER]
inline constexpr uintptr_t kRollbackLoopEnd   = 0x0095522A;  // [VER]

// Gate we replace. 0x9551CF `F6 87 BC AB 04 00 01` test byte [edi+4ABBC],1
//                  0x9551D6 `75 60`               jnz -> preview branch
// 9-byte window, nothing branches into it. [VER]
inline constexpr uintptr_t kGateParity = 0x009551CF;

}  // namespace isaac
