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
    if type(instance.show_status) == "function" then
        display = instance
        break
    end
end

if not display then
    log.info("No display found")
    return
else
    display:init()
end

local base_heap = system.heap_free() or 0
local last_gc = 0
local last_trim = 0

local stage = 1
local stage_start = system.uptime() or 0
local stage1_done = false
local stage2_done = false

local last_frame = system.uptime()
local frame_interval = 0.03
local last_status = 0
local status_interval = 2.0

while true do
    local now = system.uptime()
    local elapsed = now - stage_start

    if stage == 1 then
        if not stage1_done then
            display:test_pattern(0)
            stage1_done = true
        end
        if elapsed >= 5 then
            stage = 2
            stage_start = now
        else
            time.sleep_ms(1000)
        end

    elseif stage == 2 then
        if not stage2_done then
            display:show_welcome()
            stage2_done = true
        end
        if elapsed >= 2 then
            stage = 3
            stage_start = now
        else
            time.sleep_ms(100)
        end

    elseif stage == 3 then
        if now - last_frame >= frame_interval then
            last_frame = now

            if now - last_status >= status_interval then
                last_status = now
                local heap_free = system.heap_free() or 0
                local runtime_used = base_heap - heap_free
                if runtime_used < 0 then runtime_used = 0 end

                display:show_status(
                    heap_free / 1024,
                    runtime_used / 1024,
                    collectgarbage("count"),
                    math.floor(now + 0.5)
                )
            end

            if now - last_gc >= 10 then
                last_gc = now
                collectgarbage("collect")
            end

            if now - last_trim >= 30 then
                last_trim = now
                system.trim_heap()
            end
        else
            time.sleep_ms(10)
        end
    end
end