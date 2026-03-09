package.path = package.path .. ";/spiffs/?.lua"

local di_container = require("di_container")
local bindings = require("bindings")

local instances = {}

for iface, cfg in pairs(bindings) do
    local provider_name = cfg.provider
    local provider_module = "provider_" .. provider_name
    di_container.provide(provider_name, require(provider_module).factory)
    di_container.bind(iface, provider_name, cfg.opts)
    instances[iface] = di_container.resolve(iface)
end

local display
for iface, instance in pairs(instances) do
    log.info("Checking instance:", iface, instance)
    if type(instance.show_status) == "function" then
        display = instance
        log.info("Found display, calling init...")
        display:init()
        break
    end
end

if not display then
    log.info("No display found")
    return
end

log.info("Starting display sequence...")

local base_heap = system.heap_free() or 0

local stage = 1
local stage_start = system.uptime() or 0
local stage1_done = false
local stage2_done = false

local last_frame = system.uptime()
local frame_interval = 0.03   -- 30ms = 33 FPS
local color_index = 0
while true do
    local uptime = system.uptime()
    local elapsed = uptime - stage_start

    if stage == 1 then
        if not stage1_done then
            display:test_pattern(color_index)
            color_index = color_index + 1
            
        end
        if elapsed >= 5 then
            stage = 2
            stage_start = uptime
        else
            time.sleep_ms(100)
        end

    elseif stage == 2 then
        if not stage2_done then
            display:show_welcome()

        end
        if elapsed >= 2 then
            stage = 3
            stage_start = uptime
        else
            time.sleep_ms(100)
        end

    elseif stage == 3 then
        local now = system.uptime()
        if now - last_frame >= frame_interval then
            last_frame = now

            local heap_free = system.heap_free() or 0
            local runtime_used = base_heap - heap_free
            if runtime_used < 0 then runtime_used = 0 end
            local lua_kb = collectgarbage("count")

            display:show_status(
                heap_free / 1024,
                runtime_used / 1024,
                lua_kb,
                math.floor(now + 0.5)
            )
        else
            time.sleep_ms(10)
        end
    end
end