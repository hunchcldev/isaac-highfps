# Changelog

## v0.11.1

**The second name is `opengl32`, not `dbghelp`.** A crash handler can leave a real `dbghelp.dll`
in the game folder, and since the game directory comes before System32 in the search order the
game is already loading it. Our copy would shadow it and forward only to the system dbghelp, so
whatever that file did would stop working. `opengl32` in the folder is normally just the loader's
own from System32, so overwriting it with a forwarding proxy is safe. The fallback lives in
`alternative-name/opengl32.dll`.

## v0.11.0

**A second name to load under.** No log file and no change in frame rate means the loader never
picked the DLL up, and there is nothing the DLL can report about a situation where its own code
never runs. The download carries the same build twice, the second copy in `alternative-name/`.
The executable imports it as well, so it loads at the same point in startup. Use one or the
other, never both; there is a single-instance lock so a double install does nothing rather than
something strange.

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
