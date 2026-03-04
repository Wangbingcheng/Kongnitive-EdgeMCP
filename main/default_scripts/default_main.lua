package.path = package.path .. ";/spiffs/?.lua"

local di_container = require("di_container")
local bindings = require("bindings")

local instances = {}

for iface, cfg in pairs(bindings) do
    local provider_name = cfg.provider
    local provider_module = "provider_" .. provider_name
    di_container.provide(provider_name, require(provider_module).factory)
    di_container.bind(iface, provider_name, cfg.opts)
end

local sensor
for iface, instance in pairs(instances) do
    if type(instance.read) == "function" then
        sensor = instance
        break
    end
end

if not sensor then
    log.info("No sensor found")
    return
end

-- log.info("main.lua heap monitor started")

-- local OA = 0x3C
-- local SDA = 5
-- local SCL = 6
-- local FQ = 400000

-- local function cmd(v)
--   i2c.write(OA, 0x00, v)
-- end

-- local function set_pos(col, page)
--   cmd(0xB0 | (page & 0x07))
--   cmd(col & 0x0F)
--   cmd(0x10 | ((col >> 4) & 0x0F))
-- end

-- local function init_oled()
--   i2c.setup(SDA, SCL, FQ)
--   local seq = {
--     0xAE,0xA8,0x3F,0xD3,0x00,0x40,0xA1,0xC8,0xDA,0x12,
--     0x81,0xCF,0xA4,0xA6,0xD5,0x80,0xD9,0xF1,0xDB,0x40,
--     0x8D,0x14,0x20,0x00,0xAF
--   }
--   for _, v in ipairs(seq) do cmd(v) end
-- end

-- local FONT = {
--   [" "] = {0x00,0x00,0x00,0x00,0x00},
--   ["."] = {0x00,0x60,0x60,0x00,0x00},
--   ["-"] = {0x08,0x08,0x08,0x08,0x08},
--   ["0"] = {0x3E,0x51,0x49,0x45,0x3E},
--   ["1"] = {0x00,0x42,0x7F,0x40,0x00},
--   ["2"] = {0x42,0x61,0x51,0x49,0x46},
--   ["3"] = {0x21,0x41,0x45,0x4B,0x31},
--   ["4"] = {0x18,0x14,0x12,0x7F,0x10},
--   ["5"] = {0x27,0x45,0x45,0x45,0x39},
--   ["6"] = {0x3C,0x4A,0x49,0x49,0x30},
--   ["7"] = {0x01,0x71,0x09,0x05,0x03},
--   ["8"] = {0x36,0x49,0x49,0x49,0x36},
--   ["9"] = {0x06,0x49,0x49,0x29,0x1E},
--   ["B"] = {0x7F,0x49,0x49,0x49,0x36},
--   ["F"] = {0x7F,0x09,0x09,0x09,0x01},
--   ["H"] = {0x7F,0x08,0x08,0x08,0x7F},
--   ["K"] = {0x7F,0x08,0x14,0x22,0x41},
--   ["L"] = {0x7F,0x40,0x40,0x40,0x40},
--   ["P"] = {0x7F,0x09,0x09,0x09,0x06},
--   ["R"] = {0x7F,0x09,0x19,0x29,0x46},
--   ["S"] = {0x46,0x49,0x49,0x49,0x31},
--   ["T"] = {0x01,0x01,0x7F,0x01,0x01},
--   ["U"] = {0x3F,0x40,0x40,0x40,0x3F}
-- }

-- local function write_line(page, text)
--   set_pos(0, page)
--   local data = {0x40}
--   local idx = 2
--   for i = 1, #text do
--     local ch = text:sub(i, i)
--     local glyph = FONT[ch] or FONT[" "]
--     for col = 1, 5 do
--       if idx > 129 then break end
--       data[idx] = glyph[col]
--       idx = idx + 1
--     end
--     if idx > 129 then break end
--     data[idx] = 0x00
--     idx = idx + 1
--     if idx > 129 then break end
--   end
--   while idx <= 129 do
--     data[idx] = 0x00
--     idx = idx + 1
--   end
--   i2c.write(OA, data)
-- end

-- local function clear_all()
--   for page = 0, 7 do
--     write_line(page, "")
--   end
-- end

-- local function safe_call(fn, fallback)
--   local ok, value = pcall(fn)
--   if ok and value ~= nil then
--     return value
--   end
--   return fallback
-- end

-- init_oled()
-- clear_all()

-- local base_free = safe_call(system.heap_free, 0)

-- while true do
--   local heap_free = safe_call(system.heap_free, 0)
--   local runtime_used = base_free - heap_free
--   if runtime_used < 0 then runtime_used = 0 end

--   local lua_kb = collectgarbage("count")
--   local uptime_sec = safe_call(system.uptime, 0)

--   write_line(0, string.format("HF %5.1fKB", heap_free / 1024.0))
--   write_line(2, string.format("RT %5.1fKB", runtime_used / 1024.0))
--   write_line(4, string.format("LU %5.1fKB", lua_kb))
--   write_line(6, string.format("UP %6dS", math.floor(uptime_sec + 0.5)))

--   time.sleep_ms(1000)
-- end






local base_heap = system.heap_free() or 0
local last_sensor_read = 0
local temp = "N/A"
local hum = "N/A"

while true do
    local heap_free = system.heap_free() or 0
    local runtime_used = base_heap - heap_free
    if runtime_used < 0 then runtime_used = 0 end
    
    local lua_kb = collectgarbage("count")
    local uptime_sec = system.uptime() or 0
    
    if uptime_sec - last_sensor_read >= 5 then
        local data = sensor:read()
        if data then
            temp = string.format("%.1f", data.temperature)
            hum = string.format("%.1f", data.humidity)
        else
            temp = "N/A"
            hum = "N/A"
        end
        last_sensor_read = uptime_sec
    end
    
    log.info(string.format("HF: %.1fKB  RT: %.1fKB  LU: %.1fKB  UP: %ds  T: %sC  H: %s%%", 
        heap_free / 1024, runtime_used / 1024, lua_kb, math.floor(uptime_sec + 0.5),
        temp, hum))
    
    time.sleep_ms(1000)
end
