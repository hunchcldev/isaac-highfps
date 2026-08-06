-- High FPS - Workshop companion
--
-- This mod does not make the game run faster. It can't. No Lua API reaches the render loop,
-- which is why the actual work lives in a native component you install by hand. All this
-- does is tell you whether that component is running, and show you the rate.
--
-- Detection is a global the native part writes into this Lua state:
--   HIGH_FPS_NATIVE  api version, present only while it is running
--   HIGH_FPS_RATE    frames per second it measured over the last second
--
-- It used to detect by counting MC_POST_RENDER, on the reasoning that more than 60 a second
-- was impossible without us. That stopped being true of our own mod: to keep mod code at the
-- cadence it was written for, the native part now holds that callback at 60 and composites
-- what mods draw onto the frames in between. Counting it would report "not installed" on a
-- working install, and would report 60 on a machine doing 180.

local mod = RegisterMod("High FPS Companion", 1)

local DOWNLOAD = "github.com/hunchcldev/isaac-highfps"
local NOTICE_SECONDS = 30      -- how long the "not installed" hint stays up

local sessionStart = Isaac.GetTime()

-- Off by default: an always-on overlay is clutter. Flip it on when you want to check.
local SHOW_RATES = false

mod:AddCallback(ModCallbacks.MC_POST_RENDER, function()
    -- Read every frame rather than once at load: the native part publishes on its first
    -- dispatch, which can land after mods have finished loading.
    local version = rawget(_G, "HIGH_FPS_NATIVE")

    if version then
        if SHOW_RATES then
            local rate = rawget(_G, "HIGH_FPS_RATE") or 0
            Isaac.RenderText(string.format("%d fps", rate), 40, 40, 1, 1, 1, 0.7)
        end
        return
    end

    -- The hint only appears when the native part is missing, and only briefly after
    -- launch, so it can never interrupt a run. Centred and scaled up rather than tucked
    -- into a corner, because the Workshop description is where people would normally read
    -- the address and Steam's link filter strips it out of there.
    if (Isaac.GetTime() - sessionStart) < NOTICE_SECONDS * 1000 then
        local w = Isaac.GetScreenWidth()
        local y = Isaac.GetScreenHeight() * 0.5 - 20
        local head = "High FPS needs a component the Workshop cannot ship"
        Isaac.RenderScaledText(head, (w - #head * 6) * 0.5, y, 1, 1, 1, 0.85, 0.35, 1)
        Isaac.RenderScaledText(DOWNLOAD, (w - #DOWNLOAD * 9) * 0.5, y + 18, 1.5, 1.5,
                               1, 1, 1, 1)
    end
end)

-- The game's own fortune paper, reused as the notice. A mod cannot open a browser at all:
-- vanilla Lua has no URL function and the os library is unavailable, so the address has to
-- be readable rather than clickable. The address sits on its own line so no wrap lands in
-- the middle of it.
--
-- Shown a moment after the run starts rather than immediately. Two things wipe it at frame
-- zero: the room load that follows MC_POST_GAME_STARTED, and the item banner this used to
-- show alongside it, which takes over the same slot.
local pendingNotice = -1

mod:AddCallback(ModCallbacks.MC_POST_GAME_STARTED, function()
    pendingNotice = rawget(_G, "HIGH_FPS_NATIVE") and -1 or 45   -- ~1.5 s at 30 Hz
end)

mod:AddCallback(ModCallbacks.MC_POST_UPDATE, function()
    if pendingNotice < 0 then return end
    pendingNotice = pendingNotice - 1
    if pendingNotice ~= 0 then return end
    Game():GetHUD():ShowFortuneText("High FPS is not installed.",
                                    "The mod you subscribed to",
                                    "only reports that. Get it at",
                                    DOWNLOAD)
end)
