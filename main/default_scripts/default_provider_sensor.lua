local P = {}

-- CRC-8 多项式 0x31，初始值 0xFF
local function crc8(bytes)
    local crc = 0xFF
    for i = 1, #bytes do
        crc = crc ~ bytes:byte(i)
        for _ = 1, 8 do
            if (crc & 0x80) ~= 0 then
                crc = ((crc << 1) & 0xFF) ~ 0x31
            else
                crc = (crc << 1) & 0xFF
            end
        end
    end
    return crc
end

function P.factory(opts, _container)
    opts = opts or {}
    local addr = opts.addr or 0x44  -- 默认 7 位地址
    local self = { addr = addr }

    function self:init()
        -- 若你的 i2c API 需要 8 位地址，请在外部传入 opts.addr = (0x44 << 1)
        i2c.write(self.addr, string.char(0x94))  -- 软复位
        time.sleep_ms(100)
        -- print(string.format("[SHT40] Initialized @ 0x%02X", self.addr))
        log.info(string.format("[SHT40] Initialized @ 0x%02X", self.addr))
    end

    function self:read()
        local ok, result = pcall(function()
            i2c.write(self.addr, string.char(0xFD))  -- 高重复性测量
            time.sleep_ms(20)  -- 典型 9-15 ms，12 ms 为折中值

            local data = i2c.read(self.addr, 6)
            if not data or #data < 6 then return nil end

            -- 数据格式: T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC
            local t_msb, t_lsb, t_crc = data:byte(1,3)
            local h_msb, h_lsb, h_crc = data:byte(4,5,6)

            local t_bytes = string.char(t_msb, t_lsb)
            local h_bytes = string.char(h_msb, h_lsb)
            if crc8(t_bytes) ~= t_crc or crc8(h_bytes) ~= h_crc then
                return nil
            end

            local t_raw = t_msb * 256 + t_lsb
            local h_raw = h_msb * 256 + h_lsb

            local temp = -45 + 175 * (t_raw / 65535.0)
            local hum  =  -6 + 125 * (h_raw / 65535.0)

            return {
                temperature = math.floor(temp * 10 + 0.5) / 10,
                humidity    = math.floor(hum  * 10 + 0.5) / 10
            }
        end)
        if ok and result then
            return result
        else
            return nil
        end
    end

    return self
end

return P
