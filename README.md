# isaac-highfps

Native high-refresh support for The Binding of Isaac: Rebirth (Repentance+).

Isaac renders at a hard 60 FPS and simulates at 30 Hz. This mod lets the renderer run at your
display's refresh rate and fills the frames in between with real intermediate positions. Game
speed does not change by a single tick.

Built for build v1.9.7.17 (`isaac-ng.exe`, 32-bit). On a build it does not recognise it patches
nothing at all, see *Safety* below.

> **Beta.** Entity motion is verified: 180 FPS with the logic tick provably unchanged at 30/s.
> The camera is not. The room scroll offset still isn't interpolated, so if the camera judders
> while everything in it moves smoothly, that's why. Known gap, not a mystery. Please report it
> if you hit it.

## Install

Drop `winmm.dll` next to `isaac-ng.exe`:

```
Steam\steamapps\common\The Binding of Isaac Rebirth\winmm.dll
```

That's it. To uninstall, delete the file.

The game imports `winmm` statically, so Windows loads the mod before the game's own entry point
runs. Every export of the real `winmm.dll` is forwarded to the system copy, so nothing else in
the process notices. No injector, no launcher, and nothing asks for elevated permissions.

This can't ship on the Steam Workshop. The Workshop only accepts Lua content mods, and no Lua
API reaches the render loop. REPENTOGON has the same problem: a Workshop item can point at the
download, but the native part gets installed by hand.

## Workshop companion

`workshop/isaac-highfps/` is a small Lua mod for the Workshop. It doesn't speed anything up and
doesn't claim to. All it does is detect whether the native component is running and say so if it
isn't, which is about the only useful thing Lua can do here.

Detection needs no interprocess machinery. `MC_POST_RENDER` fires once per rendered frame and
vanilla is hard-capped at 60, so a sustained rate above that means the native part is live.

## Configuration

Optional. Create `isaac-highfps.ini` next to the DLL:

```ini
[highfps]
Enabled     = 1   ; 0 disables the mod without deleting it
Interpolate = 1   ; 0 = uncap the frame rate but do not add intermediate positions
MaxFps      = 0   ; 0 = whatever the display allows; otherwise a cap, e.g. 120
Log         = 1   ; writes %TEMP%\isaac-highfps.log
```

## How it works

Three changes, all applied to the running process in memory. No files are modified.

### The frame limiter comes out

`Isaac::IsaacMain` paces every iteration to exactly 1/60 s with a `Sleep` plus a busy-spin. Two
small patches make the loop skip that and run free.

One trap here cost us an afternoon: the 1/60 constant is read from three places, not two. The
third is a watchdog that force-enables the limiter after 31 consecutive fast frames. Patch only
the two obvious sites and the cap quietly comes back.

### The logic cadence is pinned

The loop drives the simulation, so an unpaced loop runs the game several times too fast. A
trampoline hook on the engine's update wrapper calls it on a real-time schedule at the original
60 Hz.

Both of Isaac's clocks, the render frame counter and the gameplay tick, then advance exactly as
they always did. That matters more than it sounds: roughly 52 places in the binary test the
frame counter's parity, and several more read it modulo 4, modulo 30, or shifted right by one.

### The frames in between get real positions

Each entity's position is written directly, reusing the same backup slot and "preview applied"
marker the engine uses itself, so the engine's own rollback undoes our work exactly as it would
undo its own.

Everything except the player is interpolated between its previous and current authoritative
position. Predicting instead is what the engine does for its single half-step, and over 1/60 s
that's fine. Over a whole tick it isn't: Isaac's AI changes direction constantly, the prediction
turns out wrong, and the position snaps back when the real tick lands. That snap is visible as
stutter. The player is the exception and gets extrapolated, because a tick of latency on the
character you're steering is worse than a little imprecision.

We deliberately don't call the engine's own `Interpolate` for these frames. It isn't a pure
function. `Entity_Tear::Interpolate` alone runs to 2678 bytes, rewrites velocity, forces the
height field and calls 27 other functions. Run that several times per tick and you corrupt real
game state.

## Safety

Every patch site is compared against its expected bytes before anything is written, and the
comparison accounts for ASLR relocation. The update wrapper's prologue contains an absolute
address that the loader rewrites, so a naive comparison against the on-disk bytes fails on every
single launch. If any site doesn't match, nothing is patched and the game runs normally.

The hooks go in in an order that a partial failure can't break: cadence hook first, limiter only
once that's live. Doing it the other way round, which is what happened during development, hands
you a game running at triple speed.

The per-frame work sits inside an exception handler. A fault there disables intermediate frames
and writes a log line instead of taking the game down.

## Building

Needs MSVC (x86) and Python 3 for the export-table generator.

```
build.bat
```

Produces `build\winmm.dll`. `tools\gen_proxy.py` regenerates the 192 forwarding thunks from the
real `winmm.dll`; you only need to re-run it if that file ever changes.

`src\inject.cpp` builds a small loader used during development. It is not shipped. Injecting
into a running process trips antivirus and needs permissions the proxy never asks for.

## Testing

`mods/fps-probe` is a tiny Lua mod that logs the real callback rates. The invariant that matters:

```
[FPSProbe] update=30/s render=180/s
```

`update` has to stay at 30/s no matter how high `render` goes. If it doesn't, game speed has
changed and something is wrong.

## Credits and licence

MIT, see `LICENSE`.

No REPENTOGON code is used here. What was used is their published AOB signatures
(`libzhl/functions/*.zhl`), scanned against this build to recover real names for functions we
had already found by address. Those signatures are factual observations about someone else's
binary rather than a work we copied, and none of REPENTOGON's GPLv2 source is linked, vendored
or derived from. It saved a lot of time all the same, so thanks to the REPENTOGON team.

For the record: 53 of 109 signatures matched, and a unique match is not proof. Their published
`Manager::Render` pattern matches a window-resize handler in this build. Every symbol recovered
that way was cross-checked structurally before it was trusted.
