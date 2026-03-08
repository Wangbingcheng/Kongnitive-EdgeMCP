local P = {}

local function rgb(r, g, b)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
end

local COLORS = {
    BLACK = rgb(0, 0, 0),
    WHITE = rgb(255, 255, 255),
    RED = rgb(255, 0, 0),
    GREEN = rgb(0, 255, 0),
    BLUE = rgb(0, 0, 255),
    YELLOW = rgb(255, 255, 0),
    CYAN = rgb(0, 255, 255),
    GRAY = rgb(128, 128, 128),
}

function P.factory(opts, _container)
    opts = opts or {}
    local mosi = opts.mosi or 3
    local clk = opts.clk or 2
    local cs = opts.cs or 7
    local dc = opts.dc or 10
    local res = opts.res or 6
    local bl = opts.bl or 11
    local freq = opts.freq or 20000000

    local o = {}

    function o:init()
        log.info("ST7735: init start")
        lcd.setup(mosi, clk, cs, dc, res, bl, freq)
        log.info("ST7735: lcd.setup done")
        lcd.clear(COLORS.BLACK)
        lcd.flush()
        log.info("ST7735: init complete")
    end

    function o:clear()
        lcd.clear(COLORS.BLACK)
        lcd.flush()
    end

    function o:fill(color)
        lcd.fill(0, 0, 160, 80, color or COLORS.BLACK)
        lcd.flush()
    end

    function o:test_pattern(step)
        log.info("ST7735: test_pattern")
        step = step or 0
        local colors = {COLORS.RED, COLORS.GREEN, COLORS.BLUE, COLORS.WHITE}
        local c = colors[(step % 4) + 1]
        lcd.fill(0, 0, 160, 80, c)
        lcd.flush()
    end

    function o:write_line(page, text)
        local y = page * 20
        lcd.print(0, y, text, COLORS.WHITE, COLORS.BLACK, 2)
        lcd.flush()
    end

    function o:show_welcome()
        log.info("ST7735: show_welcome")
        lcd.clear(COLORS.BLACK)
        lcd.print(0, 5, "Kongnitive", COLORS.YELLOW, COLORS.BLACK, 2)
        lcd.print(40, 28, "EdgeMCP", COLORS.RED, COLORS.BLACK, 2)
        lcd.print(50, 51, "Ready!", COLORS.WHITE, COLORS.BLACK, 2)
        lcd.flush()
    end

    function o:show_status(heap_kb, runtime_kb, lua_kb, uptime_sec)
        lcd.clear(COLORS.BLACK)
        lcd.print(0, 2, string.format("Heap:%.0fK", heap_kb), COLORS.GREEN, COLORS.BLACK, 2)
        lcd.print(0, 22, string.format("RT:%.0fK", runtime_kb), COLORS.YELLOW, COLORS.BLACK, 2)
        lcd.print(0, 42, string.format("Lua:%.0fK", lua_kb), COLORS.CYAN, COLORS.BLACK, 2)
        lcd.print(0, 62, string.format("UP:%ds", uptime_sec), COLORS.RED, COLORS.BLACK, 2)
        lcd.flush()
    end

    return o
end

return P
