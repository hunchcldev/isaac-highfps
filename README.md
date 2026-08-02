# isaac-highfps

Native high-refresh support for **The Binding of Isaac: Rebirth (Repentance+)**.

Isaac renders at a hard 60 FPS and simulates at 30 Hz. This mod lets the renderer run at
your display's refresh rate and fills the frames in between with real intermediate positions,
without changing gameplay speed by a single tick.

Built for build **v1.9.7.17** (`isaac-ng.exe`, 32-bit). It refuses to patch anything on a
build it does not recognise — see *Safety* below.

## Install

Drop `winmm.dll` next to `isaac-ng.exe`:

```
Steam\steamapps\common\The Binding of Isaac Rebirth\winmm.dll
```

That is the whole installation. **Uninstall = delete that file.**

The game imports `winmm` statically, so Windows loads the mod before the game's own entry
point runs. Every export of the real `winmm.dll` is forwarded through to the system copy, so
nothing else in the process notices. There is no injector, no launcher and no elevated
permissions involved.

This cannot ship on the Steam Workshop — the Workshop only accepts Lua content mods, and no
Lua API can change the render loop. Same situation as REPENTOGON: a Workshop item can point
at the download, but the native part has to be installed by hand.

## Workshop companion

`workshop/isaac-highfps/` is a small Lua mod for the Workshop. It does not speed anything up
and does not pretend to — it detects whether the native component is running and says so if it
is not, which is the only thing Lua can usefully contribute here.

Detection needs no interprocess machinery: `MC_POST_RENDER` fires once per rendered frame, and
vanilla is hard-capped at 60, so a sustained rate above that means the native part is live.

Publishing, in order:

1. `package.bat` builds `release\isaac-highfps-<ver>.zip` and leaves the companion ready.
2. Copy `workshop\isaac-highfps` into the game's `mods\` folder.
3. Run `tools\ModUploader\ModUploader.exe` from the game directory and upload it.
4. Steam assigns an id — paste it back into `metadata.xml` as `<id>`.
5. `metadata.xml` ships as `Private` on purpose. Publish the GitHub release **first**, then
   flip it to `Public`, otherwise subscribers land on a download that does not exist.

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

Three changes, all applied to the running process in memory — no files are modified.

**The frame limiter comes out.** `Isaac::IsaacMain` paces every iteration to exactly 1/60 s
with a `Sleep` plus a busy-spin. Two small patches make the loop skip that and run free.
Worth knowing: the constant is read from three places, not two — the third is a watchdog that
force-enables the limiter after 31 consecutive fast frames, and missing it silently puts the
cap back.

**The logic cadence is pinned.** The loop drives the simulation, so an unpaced loop would run
the game several times too fast. A trampoline hook on the engine's update wrapper calls it on
a real-time schedule at the original 60 Hz. Both of Isaac's clocks — the render frame counter
and the gameplay tick — therefore advance exactly as they always did, which matters because
roughly 52 places in the binary test the frame counter's parity and several more use it modulo
4, modulo 30, or shifted right by one.

**The frames in between get real positions.** Each entity's position is written directly,
using the same backup slot and "preview applied" marker the engine itself uses, so the engine's
own rollback undoes our work exactly as it would its own.

Everything except the player is **interpolated** between the previous and the current
authoritative position. Predicting instead — which is what the engine does for its single
half-step — looks fine over 1/60 s but not over a whole tick: Isaac's AI changes direction
constantly, the prediction turns out wrong, and the position snaps back when the real tick
lands. The player is the exception and is extrapolated, because a tick of latency on the
character you are steering is worse than a little imprecision.

We deliberately do **not** call the engine's own `Interpolate` method for these frames.
It is not a pure function — `Entity_Tear::Interpolate` alone is 2678 bytes, rewrites velocity,
forces the height field and calls 27 other functions. Running that several times per tick
corrupts real game state.

Interpolation switches itself off whenever the gameplay clock stops advancing — pause menu,
room transitions, cutscenes — because velocities stay frozen at non-zero there and
extrapolating them produces a very visible shudder.

## Safety

Every patch site is compared against its expected bytes before anything is written, and the
comparison accounts for ASLR relocation (the update wrapper's prologue contains an absolute
address that the loader rewrites, so a naive comparison against the on-disk bytes fails on
every launch). If any site does not match, nothing is patched at all and the game runs
completely normally.

The hooks are ordered so a partial failure cannot leave the game in a broken state: the cadence
hook goes in first, and the limiter only comes out once it is live. The reverse order — which
is what happened during development — gives you a game running at triple speed.

The per-frame work runs inside an exception handler that disables intermediate frames and logs
the fault rather than taking the game down with it.

## Building

Needs MSVC (x86) and Python 3 for the export-table generator.

```
build.bat
```

Produces `build\winmm.dll`. `tools\gen_proxy.py` regenerates the 192 forwarding thunks from
the real `winmm.dll` and only needs re-running if that file ever changes.

`src\inject.cpp` builds a small loader used during development. **It is not shipped** —
injecting into a running process trips antivirus and needs permissions the proxy does not.

## Credits and licence

MIT — see `LICENSE`.

No REPENTOGON code is used here. What was used is their **published AOB signatures**
(`libzhl/functions/*.zhl`), scanned against this build to recover real names for functions we
had already located by address. Those signatures are factual observations about a third-party
binary rather than a work we copied, and none of REPENTOGON's GPLv2 source is linked, vendored
or derived from. It still saved a great deal of time, so: thanks to the REPENTOGON team.

Worth recording that the match rate was 53 of 109, and that a unique match is *not* proof —
their published `Manager::Render` pattern matches a window-resize handler in this build. Every
recovered symbol here was cross-checked structurally before being trusted.

## Testing

`mods/fps-probe` is a tiny Lua mod that logs the real callback rates. The invariant that
matters:

```
[FPSProbe] update=30/s render=180/s
```

`update` must stay at 30/s no matter how high `render` goes. If it does not, gameplay speed
has changed and something is wrong.
