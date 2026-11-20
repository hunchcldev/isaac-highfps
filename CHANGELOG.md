# Changelog

## Unreleased

**The REPENTOGON build of the game is supported.** The launcher does not run the Steam
executable; it keeps a patched copy (v1.9.7.12.J273) in `Repentogon\` and starts that. It is
a different compilation — every image address moved and the frame limiter and update
wrapper have a different shape — so the DLL now reads the PE timestamp of whichever
executable loaded it and selects the address set to match. Struct offsets are the same on
both. The Lua overlay is not ported to that build, so mod callbacks run at the display rate
there (the gate only holds Lua back while the overlay is live).

**Why v0.13.0 could not go in that folder, and why that looked like a launcher bug.** Put
next to the Repentogon copy, v0.13.0 made the launcher print `Error encountered while
injecting Repentogon` and then `Unable to kill Isaac process: 5`. Neither line is about
the launcher. The launcher starts the game suspended and injects from a remote thread;
loading the executable's static imports — us — happens on that thread, and our startup
thread then went looking for the manager pointer at its 1.9.7.17 address, 0x8CC80 bytes
above where the 1.9.7.12 image keeps it. On that image the address lands inside `.bind`,
the Steam stub, and holds `0x174E5452`; that passed the `>= 0x10000` sanity check, and
the very next line dereferenced it at `0x1753000E` for the frameCount log. An unhandled
fault on any thread ends the process, and the process died while the launcher's thread
was still inside it. The launcher saw its remote
thread end with an exception code and tried to kill a process that was already gone;
error 5 is what `TerminateProcess` returns for exactly that. The byte checks that are
meant to refuse an unknown build never ran, because the thread that would have run them
was the one that faulted.

**Room transitions and the main menu run at the display rate too.** Both were held at 60
on purpose: the loop rests when the gameplay clock stands still, and it stands still during
both. Resting there was the right call as long as the extra frames would have been
duplicates — the slide between rooms is a counter `Game::Update` steps 30 times a second
(`RoomTransition::_progress`, eased in its Render as 3t² − 2t³), and the menu camera
(`MenuManager::_ViewPosition`) is eased once per 60 Hz frame. Now both are previewed on
every rendered frame the way the room scroll is — sampled at real frames, interpolated
between the last two samples before each render, restored before the engine runs — and the
loop keeps its full rate while either is moving. Interpolation rather than extrapolation
because an overshoot on the slide's last step would show and one frame of lag does not; a
cut (page switch, the transition's progress reset) is detected and left as a cut.
`SceneMotion = 0` turns it off. Not covered, because they are keyframed animations rather
than motions: the pause menu, HUD slides, the nightmare and boss-intro sequences.

**The light rays glimmer at their old pace again (#9).** Cathedral's light shafts, and the
rays some Basement rooms roll, pulsed 2.75x too fast at 165 Hz. Not everything the engine
animates lives in the update: `FXLayers::Render` steps every ray's phase by its speed on
each call, and `Room::Render` calls it once per rendered frame — a 60 Hz animation only
because vanilla rendered 60 times a second. The brightness is `(sin(phase) + 1) / 2`. The
three-instruction advance is now lifted into a stub that runs it on real frames and skips
it on intermediate ones, on both builds (`RayCadence = 0` restores the old behaviour).
Anything else the engine animates from a render function would get the same treatment;
this is the one the report named, and the one the stage overlay code contains.

**A key press is reported once, not once per rendered frame (#10).** Mod Config Menu's
cursor skipped several entries per press on the Repentogon build. MCM navigates from
`MC_POST_RENDER` with `Input.IsActionTriggered`, and "triggered" is an edge of the 60 Hz
input poll: it stays true until the next poll. Vanilla renders exactly once per poll, so a
render callback sees each press once; we render two or three times per poll, so without
the cadence gate it saw each press two or three times. On 1.9.7.17 the gate hid this for
the mod render pass; on 1.9.7.12 the gate is idle (no overlay), and the per-entity render
callbacks never had the protection on either build. `Input.IsActionTriggered` and
`Input.IsButtonTriggered` are now wrapped inside the VM to answer `false` on a frame the
engine never polled for, which is the vanilla semantic restored rather than a heuristic.
Held state is untouched. `HIGH_FPS_INTERMEDIATE` is published alongside, so a mod that
counts render frames can do the same for itself. `InputEdgeGate = 0` turns it off. What
this does not fix: MCM's hold-to-repeat counts render frames, so on 1.9.7.12 holding a key
still scrolls faster than at 60 until the overlay is ported to that build.

**Unknown builds are now a no-op, as the README always claimed.** Every build-specific
address is checked to be mapped before its first read, not just compared before its first
write: the manager slot, the manager pointer it yields, every patch site and both wrapper
hook shapes. The sub-pixel scan takes its `.text` bounds from the section table instead of
a 1.9.7.17 constant, so it cannot run on into `.rdata` on a build whose code ends earlier.

## v0.13.0

**The companion can now tell you the DLL is outdated.** The two halves of this mod age
differently: the Workshop part updates itself, the native part is installed by hand and
then silently falls behind. The DLL now publishes its release version into the Lua state
(`HIGH_FPS_VERSION`), and the companion — which is re-uploaded with every native release
and therefore knows the newest one — shows a short fortune at run start when the installed
DLL is older. Builds before 0.13.0 never published a version, so its absence identifies
them exactly and the notice covers them too.

The version string itself now lives in one place, `src/version.h`: the DLL publishes it,
package.bat names the zip after it, and package.bat reminds us to bump the companion —
because a version scheme where the two halves can drift apart quietly is how the previous
"detected by counting callbacks" scheme died.

**The companion no longer cries wolf.** "High FPS is not installed" could appear although
the DLL was running: the native part announces itself on its first Lua dispatch, which on
a slow disk lands seconds after mods have loaded, and the companion believed whatever it
saw first. It now waits a few seconds before showing the launch hint, re-checks at the
moment the run-start notice would fire instead of trusting a snapshot, and cancels any
queued notice the instant the native part appears.

## v0.12.0

**The big-room shiver is fixed.** Walking through a room large enough to scroll made Isaac
himself tremble while everything around him moved smoothly — only him, only while moving,
only while the camera scrolled. Two separate causes, and both had to fall.

The first: the mod was smoothing the wrong field. What we previewed each frame was the
camera's position — but that value is only the camera's internal focus. Once per 60 Hz
frame the engine maps it through the world-to-screen transform, clamps it to the room, and
stores the result in the room's scroll offset, and it is THAT field every sprite draw
actually adds. Our per-frame camera writes never reached the screen: the scroll offset
froze for three rendered frames while the player's position advanced on every one, so he
crept forward and snapped back sixty times a second. The preview now samples, advances and
rolls back the scroll offset itself — the value the renderer consumes — so the scroll
finally moves at the full frame rate. (Proving this took measuring the field's update
cadence in the running game: 60 Hz, no matter what we wrote into the camera. A lesson worth
the price: never assume the field you write is the field the renderer reads.)

The second: the renderer quantises every sprite's position term to the render-target pixel
grid BEFORE adding the scroll offset. Advancing the offset by the player's raw sub-frame
movement would therefore hold his true position while his drawn position kept stepping
whole pixels each time his world position crossed a grid line — a one-pixel sawtooth
against a smooth background, visible precisely because a screen-locked sprite has no
motion of its own to hide it. The offset therefore carries the SNAPPED player term,
anchored so that staircase and ramp step in the same instant: the drawn player holds one
render-target pixel exactly, and the background scrolls in whole pixels at the full frame
rate — the finest motion a pixel-snapped world can show.

**A per-frame diagnostic dump.** `Diag = 1` in the ini logs, for every rendered frame, the
exact player/scroll state the renderer is about to read. It exists because probing the
game from outside answers a different question: an external reader races the mod's own
write sequence and mostly catches the sub-millisecond windows between two writes. Off by
default; costs log I/O when on.

## v0.11.0

**A second name to load under.** No log file and no change in frame rate means the loader never
picked the DLL up, and there is nothing the DLL can report about a situation where its own code
never runs. The download now carries the same build twice, the second copy named `dbghelp.dll`
in `alternative-name/`. The executable imports that as well, so it loads at the same point in
startup. Use one or the other, never both; there is a single-instance lock so a double install
does nothing rather than something strange.

`dbghelp` was picked over the other candidates on purpose. Of everything `isaac-ng.exe` imports,
most are on Windows' KnownDLLs list and cannot be shadowed from the game folder at all. Of the
rest, `dbghelp` pulls only five functions, is not tangled up with the graphics stack the way
`opengl32` is, and is not the sort of file antivirus software takes an interest in.

**Log paths handle any locale.** They were ANSI, which fails silently when a profile or install
path holds characters the system codepage cannot carry. A failed open left logging switched off
entirely, including the fatal lines that would have said why nothing was patched, so the symptom
was "no log at all" — the same thing a DLL that never loaded looks like. Wide paths throughout
now, with a fallback next to the DLL if `%TEMP%` cannot be written.

## v0.10.0

**Mods keep working at high frame rates.** Uncapping the render loop meant the engine asked every
installed mod to draw at the display rate instead of 60 times a second. That tripled the Lua cost
of every mod present, and it broke mods that keep counters or timers in a render callback, which
was for a long time the only per-frame hook the API offered.

The mod render pass is now held at its original cadence. Engine calls into Lua go through
`lua_pcallk` and `lua_callk`, both swapped in the import table; on a frame the engine would never
have drawn, the call is emulated rather than forwarded. The callee and its arguments come off the
Lua stack, the requested number of nils go on, and `LUA_OK` comes back, which is exactly what
every call site already sees when no mod has registered anything.

That alone would leave mod graphics on one frame in three, which reads as flicker. So what mods
draw goes onto a render target of its own, composited onto every frame from inside
`LuaEngine::PostRender`. The engine's own surfaces and blit are reused, so nothing here is drawn
by us. Compositing at `SwapBuffers` was the first attempt and faulted immediately: by then the
frame's render batches are already torn down.

Per-entity render callbacks deliberately keep the full rate. They are cheap, and they draw
relative to entities whose positions are already being interpolated, so per frame is both correct
and better looking. Bracketing those too was the first attempt and cost most of the frame rate:
a batch is drawn into whatever target is bound when it is flushed, not when it was queued, so
redirecting them meant flushing the queue dozens of times a frame. That also reordered the
engine's own draws enough to make the game's UI flicker.

One consequence worth naming: a mod drawing in both the main pass and a per-entity callback used
to see them once each per frame and now sees the main pass less often. `LuaVanillaCadence = 0`
turns the whole mechanism off.

**Mods can detect the native component.** Two globals, `HIGH_FPS_NATIVE` and `HIGH_FPS_RATE`,
published into the Lua state from inside a dispatch. The Workshop companion used to count render
callbacks above 60, which the change above makes impossible.

**The refresh rate requirement is documented.** The frame limiter is removed but the OpenGL swap
interval is untouched, so vsync still pins the loop to the display. On a 60 Hz panel there is
nothing above 60 to show.

## v0.9.0

First release. Renderer runs at the display's refresh rate, logic stays at 30 Hz, and the frames
in between get interpolated entity positions rather than extrapolated guesses.
