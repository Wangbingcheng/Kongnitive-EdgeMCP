# LCD Display Optimization

This document explains the technical implementation of LCD display optimizations in Kongnitive EdgeMCP.

## Overview

The LCD driver (ST7735) implements three key optimizations to reduce CPU overhead and improve display performance:

1. **Dirty Rectangle Tracking** - Only refresh changed regions
2. **DMA Async Transfer with Synchronization** - Non-blocking SPI transfer with completion callback
3. **Double Buffering** - Parallel drawing and transfer, eliminating tearing

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Lua Application                         │
│  lcd.clear() → lcd.print() → lcd.fill() → lcd.flush()       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                 Double Frame Buffer (51.2KB)                 │
│  ┌─────────────────────┐  ┌─────────────────────┐          │
│  │   Buffer[0] (25.6KB)│  │   Buffer[1] (25.6KB)│          │
│  │   Draw/Display Buf  │  │   Display/Draw Buf  │          │
│  └─────────────────────┘  └─────────────────────┘          │
│         ↑ draw_idx=0           ↑ draw_idx=1                │
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
│  flush() returns immediately, draw/transfer parallel        │
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

## Optimization 3: Double Buffering

### Problem

With single buffering, Lua must wait for DMA transfer to complete before starting the next frame, causing CPU idle time.

### Solution

Use two frame buffers, allowing drawing and transfer to run in parallel on different buffers.

### Implementation

```c
static uint16_t *lcd_framebuf[2] = {NULL, NULL};  // Double buffer
static int lcd_draw_idx = 0;                       // Current draw buffer index
static volatile bool lcd_dma_busy = false;         // DMA busy flag

static int l_lcd_flush(lua_State *L)
{
    // 1. Wait for previous DMA to complete
    if (lcd_dma_busy) {
        xSemaphoreTake(lcd_flush_sem, pdMS_TO_TICKS(100));
    }

    // 2. Current draw buffer becomes display buffer
    int disp_idx = lcd_draw_idx;
    
    // 3. Switch to other buffer as new draw buffer
    lcd_draw_idx = 1 - lcd_draw_idx;

    // 4. Copy dirty rect data to new draw buffer (consistency)
    for (int y = dirty_y1; y < dirty_y2; y++) {
        for (int x = dirty_x1; x < dirty_x2; x++) {
            lcd_framebuf[lcd_draw_idx][y * LCD_WIDTH + x] = 
                lcd_framebuf[disp_idx][y * LCD_WIDTH + x];
        }
    }

    // 5. Start DMA transfer (using old draw buffer)
    lcd_dma_busy = true;
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, ... lcd_framebuf[disp_idx]);

    // 6. Return immediately
    fb_clear_dirty();
    return 0;
}
```

### Parallel Timeline

```
Time ──────────────────────────────────────────────────────────►

Frame1: [Lua draw Buffer[0]] [flush] [Lua draw Buffer[1]] [flush] ...
                               │            │              │
                               ▼            ▼              ▼
DMA:                      [Transfer Buffer[0]] [Transfer Buffer[1]] ...
                               │            │
                               └────────────┴── Lua and DMA parallel!
```

### Data Consistency

Dirty rectangle data is copied from old buffer to new buffer:

```
Buffer[0] (disp_idx=0)          Buffer[1] (draw_idx=1)
┌──────────────────┐           ┌──────────────────┐
│  ┌────────────┐  │   copy    │  ┌────────────┐  │
│  │ Dirty Rect │──┼──────────►│  │ Dirty Rect │  │
│  └────────────┘  │           │  └────────────┘  │
│                  │           │                  │
└──────────────────┘           └──────────────────┘
     DMA transfer ◄─────              Lua draw ◄─────
```

### Memory Cost

| Item | Size |
|------|------|
| Single buffer | 25.6KB |
| Double buffer | 51.2KB |

Trading 51.2KB memory for parallel drawing and transfer.

## Memory Layout

```
┌────────────────────────────────────────┐
│        Double Frame Buffer (51.2KB)     │
│  ┌────────────────────────────────┐    │
│  │      Buffer[0] (25.6KB)        │    │
│  │  ┌──────────────────────────┐  │    │
│  │  │Pixel(0,0)│Pixel(1,0)│...│  │    │  Row 0
│  │  ├──────────────────────────┤  │    │
│  │  │Pixel(0,1)│Pixel(1,1)│...│  │    │  Row 1
│  │  │          ...             │  │    │
│  │  │Pixel(0,79)│Pixel(1,79)│..│  │    │  Row 79
│  │  └──────────────────────────┘  │    │
│  └────────────────────────────────┘    │
│  ┌────────────────────────────────┐    │
│  │      Buffer[1] (25.6KB)        │    │
│  │  ┌──────────────────────────┐  │    │
│  │  │Pixel(0,0)│Pixel(1,0)│...│  │    │  Row 0
│  │  ├──────────────────────────┤  │    │
│  │  │Pixel(0,1)│Pixel(1,1)│...│  │    │  Row 1
│  │  │          ...             │  │    │
│  │  │Pixel(0,79)│Pixel(1,79)│..│  │    │  Row 79
│  │  └──────────────────────────┘  │    │
│  └────────────────────────────────┘    │
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

1. **Dirty region merging**: Currently uses a single bounding box. Multiple small updates in corners will refresh the entire bounding box.

2. **Dirty rect copy overhead**: Double buffering requires copying dirty rect data to new buffer during switch, which has some overhead for large updates.

## Future Improvements

1. **Multiple dirty regions**: Track separate rectangles for better optimization
2. **Tearing effect elimination**: Use TE pin for perfectly timed updates
3. **Async callback notification**: Notify Lua when DMA completes, instead of blocking wait
4. **Partial Lua buffer**: Allow Lua to manage smaller buffers

## References

- [ESP-IDF LCD Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html)
- [ST7735 Datasheet](https://www.displayfuture.com/DISPLAY/datasheet/controller/ST7735.pdf)
- [ESP32 SPI DMA](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/spi_master.html#dma-features)
