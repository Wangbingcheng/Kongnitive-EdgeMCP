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

- MCP server capabilities endpoint
  - GET /mcp returns capabilities object with:
    - `maxMessageSize`: 16384 (16KB)
    - `maxToolResultSize`: 8192 (8KB)
    - `maxScriptSize`: 15884 (15.5KB)
    - `supportsChunkedUpload`: true
    - `supportsChunkedDownload`: true
  - Tool descriptions updated with size limits

- Heap memory check before large script push
  - Rejects scripts >8KB if free heap <32KB

### Changed

- Increased MCP server limits
  - `CONFIG_MCP_MAX_MESSAGE_SIZE`: 4096 → 16384
  - `CONFIG_MCP_MAX_TOOL_RESULT_SIZE`: 2048 → 8192
  - HTTP/HTTPS server stack: 8192 → 12288
  - Lua task stack: 6144 → 8192
  - OTA task stack: 6144 → 8192

- `lcd.clear()` now fills frame buffer instead of direct SPI transfer
- `lcd.fill()` now writes to frame buffer
- `lcd.pixel()` now writes to frame buffer
- All Lua display functions now require `lcd.flush()` call after drawing
- Debug log level: reduced verbose logging in `jsonrpc.c` and `mcp_server.c` from `ESP_LOGI` to `ESP_LOGD`
- JSON-RPC parsing: use `cJSON_DetachItemFromObject` instead of `cJSON_Duplicate` to avoid memory pressure

### Fixed

- Screen flicker issue caused by clearing screen before drawing text
- Byte order issue (ESP32 little-endian vs ST7735 MSB-first SPI)
- Watchdog timeout caused by busy loop without yielding CPU
- **CRITICAL**: Memory leak in `mcp_handle_tools_call` — `cJSON_CreateObject()` for missing `arguments` was never freed (`mcp_protocol.c:132`)
- **MEDIUM**: Memory leak in `check_client_alive_cb` — `async_resp_arg` leaked when `httpd_queue_work()` fails (`main.c:118`)
- **LOW**: I2C scan leak — devices added during scan were never removed (`lua_runtime.c:527`)
- SPI bus reinitialization issue on lua_restart — use flag to track bus state instead of reinitializing
- SPIFFS append bug — use fwrite + fflush instead of fputs

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

### Memory Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    ESP32-C3 RAM (约 240KB)                    │
├─────────────────────────────────────────────────────────────┤
│ 静态全局区 (编译时确定，不占栈空间)                            │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │ s_http_body_buf[16384]     ← HTTP 请求体 (16KB)          │ │
│ │ s_ws_frame_buf[16384]      ← WebSocket 帧 (16KB)         │ │
│ └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│ 堆 (Heap) - 动态分配                                         │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │ cJSON 解析树 (~2-4KB)                                    │ │
│ │ Lua VM (~80-100KB)                                      │ │
│ └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│ 栈 (Stack) - HTTP 服务器任务                                 │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │ mcp_http_handler 栈帧 (~5-6KB 使用)  ← 12KB 分配          │ │
│ └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

Key insight: The 15KB script payload is stored in `s_http_body_buf` (static), not on the stack. The stack only handles control flow (~5-6KB).

### Files Modified

| File | Changes |
|------|---------|
| `main/lua_runtime.c` | Frame buffer, unified print, byte swap, SPI bus state tracking, stack size |
| `main/default_scripts/default_provider_st7735.lua` | Updated to use new API with flush() |
| `main/default_scripts/default_main.lua` | Added sleep_ms() to prevent watchdog timeout |
| `main/mcp_server.c` | Increased buffers, capabilities endpoint |
| `main/mcp_tools.c` | Updated tool descriptions, heap check |
| `main/mcp_protocol.c` | Increased tool result buffer |
| `main/main.c` | Increased HTTP/HTTPS stack sizes |
| `main/mcp_ota.c` | Increased OTA task stack size |
| `sdkconfig.defaults` | Updated message size limits |
