-- High FPS - Workshop companion
--
-- This mod does not make the game run faster. It cannot: no Lua API can change the render
-- loop, which is why the actual work lives in a native component installed by hand.
-- What this does is tell you whether that component is running, and show you the rates.
--
-- Detection needs no interprocess anything: MC_POST_RENDER fires once per rendered frame,
-- so if it fires appreciably more than 60 times a second, the native part is live.

local mod = RegisterMod("High FPS Companion", 1)

local DOWNLOAD = "github.com/hunchcldev/isaac-highfps"
local NOTICE_SECONDS = 20      -- how long the "not installed" hint stays up
local DETECT_THRESHOLD = 70    -- renders/s above vanilla's hard 60 cap

local renders, updates = 0, 0
local windowStart = Isaac.GetTime()
local sessionStart = windowStart
local renderRate, updateRate = 0, 0
local detected = false

-- Off by default: an always-on overlay is clutter. Flip it on when you want to check.
local SHOW_RATES = false

mod:AddCallback(ModCallbacks.MC_POST_UPDATE, function()
    updates = updates + 1
end)

mod:AddCallback(ModCallbacks.MC_POST_RENDER, function()
    renders = renders + 1

    local now = Isaac.GetTime()
    local elapsed = now - windowStart
    if elapsed >= 500 then
        renderRate = math.floor(renders * 1000 / elapsed + 0.5)
        updateRate = math.floor(updates * 1000 / elapsed + 0.5)
        renders, updates, windowStart = 0, 0, now
        if renderRate >= DETECT_THRESHOLD then
            detected = true
        end
    end

    if SHOW_RATES and detected then
        Isaac.RenderText(string.format("%d fps  (logic %d/s)", renderRate, updateRate),
                         40, 40, 1, 1, 1, 0.7)
    end

    -- The hint only appears when the native part is missing, and only briefly after
    -- launch, so it can never interrupt a run.
    if not detected and (now - sessionStart) < NOTICE_SECONDS * 1000 then
        Isaac.RenderText("High FPS: native component not installed", 40, 40, 1, 0.8, 0.3, 1)
        Isaac.RenderText(DOWNLOAD, 40, 52, 1, 0.8, 0.3, 1)
    end
end)
