local P = {}

-- CRC-8 多项式 0x31，初始值 0xFF
local function crc8(bytes)
    local crc = 0xFF
    for i = 1, #bytes do
        crc = crc ~ bytes[i]
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
    local addr = opts.addr or 0x44
    local sda = opts.sda or 0
    local scl = opts.scl or 1
    local freq = opts.freq or 100000
    local self = {}

    function self:init()
        log.info("[SHT40] Setting up I2C...")
        i2c.setup(sda, scl, freq)
        
        -- Verify I2C bus is working
        local devices = i2c.scan()
        if not devices or #devices == 0 then
            log.warn("[SHT40] No I2C devices found!")
        else
            log.info("[SHT40] I2C devices found: " .. table.concat(devices, ", "))
        end
    end

    function self:read()
        local ok, result = pcall(function()
            log.info("[SHT40] Sending read command...")
            i2c.write(addr, string.char(0xFD))  -- 高重复性测量
            time.sleep_ms(20)  -- 典型 9-15 ms，12 ms 为折中值

            log.info("[SHT40] Reading data...")
            local data = i2c.read(addr, 6)
            log.info("[SHT40] Data received, length: " .. (data and #data or "nil"))
            if not data or #data < 6 then
                log.warn("[SHT40] Read failed: incomplete data")
                return nil
            end

            -- 数据格式: T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC
            local t_msb = data[1]
            local t_lsb = data[2]
            local t_crc = data[3]
            local h_msb = data[4]
            local h_lsb = data[5]
            local h_crc = data[6]

            log.info(string.format("[SHT40] CRC: t_crc=%02X calc=%02X h_crc=%02X calc=%02X", 
                t_crc, crc8({t_msb, t_lsb}), h_crc, crc8({h_msb, h_lsb})))
            if crc8({t_msb, t_lsb}) ~= t_crc or crc8({h_msb, h_lsb}) ~= h_crc then
                log.warn("[SHT40] CRC check failed!")
                return nil
            end

            local t_raw = t_msb * 256 + t_lsb
            local h_raw = h_msb * 256 + h_lsb
            log.info(string.format("[SHT40] Raw: t_raw=%d h_raw=%d", t_raw, h_raw))

            local temp = -45 + 175 * (t_raw / 65535.0)
            local hum  =  -6 + 125 * (h_raw / 65535.0)

            log.info(string.format("[SHT40] Result: T=%.1f H=%.1f", temp, hum))
            return {
                temperature = math.floor(temp * 10 + 0.5) / 10,
                humidity    = math.floor(hum  * 10 + 0.5) / 10
            }
        end)
        if ok and result then
            log.info("[SHT40] Read success!")
            return result
        else
            log.warn("[SHT40] Read failed: " .. tostring(result))
            return nil
        end
    end

    return self
end

return P