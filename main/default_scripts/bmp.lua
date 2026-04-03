-- BMP image loader for ST7735 LCD
-- Usage: local w, h = bmp.load("/spiffs/image.bmp", x, y)

local M = {}

local function rgb888_to_rgb565(r, g, b)
    local r5 = (r >> 3) & 0x1F
    local g6 = (g >> 2) & 0x3F
    local b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5
end

local function read_wordLE(str, offset)
    local b1 = str:byte(offset)
    local b2 = str:byte(offset + 1)
    return b1 | (b2 << 8)
end

local function read_dwordLE(str, offset)
    local b1 = str:byte(offset)
    local b2 = str:byte(offset + 1)
    local b3 = str:byte(offset + 2)
    local b4 = str:byte(offset + 3)
    return b1 | (b2 << 8) | (b3 << 16) | (b4 << 24)
end

function M.load(path, offset_x, offset_y)
    offset_x = offset_x or 0
    offset_y = offset_y or 0
    
    local f = io.open(path, "rb")
    if not f then
        return nil, "Cannot open file: " .. path
    end
    
    local data = f:read("*a")
    f:close()
    
    if #data < 54 then
        return nil, "File too small to be BMP"
    end
    
    local signature = data:sub(1, 2)
    if signature ~= "BM" then
        return nil, "Not a BMP file"
    end
    
    local width = read_dwordLE(data, 0x12)
    local height = read_dwordLE(data, 0x16)
    local bpp = read_wordLE(data, 0x1C)
    local compression = read_dwordLE(data, 0x1E)
    local pixel_offset = read_dwordLE(data, 0x0A)
    
    if bpp ~= 24 then
        return nil, string.format("Only 24-bit BMP supported, got %d-bit", bpp)
    end
    
    if compression ~= 0 then
        return nil, "Compressed BMP not supported"
    end
    
    local is_bottom_up = (height > 0)
    if not is_bottom_up then
        height = -height
    end
    
    local row_size = math.ceil(width * 3 / 4) * 4
    local rgb565 = {}
    
    for y = 0, height - 1 do
        local src_y = is_bottom_up and (height - 1 - y) or y
        local row_start = pixel_offset + src_y * row_size
        
        for x = 0, width - 1 do
            local idx = row_start + x * 3 + 1
            local b = data:byte(idx)
            local g = data:byte(idx + 1)
            local r = data:byte(idx + 2)
            local rgb = rgb888_to_rgb565(r, g, b)
            table.insert(rgb565, string.pack("<I2", rgb))
        end
    end
    
    lcd.draw_pixels(offset_x, offset_y, width, height, table.concat(rgb565))
    
    return width, height
end

return M
