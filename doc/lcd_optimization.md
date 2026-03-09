# LCD Display Optimization

This document explains the technical implementation of LCD display optimizations in Kongnitive EdgeMCP.

## Overview

The LCD driver (ST7735) implements two key optimizations to reduce CPU overhead and improve display performance:

1. **Dirty Rectangle Tracking** - Only refresh changed regions
2. **DMA Async Transfer with Synchronization** - Non-blocking SPI transfer with completion callback

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Lua Application                         │
│  lcd.clear() → lcd.print() → lcd.fill() → lcd.flush()       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   Frame Buffer (25.6KB)                      │
│  160×80 pixels × 2 bytes/pixel = 25,600 bytes               │
│  Allocated with MALLOC_CAP_DMA for DMA compatibility        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                 Dirty Rectangle Tracker                      │
│  Tracks: dirty_x1, dirty_y1, dirty_x2, dirty_y2            │
│  Only transmits the bounding box of changed pixels          │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│              DMA Transfer + Semaphore Sync                   │
│  esp_lcd_panel_draw_bitmap() → DMA transfer                 │
│  on_color_trans_done() callback → xSemaphoreGive()          │
│  lcd.flush() blocks until transfer complete                 │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   ST7735 LCD Panel                           │
│  SPI @ 20MHz, 160×80 RGB565 display                          │
└─────────────────────────────────────────────────────────────┘
```

## Optimization 1: Dirty Rectangle Tracking

### Problem

Refreshing the entire screen (25.6KB) every frame is wasteful when only small regions change (e.g., text updates).

### Solution

Track the bounding box of all modified pixels and only transmit that region.

### Implementation

```c
static int dirty_x1 = LCD_WIDTH, dirty_y1 = LCD_HEIGHT;
static int dirty_x2 = 0, dirty_y2 = 0;

static void fb_mark_dirty(int x, int y, int w, int h)
{
    if (x < dirty_x1) dirty_x1 = x;
    if (y < dirty_y1) dirty_y1 = y;
    if (x + w > dirty_x2) dirty_x2 = x + w;
    if (y + h > dirty_y2) dirty_y2 = y + h;
}
```

### API Integration

| Function | Dirty Tracking |
|----------|----------------|
| `lcd.clear()` | Marks full screen (0,0 to 160,80) |
| `lcd.fill(x,y,w,h,color)` | Marks filled rectangle |
| `lcd.pixel(x,y,color)` | Marks single pixel |
| `lcd.print(x,y,text,...)` | Marks character bounding box |

### Data Transfer Reduction

| Scenario | Full Refresh | Dirty Region | Savings |
|----------|-------------|--------------|---------|
| Clear screen | 25.6KB | 25.6KB | 0% |
| Print "Hello!" (scale=2) | 25.6KB | ~0.5KB | ~98% |
| Update 4 status lines | 25.6KB | ~4KB | ~84% |

## Optimization 2: DMA Async Transfer

### Problem

SPI transfers block the CPU while data is being transmitted, wasting precious cycles.

### Solution

Use DMA (Direct Memory Access) for SPI transfers, with a callback to signal completion.

### Implementation

#### 1. DMA-Capable Buffer

```c
lcd_framebuf = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_DMA);
```

`MALLOC_CAP_DMA` ensures the buffer is in a memory region accessible by the DMA controller.

#### 2. SPI Bus with DMA Channel

```c
spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
```

`SPI_DMA_CH_AUTO` lets ESP-IDF automatically allocate a DMA channel.

#### 3. Transfer Completion Callback

```c
static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, 
                                 esp_lcd_panel_io_event_data_t *edata, 
                                 void *user_ctx)
{
    if (lcd_flush_sem) {
        xSemaphoreGive(lcd_flush_sem);
    }
    return false;
}

esp_lcd_panel_io_spi_config_t io_cfg = {
    ...
    .on_color_trans_done = on_color_trans_done,
};
```

#### 4. Synchronized Flush

```c
static int l_lcd_flush(lua_State *L)
{
    if (!fb_has_dirty()) return 0;  // No changes, skip transfer
    
    xSemaphoreTake(lcd_flush_sem, 0);  // Clear any pending signal
    
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, 
                               dirty_x1, dirty_y1, dirty_x2, dirty_y2, 
                               lcd_framebuf);  // Start DMA transfer
    
    xSemaphoreTake(lcd_flush_sem, pdMS_TO_TICKS(100));  // Wait for completion
    
    fb_clear_dirty();
    return 0;
}
```

### Flow Diagram

```
Lua: lcd.flush()
        │
        ▼
┌───────────────────┐
│ Check dirty area  │──► No dirty? ──► Return immediately
└───────────────────┘
        │ Yes
        ▼
┌───────────────────┐
│ Clear semaphore   │
└───────────────────┘
        │
        ▼
┌───────────────────┐    DMA transfer runs in background
│ Start DMA transfer│────► SPI hardware reads from RAM
└───────────────────┘     CPU is free (mostly)
        │
        ▼
┌───────────────────┐
│ Wait on semaphore │◄── Blocks until DMA done
└───────────────────┘
        │
        ▼
┌───────────────────┐
│ Clear dirty flags │
└───────────────────┘
        │
        ▼
   Return to Lua
```

### Why Block on Semaphore?

The `lcd.flush()` function blocks until DMA completes because:

1. **Data integrity**: Ensures the frame buffer isn't modified during transfer
2. **Simple API**: Lua scripts don't need to manage async complexity
3. **Tear-free**: Next frame starts only after current one is displayed

## Memory Layout

```
┌────────────────────────────────────────┐
│           Frame Buffer (25.6KB)         │
│  ┌──────────────────────────────────┐  │
│  │  Pixel (0,0)  │ Pixel (1,0) │ ...│  │  Row 0
│  ├──────────────────────────────────┤  │
│  │  Pixel (0,1)  │ Pixel (1,1) │ ...│  │  Row 1
│  ├──────────────────────────────────┤  │
│  │              ...                  │  │
│  ├──────────────────────────────────┤  │
│  │  Pixel (0,79) │ Pixel (1,79)│ ...│  │  Row 79
│  └──────────────────────────────────┘  │
└────────────────────────────────────────┘

Where each pixel is 2 bytes (RGB565):
┌─────────────┬─────────────┐
│ High Byte   │  Low Byte   │
│ R4-R0 G5-G3 │ G2-G0 B4-B0 │
└─────────────┴─────────────┘
```

## Performance Metrics

### SPI Transfer Time

| Transfer Size | @10MHz | @20MHz | @40MHz |
|--------------|--------|--------|--------|
| Full screen (25.6KB) | 20.5ms | 10.2ms | 5.1ms |
| Text line (~512B) | 0.4ms | 0.2ms | 0.1ms |

### CPU Overhead Reduction

| Method | CPU Involvement |
|--------|-----------------|
| Polling SPI | 100% (CPU drives each byte) |
| DMA transfer | ~5% (CPU just sets up and waits) |

### Real-World Impact

For a typical status display updating 4 times per second:

| Metric | Before | After |
|--------|--------|-------|
| Data per second | 102KB | ~16KB |
| SPI time per second | 40ms | 6ms |
| CPU available | 96% | 99.4% |

## Best Practices

### 1. Batch Updates

```lua
-- Good: Single flush for multiple operations
lcd.clear(COLORS.BLACK)
lcd.print(0, 0, "Line 1", COLORS.WHITE)
lcd.print(0, 20, "Line 2", COLORS.WHITE)
lcd.flush()  -- One transfer

-- Avoid: Flush after each operation
lcd.clear(COLORS.BLACK)
lcd.flush()  -- Unnecessary transfer
lcd.print(0, 0, "Line 1", COLORS.WHITE)
lcd.flush()  -- Another transfer
```

### 2. Avoid Overlapping Regions

```lua
-- Good: Non-overlapping regions are marked once
lcd.fill(0, 0, 80, 40, COLORS.RED)
lcd.fill(80, 0, 80, 40, COLORS.BLUE)
lcd.flush()

-- The dirty region will be (0,0)-(160,40), not two separate transfers
```

### 3. Use Clear Judiciously

```lua
-- clear() marks entire screen dirty
-- Only use when truly needed

-- If updating same region, no clear needed:
lcd.fill(0, 0, 160, 20, COLORS.BLACK)  -- Overwrites old content
lcd.print(0, 0, "Updated", COLORS.WHITE)
lcd.flush()
```

## Limitations

1. **No true double buffering**: Current implementation uses single buffer with dirty tracking. For animations, consider adding a second buffer.

2. **Blocking flush**: `lcd.flush()` blocks until DMA completes. For non-blocking operation, would need Lua callback support.

3. **Dirty region merging**: Currently uses a single bounding box. Multiple small updates in corners will refresh the entire bounding box.

## Future Improvements

1. **Multiple dirty regions**: Track separate rectangles for better optimization
2. **Non-blocking flush**: Return immediately, callback when done
3. **Tearing effect**: Use TE pin for perfectly timed updates
4. **Partial Lua buffer**: Allow Lua to manage smaller buffers

## References

- [ESP-IDF LCD Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html)
- [ST7735 Datasheet](https://www.displayfuture.com/DISPLAY/datasheet/controller/ST7735.pdf)
- [ESP32 SPI DMA](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/spi_master.html#dma-features)
