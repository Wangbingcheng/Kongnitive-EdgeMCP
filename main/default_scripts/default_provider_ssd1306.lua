local P = {}

local DC_CMD = 0
local DC_DATA = 1

local FONT = {
    [" "] = {0x00,0x00,0x00,0x00,0x00},
    ["."] = {0x00,0x60,0x60,0x00,0x00},
    [":"] = {0x00,0x36,0x36,0x00,0x00},
    ["-"] = {0x08,0x08,0x08,0x08,0x08},
    ["0"] = {0x3E,0x51,0x49,0x45,0x3E},
    ["1"] = {0x00,0x42,0x7F,0x40,0x00},
    ["2"] = {0x42,0x61,0x51,0x49,0x46},
    ["3"] = {0x21,0x41,0x45,0x4B,0x31},
    ["4"] = {0x18,0x14,0x12,0x7F,0x10},
    ["5"] = {0x27,0x45,0x45,0x45,0x39},
    ["6"] = {0x3C,0x4A,0x49,0x49,0x30},
    ["7"] = {0x01,0x71,0x09,0x05,0x03},
    ["8"] = {0x36,0x49,0x49,0x49,0x36},
    ["9"] = {0x06,0x49,0x49,0x29,0x1E},
    ["A"] = {0x7F,0x09,0x09,0x09,0x06},
    ["B"] = {0x7F,0x49,0x49,0x49,0x36},
    ["C"] = {0x3E,0x41,0x41,0x41,0x22},
    ["D"] = {0x7F,0x41,0x41,0x41,0x3E},
    ["E"] = {0x7F,0x49,0x49,0x49,0x41},
    ["F"] = {0x7F,0x09,0x09,0x09,0x01},
    ["G"] = {0x3E,0x41,0x49,0x49,0x3A},
    ["H"] = {0x7F,0x08,0x08,0x08,0x7F},
    ["I"] = {0x41,0x41,0x7F,0x41,0x41},
    ["J"] = {0x40,0x40,0x40,0x40,0x7F},
    ["K"] = {0x7F,0x08,0x14,0x22,0x41},
    ["L"] = {0x7F,0x40,0x40,0x40,0x40},
    ["M"] = {0x7F,0x02,0x0C,0x02,0x7F},
    ["N"] = {0x7F,0x04,0x08,0x10,0x7F},
    ["O"] = {0x3E,0x41,0x41,0x41,0x3E},
    ["P"] = {0x7F,0x09,0x09,0x09,0x06},
    ["Q"] = {0x3E,0x41,0x51,0x21,0x5E},
    ["R"] = {0x7F,0x09,0x19,0x29,0x46},
    ["S"] = {0x46,0x49,0x49,0x49,0x31},
    ["T"] = {0x01,0x01,0x7F,0x01,0x01},
    ["U"] = {0x3F,0x40,0x40,0x40,0x3F},
    ["V"] = {0x1F,0x20,0x40,0x20,0x1F},
    ["W"] = {0x3F,0x40,0x38,0x40,0x3F},
    ["X"] = {0x63,0x14,0x08,0x14,0x63},
    ["Y"] = {0x03,0x04,0x78,0x04,0x03},
    ["Z"] = {0x61,0x51,0x49,0x45,0x43},
    ["_"] = {0x20,0x40,0x40,0x40,0x40},
}

local function spi_cmd(value)
    spi.write(DC_CMD, value)
end

local function set_pos(col, page)
    spi_cmd(0xB0 | (page & 0x07))
    spi_cmd(col & 0x0F)
    spi_cmd(0x10 | ((col >> 4) & 0x0F))
end

local function send_page(byte_value)
    local data = {0x40}
    for i = 1, 128 do
        data[i + 1] = byte_value
    end
    spi.write(DC_DATA, data)
end

function P.factory(opts, _container)
    opts = opts or {}
    local mosi = opts.mosi or 3
    local clk = opts.clk or 2
    local cs = opts.cs or 7
    local dc = opts.dc or 11
    local res = opts.res or 6
    local freq = opts.freq or 1000000

    local o = {}

    function o:init()
        log.info("SSD1306: init start")
        spi.setup(mosi, clk, cs, dc, res, freq)
        log.info("SSD1306: spi.setup done")
        local init_seq = {
            0xAE,0xA8,0x3F,0xD3,0x00,0x40,0xA1,0xC8,0xDA,0x12,
            0x81,0xCF,0xA4,0xA6,0xD5,0x80,0xD9,0xF1,0xDB,0x40,
            0x8D,0x14,0x20,0x00,0xAF
        }
        for _, v in ipairs(init_seq) do
            spi_cmd(v)
        end
        log.info("SSD1306: init seq done")
        o:clear()
        log.info("SSD1306: init complete")
    end

    function o:clear()
        for page = 0, 7 do
            set_pos(0, page)
            send_page(0x00)
        end
    end

    function o:fill(on)
        local value = on and 0xFF or 0x00
        for page = 0, 7 do
            set_pos(0, page)
            send_page(value)
        end
    end

    function o:test_pattern(step)
        log.info("SSD1306: test_pattern")
        step = step or 0
        for page = 0, 7 do
            set_pos(0, page)
            local data = {0x40}
            for col = 0, 127 do
                local val = ((col + page + step) % 2 == 0) and 0xAA or 0x55
                data[col + 2] = val
            end
            spi.write(DC_DATA, data)
        end
        log.info("SSD1306: test_pattern done")
    end

    function o:write_line(page, text)
        log.info("SSD1306: write_line page=" .. page .. " text=" .. text)
        set_pos(0, page)
        local data = {0x40}
        local idx = 2
        for i = 1, #text do
            local ch = text:sub(i, i)
            local glyph = FONT[ch] or FONT[" "]
            for col = 1, 5 do
                if idx > 129 then break end
                data[idx] = glyph[col]
                idx = idx + 1
            end
            if idx > 129 then break end
            data[idx] = 0x00
            idx = idx + 1
            if idx > 129 then break end
        end
        while idx <= 129 do
            data[idx] = 0x00
            idx = idx + 1
        end
        spi.write(DC_DATA, data)
        log.info("SSD1306: write_line done")
    end

    function o:show_welcome()
        log.info("SSD1306: show_welcome")
        o:clear()
        o:write_line(1, "Kongnitive")
        o:write_line(3, "EdgeMCP")
        o:write_line(5, "ESP32-C3")
        log.info("SSD1306: show_welcome done")
    end

    function o:show_status(heap_kb, runtime_kb, lua_kb, uptime_sec)
        o:clear()
        o:write_line(0, string.format("HF:%5.1fKB", heap_kb))
        o:write_line(2, string.format("RT:%5.1fKB", runtime_kb))
        o:write_line(4, string.format("LU:%5.1fKB", lua_kb))
        o:write_line(6, string.format("UP:%6dS", uptime_sec))
    end

    return o
end

return P
