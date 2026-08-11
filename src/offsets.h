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

// ------------------------------------------------------------ build select
// The game ships as two diverging x86 builds we support with ONE dll: the main-folder
// Steam build (v1.9.7.17, "J460") and the REPENTOGON build in the Repentogon\ subfolder
// (v1.9.7.12, "J293"). They are different compilations - every image address moved, three
// Manager members shifted, and the frame limiter + update wrapper have a different SHAPE
// (see RemoveFrameLimiter / InstallWrapperHook). Struct member offsets are identical.
//
// Detection is the PE TimeDateStamp read from the host exe's in-memory header. The header
// is NOT encrypted (only .text is, by the Steam DRM stub that runs after us), so this is
// safe to read at init before anything else - unlike a .text byte probe. It is also
// rename-proof, unlike keying on the "Repentogon" folder name.
enum Build { kBuild_1_9_7_17 = 0, kBuild_1_9_7_12 = 1 };
inline Build g_build = kBuild_1_9_7_17;
inline bool  g_isRepentogon = false;
constexpr uint32_t kTimeDateStamp_1_9_7_17 = 0x69E6E3A7;
constexpr uint32_t kTimeDateStamp_1_9_7_12 = 0x68154D8D;

// Addresses that MOVE between builds are mutable globals, seeded with the 1.9.7.17 values
// and overwritten by SelectBuild() for 1.9.7.12. Use sites are unchanged - they were
// `isaac::kX` before and still are; they are just no longer compile-time constants.

// ---------------------------------------------------------------- globals
// g_Manager: the global Manager*. [SIG] captured from the g_Manager ZHL pattern,
// [LIVE] dereferenced in the running game and all member offsets below read back sane.
inline uintptr_t kGManagerPtr   = 0x00C7169C;
inline uintptr_t kGameObjPtr    = 0x00C71678;  // g_Game; was hardcoded in dllmain
inline uintptr_t kGameTick      = 0x000264F8;  // Game + this = 30 Hz gameplay clock
inline constexpr uintptr_t kGMenuManager  = 0x00C72A20;  // [SIG] unused by code
inline uintptr_t kGWindowFlags  = 0x00C798E4;  // [VER] bit 0x200 = vsync enabled

// ------------------------------------------------------- Manager members
// Offsets from *g_Manager. Note REPENTOGON publishes 0x4A8B8 for _framecount and
// 0x2A1FB for _enableInterpolation on their 1.9.7.12 build — both differ from ours,
// so these were derived against THIS binary, not ported.
inline uintptr_t kMgrFrameCount        = 0x4ABBC;  // [LIVE] +1 per main-loop iteration (varies: 12 = 0x4A8B8)
inline constexpr uintptr_t kMgrFps               = 0x4B12C;  // [LIVE] engine's own 1/dt (unused by code)
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

// ------------------------------------------------------- room camera
// The camera is a SEPARATE 0x94-byte object, not inline in the Game object - which is why
// scanning the Game object for it turned up nothing. It is reached Game -> Room -> Camera:
//   Room   = *(Game + 0x18300)   the same pointer the entity list is walked through
//   Camera = *(Room + 0x11F8)    Room::_Camera, from REPENTOGON's Room.zhl
//   pos    =  Camera + 0x60      the render position, a float2
// [LIVE] verified in the running game: Camera+0x60/+0x64 read back the exact player position
// and scrolled with movement; Camera+0x8C is _shouldOverride (matches REPENTOGON's Camera.zhl)
// and Camera+0x90 is the 0.1 ease factor. It is written at 60 Hz (Camera::DoUpdate) and read
// by the render every frame, so uncapped it holds for three frames and then jumps - exactly
// the big-room judder.
inline constexpr uintptr_t kRoomInGame   = 0x18300;  // [LIVE] Game -> current Room*
inline constexpr uintptr_t kCameraInRoom = 0x11F8;   // [SIG] Room::_Camera (Room.zhl)
inline constexpr uintptr_t kCamPosition  = 0x60;     // [LIVE] Camera focus position, float2

// What the renderer ACTUALLY consumes. Camera+0x60 is only the camera's own focus
// state: once per 60 Hz frame Camera::DoUpdate (0x9437E0 -> 0x943830) maps it through
// the same 0.65 world->screen transform the sprites use, clamps it to the room, and
// stores the result here - and every sprite draw adds THIS field, post-snap
// (Room::Render 0x80F200 passes &Room+0x1204 into every Entity::Render).
// [LIVE] proven by cadence: this field steps at exactly 60 Hz while our Camera+0x60
// previews ran at 180 - which is why previewing the camera field never reached the
// screen, and why the extrapolated player sawtoothed against the frozen scroll.
inline constexpr uintptr_t kRoomScrollOfs = 0x1204;  // [VER]+[LIVE] float2, screen units

// ------------------------------------------------- render pixel snapping
// The world->screen mapping every sprite renderer inlines ([VER] three copies in
// Entity_Player::Render 0x78CA00, more in Room::Render 0x80F200):
//   screen = floor(((world - 60|140) * 0.65 + (ortho - 338|182) * 0.5) * s + 0.5) / s
//          + scrollOffset
// with s = *kSnapScaleA * *kSnapScaleB ([LIVE] 1.0 * 2.0 = 2 RT pixels per screen unit)
// and scrollOffset = Room+0x1204/+0x1208, the camera-derived term - added AFTER the
// snap. The position term is therefore quantised to the render-target pixel grid
// independently of the camera, which is what makes a screen-locked sprite oscillate
// one RT pixel against a smoothly scrolling background (the big-room walk shiver).
inline uintptr_t kSnapScaleA = 0x00BF941C;  // [VER]+[LIVE] float 1.0
inline uintptr_t kSnapScaleB = 0x00BF93E8;  // [VER]+[LIVE] float 2.0

// The CRT floor() every snap site calls. Each site is `mulss` by the scale product,
// `addss` a 0.5 constant, `call floor`, `divss` back - i.e. round-to-nearest on the RT
// grid. The sites are found at runtime by exactly that context (a direct E8 call to
// floor with a 0.5 addss and a kSnapScaleA reference in the preceding bytes), never by
// a hardcoded list: the compiler inlined the idiom dozens of times and a list would rot.
inline uintptr_t kCrtFloor = 0x00AF0917;    // [VER] double floor(double), cdecl

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
inline uintptr_t kConstHalfShared = 0x00BAA2D0;  // [VER] float 0.5, 2810 users
inline uintptr_t kConst065        = 0x00BAA324;  // [VER] float 0.65, world->screen
inline constexpr uintptr_t kPreviewHalfOperand[] = {      // [VER] instruction+4
    0x006B0F43, 0x006B0F4B,  // base Interpolate (Player, Bomb, Slot, + delegated)
    0x0065C583, 0x0065C5A7,  // Projectile
    0x00967F85, 0x00967FA5,  // Knife
    0x0097E761, 0x0097E781,  // Laser
    0x0067989A, 0x00679BE8,  // Tear
};

// ------------------------------------------------- KAGE graphics / overlay
// Everything here was read out of THIS binary, mostly by following the engine's own
// "Colormod Surface" path in sub_6FBC10 — the same sequence we imitate: create a
// procedural render target, push/bind it, draw, flush, pop, blit it full-screen.
inline uintptr_t kGraphicsManager   = 0x00C798E0;  // [VER] +4 == kGWindowFlags
inline uintptr_t kScreenWidth       = 0x00C79110;  // [VER] uint32, feeds every surface
inline uintptr_t kScreenHeight      = 0x00C79114;  // [VER] uint32
inline uintptr_t kOrthoWidth        = 0x00C78DC4;  // [VER] float, full-screen quad
inline uintptr_t kOrthoHeight       = 0x00C78EDC;  // [VER] float
inline uintptr_t kCurrentShader     = 0x00C379B8;  // [VER] written by kSelectShader (0 on 12 = overlay off)

// Manager fields, relative to kGraphicsManager.
inline constexpr uintptr_t kMgrBlendState     = 0x20;  // [VER] 5 dwords, reset by Clear()
inline constexpr uintptr_t kMgrCurRenderTgt   = 0x68;  // [VER] == 0x00C79948

// [VER] all four confirmed by disassembly, including their stack cleanup:
// Overlay path only. On 1.9.7.12 the KAGE render pipeline was refactored (Clear/Flush
// became vtable calls, CreateRenderTgt's arg order changed), so these are left 0 there
// and the overlay is disabled - core high-FPS + subpixel do not touch them.
inline uintptr_t kPushRenderTarget  = 0x00683680;  // no args, retn
inline uintptr_t kPopRenderTarget   = 0x006836C0;  // no args, retn ("stack is empty!")
inline uintptr_t kSetRenderTarget   = 0x00A18300;  // thiscall(mgr, target, screenSized), retn 8
inline uintptr_t kClearTarget       = 0x00A18230;  // thiscall(mgr), clears to (0,0,0,0)
inline uintptr_t kFlushBatches      = 0x00A19180;  // thiscall(mgr, 1), retn 4
inline uintptr_t kSelectShader      = 0x00A14050;  // stdcall(x) -> KAGE_ColorTextureShader
inline uintptr_t kCreateRenderTgt   = 0x00A11F30;  // stdcall(&out, w, h, name, &color), retn 0x14

// ImageBase vtable slots, byte offsets confirmed at the engine's own call sites.
inline constexpr int kVtImageRender    = 5;   // +0x14  Render(SourceQuad*, DestQuad*, KColor*)
inline constexpr int kVtImageSetFilter = 18;  // +0x48  SetFilterMode(min, mag)
inline constexpr uintptr_t kImgFilterCacheMin = 0x38;  // engine mirrors the filter mode here
inline constexpr uintptr_t kImgFilterCacheMag = 0x3C;

// LuaEngine::PostRender — where mods are given the finished world to draw on, and so the
// one place in the frame where compositing our own layer is guaranteed to be both legal
// and correctly ordered. Exactly one caller (0x006FC0A6, inside the render function), and
// its prologue `55 8B EC 6A FF` is 5 bytes on an instruction boundary.
inline uintptr_t kLuaPostRender     = 0x00863680;  // [VER]

// The rollback loop that undoes previews before the authoritative tick.
// Self-contained; iterates Game+0x125C (array) / +0x1264 (count).
inline constexpr uintptr_t kRollbackLoopBegin = 0x009551D8;  // [VER]
inline constexpr uintptr_t kRollbackLoopEnd   = 0x0095522A;  // [VER]

// Gate we replace. 0x9551CF `F6 87 BC AB 04 00 01` test byte [edi+4ABBC],1
//                  0x9551D6 `75 60`               jnz -> preview branch
// 9-byte window, nothing branches into it. [VER]
inline constexpr uintptr_t kGateParity = 0x009551CF;

// ---------------------------------------------------------- build selection
// Called once at init with the host exe's PE TimeDateStamp, before any address above is
// read. Leaves the 1.9.7.17 defaults in place for that build (and for any unknown stamp,
// where the caller must still validate every patch site by its bytes before writing).
// For 1.9.7.12 it retargets the addresses that moved. The wrapper-hook and limiter SITES
// are not here - those live in their build-specific patch functions in dllmain, because
// the code shape differs, not just the address.
inline void SelectBuild(uint32_t timeDateStamp) {
    if (timeDateStamp != kTimeDateStamp_1_9_7_12) return;   // defaults already = 1.9.7.17
    g_build = kBuild_1_9_7_12;
    g_isRepentogon = true;

    kGManagerPtr   = 0x00BE4A1C;
    kGameObjPtr    = 0x00BE49F4;
    kGameTick      = 0x00025A70;   // Game::_frameCount (Game.zhl)
    kMgrFrameCount = 0x0004A8B8;   // Manager::_framecount (Manager.zhl); the engine's own
                                   // parity gate reads test byte[mgr+0x4A8B8],1

    kConstHalfShared = 0x00B40178;
    kConst065        = 0x00B401CC;
    kSnapScaleA      = 0x00B88730;
    kSnapScaleB      = 0x00B883E8;
    kCrtFloor        = 0x00A882C2;
    kOrthoWidth      = 0x00BE6F2C;
    kOrthoHeight     = 0x00BE7298;
    kScreenWidth     = 0x00BE729C;
    kScreenHeight    = 0x00BE72A0;

    kGraphicsManager = 0x00BAE190;   // g_KAGE_Graphics_Manager
    kGWindowFlags    = 0x00BAE194;   // +4, as on 1.9.7.17
    kLuaPostRender   = 0x00849DD0;

    // Overlay pipeline: not ported (see the block that defines these). Zeroed so the
    // overlay refuses to install on this build; dllmain also forces LuaOverlay off.
    kCurrentShader   = 0;
    kClearTarget     = 0;
    kFlushBatches    = 0;
    kSelectShader    = 0;
    kCreateRenderTgt = 0;
    kSetRenderTarget = 0x009E80F0;   // resolved but unused while overlay is off
    kPushRenderTarget= 0x0067EDD0;
    kPopRenderTarget = 0x0067EEB0;
}

}  // namespace isaac
