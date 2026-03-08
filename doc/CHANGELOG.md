# Changelog

## [Unreleased]

### Added

- LCD double buffering support for ST7735 display
  - Frame buffer: 160x80x2 = 25,600 bytes in DMA-capable memory
  - Eliminates screen flicker by rendering to memory first, then single DMA transfer to display
  - New `lcd.flush()` function to send frame buffer to screen

- Unified `lcd.print()` function with scale parameter
  - Signature: `lcd.print(x, y, text, fg, bg, scale)`
  - `scale` parameter: 1-4 (8px to 32px font height)
  - `bg` parameter: nil for transparent background
  - Replaces old `lcd.print()` and `lcd.print_2x()` functions

### Changed

- `lcd.clear()` now fills frame buffer instead of direct SPI transfer
- `lcd.fill()` now writes to frame buffer
- `lcd.pixel()` now writes to frame buffer
- All Lua display functions now require `lcd.flush()` call after drawing

### Fixed

- Screen flicker issue caused by clearing screen before drawing text
- Byte order issue (ESP32 little-endian vs ST7735 MSB-first SPI)
- Watchdog timeout caused by busy loop without yielding CPU

### Removed

- `lcd.print_2x()` - replaced by `lcd.print()` with scale parameter
- `lcd_fill_area()` - no longer needed, replaced by frame buffer operations

## Technical Details

### Double Buffering Implementation

```
Lua Code                     C Layer                      Hardware
---------                    ---------                    --------
lcd.clear(color)     -->    fb_fill_rect()       -->    frame_buffer[]
lcd.print(...)       -->    fb_draw_char()       -->    frame_buffer[]
lcd.flush()          -->    lcd_send_buffer()    -->    ST7735 (SPI)
```

### Byte Order Fix

ESP32 stores `uint16_t` in little-endian, but ST7735 expects MSB-first on SPI.
Solution: `swap_bytes()` function swaps bytes when writing to frame buffer.

```c
static inline uint16_t swap_bytes(uint16_t val) {
    return (val >> 8) | (val << 8);
}
```

### Files Modified

| File | Changes |
|------|---------|
| `main/lua_runtime.c` | Frame buffer, unified print, byte swap, removed unused functions |
| `main/default_scripts/default_provider_st7735.lua` | Updated to use new API with flush() |
| `main/default_scripts/default_main.lua` | Added sleep_ms() to prevent watchdog timeout |
