# LCD 显示优化技术文档

本文档详细说明 Kongnitive EdgeMCP 中 LCD 显示优化的技术实现原理。

## 概述

LCD 驱动（ST7735）实现了三项关键优化，以减少 CPU 开销并提高显示性能：

1. **脏矩形追踪** - 仅刷新变化的区域
2. **DMA 异步传输 + 同步等待** - 非阻塞 SPI 传输配合完成回调
3. **双缓冲** - 绘制与传输并行，消除撕裂

## 架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      Lua 应用层                              │
│  lcd.clear() → lcd.print() → lcd.fill() → lcd.flush()       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                 双帧缓冲区 (51.2KB)                          │
│  ┌─────────────────────┐  ┌─────────────────────┐          │
│  │   Buffer[0] (25.6KB)│  │   Buffer[1] (25.6KB)│          │
│  │   绘制缓冲区/传输缓冲区  │  │   传输缓冲区/绘制缓冲区  │          │
│  └─────────────────────┘  └─────────────────────┘          │
│         ↑ draw_idx=0           ↑ draw_idx=1                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   脏矩形追踪器                                │
│  追踪: dirty_x1, dirty_y1, dirty_x2, dirty_y2              │
│  只传输变化像素的包围盒                                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│              DMA 传输 + 信号量同步                            │
│  esp_lcd_panel_draw_bitmap() → 启动 DMA 传输                 │
│  on_color_trans_done() 回调 → xSemaphoreGive() 释放信号量    │
│  flush() 立即返回，绘制与传输并行                              │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   ST7735 LCD 屏幕                             │
│  SPI @ 20MHz, 160×80 RGB565 显示屏                            │
└─────────────────────────────────────────────────────────────┘
```

## 优化一：脏矩形追踪

### 问题背景

每次刷新整个屏幕（25.6KB）是浪费的，尤其是只有小区域发生变化时（如更新文本）。

### 解决方案

追踪所有修改像素的包围盒，只传输该区域。

### 实现代码

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

### API 脏标记行为

| 函数 | 脏标记范围 |
|------|-----------|
| `lcd.clear()` | 标记全屏 (0,0 到 160,80) |
| `lcd.fill(x,y,w,h,color)` | 标记填充矩形 |
| `lcd.pixel(x,y,color)` | 标记单个像素 |
| `lcd.print(x,y,text,...)` | 标记字符包围盒 |

### 数据传输对比

| 场景 | 全屏刷新 | 脏区域刷新 | 节省比例 |
|------|---------|-----------|---------|
| 清屏 | 25.6KB | 25.6KB | 0% |
| 打印 "Hello!" (scale=2) | 25.6KB | ~0.5KB | ~98% |
| 更新 4 行状态 | 25.6KB | ~4KB | ~84% |

## 优化二：DMA 异步传输

### 问题背景

SPI 传输时 CPU 阻塞等待数据发送完成，浪费宝贵的 CPU 周期。

### 解决方案

使用 DMA（直接内存访问）进行 SPI 传输，通过回调函数通知传输完成。

### 实现细节

#### 1. DMA 兼容的缓冲区

```c
lcd_framebuf = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_DMA);
```

`MALLOC_CAP_DMA` 确保缓冲区位于 DMA 控制器可访问的内存区域。

#### 2. 启用 DMA 的 SPI 总线

```c
spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
```

`SPI_DMA_CH_AUTO` 让 ESP-IDF 自动分配 DMA 通道。

#### 3. 传输完成回调

```c
static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, 
                                 esp_lcd_panel_io_event_data_t *edata, 
                                 void *user_ctx)
{
    if (lcd_flush_sem) {
        xSemaphoreGive(lcd_flush_sem);  // 释放信号量
    }
    return false;
}

esp_lcd_panel_io_spi_config_t io_cfg = {
    ...
    .on_color_trans_done = on_color_trans_done,
};
```

#### 4. 同步等待的 flush

```c
static int l_lcd_flush(lua_State *L)
{
    if (!fb_has_dirty()) return 0;  // 无变化，跳过传输
    
    xSemaphoreTake(lcd_flush_sem, 0);  // 清除可能存在的信号
    
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, 
                               dirty_x1, dirty_y1, dirty_x2, dirty_y2, 
                               lcd_framebuf);  // 启动 DMA 传输
    
    xSemaphoreTake(lcd_flush_sem, pdMS_TO_TICKS(100));  // 等待完成
    
    fb_clear_dirty();
    return 0;
}
```

### 流程图

```
Lua: lcd.flush()
        │
        ▼
┌───────────────────┐
│ 检查是否有脏区域    │──► 无脏区域 ──► 立即返回
└───────────────────┘
        │ 有脏区域
        ▼
┌───────────────────┐
│ 清除信号量         │
└───────────────────┘
        │
        ▼
┌───────────────────┐    DMA 传输在后台运行
│ 启动 DMA 传输      │────► SPI 硬件直接从 RAM 读取
└───────────────────┘     CPU 基本空闲
        │
        ▼
┌───────────────────┐
│ 等待信号量         │◄── 阻塞直到 DMA 完成
└───────────────────┘
        │
        ▼
┌───────────────────┐
│ 清除脏标记         │
└───────────────────┘
        │
        ▼
   返回 Lua
```

## 优化三：双缓冲

### 问题背景

单缓冲方案中，Lua 必须等待 DMA 传输完成才能开始下一帧绘制，造成 CPU 空闲等待。

### 解决方案

使用两个帧缓冲区，绘制和传输可以在不同缓冲区上并行进行。

### 实现代码

```c
static uint16_t *lcd_framebuf[2] = {NULL, NULL};  // 双缓冲
static int lcd_draw_idx = 0;                       // 当前绘制缓冲区索引
static volatile bool lcd_dma_busy = false;         // DMA 忙状态

static int l_lcd_flush(lua_State *L)
{
    // 1. 等待上一次 DMA 完成
    if (lcd_dma_busy) {
        xSemaphoreTake(lcd_flush_sem, pdMS_TO_TICKS(100));
    }

    // 2. 记录当前绘制缓冲区为传输缓冲区
    int disp_idx = lcd_draw_idx;
    
    // 3. 切换到另一个缓冲区作为新的绘制缓冲区
    lcd_draw_idx = 1 - lcd_draw_idx;

    // 4. 复制脏矩形数据到新绘制缓冲区（保持一致性）
    for (int y = dirty_y1; y < dirty_y2; y++) {
        for (int x = dirty_x1; x < dirty_x2; x++) {
            lcd_framebuf[lcd_draw_idx][y * LCD_WIDTH + x] = 
                lcd_framebuf[disp_idx][y * LCD_WIDTH + x];
        }
    }

    // 5. 启动 DMA 传输（使用旧绘制缓冲区）
    lcd_dma_busy = true;
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, ... lcd_framebuf[disp_idx]);

    // 6. 立即返回
    fb_clear_dirty();
    return 0;
}
```

### 并行时间线

```
时间 ──────────────────────────────────────────────────────────►

帧1: [Lua 绘制 Buffer[0]] [flush] [Lua 绘制 Buffer[1]] [flush] ...
                              │            │              │
                              ▼            ▼              ▼
DMA:                    [传输 Buffer[0]] [传输 Buffer[1]] ...
                              │            │
                              └────────────┴── Lua 和 DMA 并行！
```

### 数据一致性

脏矩形数据会从旧缓冲区复制到新缓冲区，确保两个缓冲区的脏区域内容一致：

```
Buffer[0] (disp_idx=0)          Buffer[1] (draw_idx=1)
┌──────────────────┐           ┌──────────────────┐
│  ┌────────────┐  │   复制    │  ┌────────────┐  │
│  │ 脏矩形数据  │──┼──────────►│  │ 脏矩形数据  │  │
│  └────────────┘  │           │  └────────────┘  │
│                  │           │                  │
└──────────────────┘           └──────────────────┘
     DMA 传输 ◄─────              Lua 绘制 ◄─────
```

### 内存开销

| 项目 | 大小 |
|------|------|
| 单缓冲 | 25.6KB |
| 双缓冲 | 51.2KB |

以 51.2KB 的内存代价换取绘制与传输的并行。

## 内存布局

```
┌────────────────────────────────────────┐
│          双帧缓冲区 (51.2KB)            │
│  ┌────────────────────────────────┐    │
│  │      Buffer[0] (25.6KB)        │    │
│  │  ┌──────────────────────────┐  │    │
│  │  │ 像素(0,0) │ 像素(1,0) │...│  │    │  第 0 行
│  │  ├──────────────────────────┤  │    │
│  │  │ 像素(0,1) │ 像素(1,1) │...│  │    │  第 1 行
│  │  │          ...             │  │    │
│  │  │ 像素(0,79)│像素(1,79)│...│  │    │  第 79 行
│  │  └──────────────────────────┘  │    │
│  └────────────────────────────────┘    │
│  ┌────────────────────────────────┐    │
│  │      Buffer[1] (25.6KB)        │    │
│  │  ┌──────────────────────────┐  │    │
│  │  │ 像素(0,0) │ 像素(1,0) │...│  │    │  第 0 行
│  │  ├──────────────────────────┤  │    │
│  │  │ 像素(0,1) │ 像素(1,1) │...│  │    │  第 1 行
│  │  │          ...             │  │    │
│  │  │ 像素(0,79)│像素(1,79)│...│  │    │  第 79 行
│  │  └──────────────────────────┘  │    │
│  └────────────────────────────────┘    │
└────────────────────────────────────────┘

每个像素 2 字节 (RGB565 格式):
┌─────────────┬─────────────┐
│   高字节     │   低字节     │
│ R4-R0 G5-G3 │ G2-G0 B4-B0 │
└─────────────┴─────────────┘
```

## 性能指标

### SPI 传输时间

| 传输大小 | @10MHz | @20MHz | @40MHz |
|---------|--------|--------|--------|
| 全屏 (25.6KB) | 20.5ms | 10.2ms | 5.1ms |
| 文本行 (~512B) | 0.4ms | 0.2ms | 0.1ms |

### CPU 开销对比

| 方式 | CPU 参与度 |
|------|-----------|
| 轮询 SPI | 100% (CPU 驱动每个字节) |
| DMA 传输 | ~5% (CPU 仅配置和等待) |

### 实际场景收益

典型的状态显示，每秒更新 4 次：

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| 每秒数据量 | 102KB | ~16KB |
| 每秒 SPI 时间 | 40ms | 6ms |
| CPU 可用时间 | 96% | 99.4% |

## 最佳实践

### 1. 批量更新

```lua
-- 推荐：多次操作后一次 flush
lcd.clear(COLORS.BLACK)
lcd.print(0, 0, "第一行", COLORS.WHITE)
lcd.print(0, 20, "第二行", COLORS.WHITE)
lcd.flush()  -- 只传输一次

-- 避免：每次操作后都 flush
lcd.clear(COLORS.BLACK)
lcd.flush()  -- 不必要的传输
lcd.print(0, 0, "第一行", COLORS.WHITE)
lcd.flush()  -- 又一次传输
```

### 2. 避免不必要的 clear

```lua
-- 推荐：如果更新同一区域，无需 clear
lcd.fill(0, 0, 160, 20, COLORS.BLACK)  -- 用背景色覆盖旧内容
lcd.print(0, 0, "更新内容", COLORS.WHITE)
lcd.flush()

-- 避免：clear 会标记全屏脏
lcd.clear(COLORS.BLACK)  -- 全屏变脏
lcd.print(0, 0, "更新内容", COLORS.WHITE)  -- 只改了一小块
lcd.flush()  -- 却传输了全屏
```

### 3. 静态内容只绘制一次

```lua
-- init.lua 中绘制静态内容
lcd.print(0, 60, "固定标签:", COLORS.GRAY)
lcd.flush()

-- 循环中只更新变化部分
while true do
    lcd.fill(80, 60, 80, 16, COLORS.BLACK)  -- 只清除值区域
    lcd.print(80, 60, value, COLORS.WHITE)
    lcd.flush()
end
```

## 当前限制

1. **脏区域合并**：当前使用单一包围盒。四角的小更新会导致整个包围盒被传输。

2. **脏矩形复制开销**：双缓冲切换时需要复制脏矩形数据到新缓冲区，对于大面积更新有一定开销。

## 未来改进方向

| 改进项 | 说明 |
|--------|------|
| 多脏区域追踪 | 分开追踪多个矩形，提高优化效率 |
| 撕裂效应消除 | 使用 TE 引引脚实现精确时机更新 |
| 异步回调通知 | DMA 完成后通知 Lua，而非阻塞等待 |
| Lua 端缓冲区 | 允许 Lua 管理更小的缓冲区 |

## 参考资料

- [ESP-IDF LCD 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html)
- [ST7735 数据手册](https://www.displayfuture.com/DISPLAY/datasheet/controller/ST7735.pdf)
- [ESP32 SPI DMA](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/spi_master.html#dma-features)
