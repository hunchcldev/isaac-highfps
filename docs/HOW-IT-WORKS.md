# How isaac-highfps works

The README covers installing and using the mod. This is what sits underneath: what is changed in
the running game, why other mods keep working, and what protects the game when something does not
match.

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

### Menus and room transitions

Two more things move while the gameplay clock stands still, and both are plain engine fields the
renderer reads as they are:

- **The slide between rooms.** `RoomTransition` lives inline in `Game`; `Game::Update` steps its
  counter once per 30 Hz tick and stores `progress = counter / duration`, and
  `RoomTransition::Render` eases that (3t² − 2t³) into the offset both rooms are drawn at. Vanilla
  draws every step twice. The progress is previewed per rendered frame between its last two
  values, so the slide moves at the display rate.
- **The main menu's camera.** `MenuManager::_ViewPosition` is eased toward the selected page once
  per 60 Hz frame and added to everything the menu draws. Same treatment, on the frame cadence.

Interpolation, not extrapolation: one frame of latency on a slide is invisible and an overshoot on
its final step is not — and the ease-out means the step we lag behind by is nearly nothing. A cut
(a page switch, the transition's progress reset) is detected and shown as a cut.

The loop used to rest at 60 whenever the gameplay clock stood still for a quarter second, which is
exactly what happens during both of these. It now keeps its full rate while either is moving, and
still rests when nothing is.

What this does *not* smooth: things that are animations rather than motions. The pause menu's
appear/disappear, the HUD's pickup slides, the nightmare and boss-intro sequences and every menu
sprite are ANM2 animations stepping through keyframes, and loading screens are static. Giving
those more frames would need interpolating animation keyframes themselves (what REPENTOGON's
"InterpolV2" does for its half-step), which is a different project; with `SceneMotion` on, those
moments simply run at the engine's own cadence.

### What the renderer animates on its own

Not everything the engine animates lives in the update. The stage overlay (`FXLayers::Render`,
Cathedral's light shafts and the rays some Basement rooms roll) steps each ray's phase by its
speed every time it is *drawn*, and the brightness is `(sin(phase) + 1) / 2`. That was a 60 Hz
animation only because vanilla drew 60 times a second; free-running, it glimmered 2.75× too fast
at 165 Hz. The three-instruction advance is lifted into a stub that runs it on real frames only.
It is the one render-side animation found in the overlay code; anything similar elsewhere would
get the same treatment.

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

## Mod compatibility

Uncapping the render loop means the engine asks every installed mod "do you want to draw?" at the
display rate instead of 60 times a second. That triples the Lua cost of every mod you have, and it
breaks mods that keep counters or timers in their render callback, which was for a long time the
only per-frame hook the api offered.

So the mod render pass is held at its original 60 Hz. Every engine call into Lua goes through two
imported functions, `lua_pcallk` and `lua_callk`, both swapped in the import table. On a frame the
engine would never have drawn, that pass is not forwarded to Lua at all: the call is emulated
instead, popping the callee and its arguments off the Lua stack, pushing the requested number of
nils and returning `LUA_OK`. That is indistinguishable from no mod having registered anything, a
case every call site already handles.

That alone would mean mod graphics existed on one frame in three, which reads as flicker. So what
mods draw goes onto a surface of its own, and that surface is composited onto every frame. The
engine already has the machinery, because it uses render targets for its own bloom, pixelation and
colormod passes, so nothing here is drawn by us. Mod graphics are present on every frame while
their Lua still runs 60 times a second.

The per-entity render callbacks are left alone and run at the full rate. They are cheap, and they
draw relative to entities whose positions we are already interpolating, so running them per frame
means they track that movement instead of being pinned to 60 Hz.

One consequence worth naming: a mod drawing in both the main pass and a per-entity callback used
to see them once each per frame and now sees the main pass less often. `LuaVanillaCadence = 0`
turns the whole thing off and gives every callback the display rate, at the cost above.

Holding Lua back is only ever right while the layer that carries its output is live, so with the
overlay off (by config, after a fault, or on the Repentogon build where it is not ported) the gate
stays installed but lets every dispatch through. Mod callbacks then run at the display rate, the
pre-v0.10.0 behaviour. The log says which of the two you are getting.

## For mod authors

The DLL publishes these globals into the Lua state:

```lua
HIGH_FPS_NATIVE        -- api version (3); present only while frames are actually being added
HIGH_FPS_STATUS        -- "starting", "active" or "unsupported" (this game version is not known
                       -- to the DLL, nothing was patched); absent on 0.13.0 and older
HIGH_FPS_VERSION       -- release version, e.g. "0.14.0"; absent means 0.12.0 or older
HIGH_FPS_RATE          -- frames per second measured over the last second
HIGH_FPS_INTERMEDIATE  -- true while the frame being rendered is one vanilla never drew
```

`HIGH_FPS_NATIVE` is the one to test if you want to know whether high-FPS rendering is on.
`HIGH_FPS_STATUS` and `HIGH_FPS_VERSION` are also there when the DLL is loaded but idle.

Read them per frame rather than once at load, since they appear on the first dispatch and that
can land after mods have finished loading.

Counting `MC_POST_RENDER` to work out the frame rate does not work here: that callback is
deliberately held at 60 no matter how fast the game renders. `HIGH_FPS_RATE` exists because a
mod can no longer measure the rate for itself.

`HIGH_FPS_INTERMEDIATE` is for callbacks that do run per rendered frame (the per-entity render
callbacks everywhere; every render callback on the REPENTOGON copy). Anything that counts
frames or reads input can skip the work when it is true and behave exactly as it did at 60.
The DLL already does this for the two input edge functions: `Input.IsActionTriggered` and
`Input.IsButtonTriggered` answer `false` on intermediate frames, because the engine polls input
60 times a second and a press it reported on the real frame would otherwise be reported again
on every frame rendered before the next poll. Held state (`IsActionPressed`,
`IsButtonPressed`) is untouched. `InputEdgeGate = 0` in the ini turns this off.

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

## Why v0.13.0 and older break the REPENTOGON launcher

That copy is a different compilation of the game, and only builds of this mod that know it
(this source; anything newer than v0.13.0) can go there. **Do not put v0.13.0 or older in
`Repentogon\`.** Those builds know the Steam executable's addresses only, and the very first
thing they read — the engine's manager pointer — sits 0x8CC80 bytes away from where the
Repentogon copy keeps it, inside that image's Steam stub. What is there (`0x174E5452`) looks
enough like a pointer to pass the sanity check, and the next line dereferences it. The DLL's
startup thread dies, which takes the whole game process down, and it does so while the launcher
is still injecting into the suspended game. What you see is the launcher reporting

```
[ERROR] Error encountered while injecting Repentogon, check log files
[ERROR] Unable to kill Isaac process: 5
```

The second line is the tell: error 5 is what `TerminateProcess` returns for a process that has
already died. `isaac-highfps.log` from such a launch stops within a line or two of
`host exe = ...\Repentogon\isaac-ng.exe`, before any `installed` line.
Deleting the DLL from `Repentogon\` (or replacing it with a build that knows that executable)
fixes it; the main-folder install is unaffected either way.

## Testing

`mods/fps-probe` is a tiny Lua mod that logs the real callback rates. The invariant that matters:

```
[FPSProbe] update=30/s render=180/s
```

`update` has to stay at 30/s no matter how high `render` goes. If it doesn't, game speed has
changed and something is wrong.
