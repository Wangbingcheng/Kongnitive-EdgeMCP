# LCD 显示优化技术文档

本文档详细说明 Kongnitive EdgeMCP 中 LCD 显示优化的技术实现原理。

## 概述

LCD 驱动（ST7735）实现了两项关键优化，以减少 CPU 开销并提高显示性能：

1. **脏矩形追踪** - 仅刷新变化的区域
2. **DMA 异步传输 + 同步等待** - 非阻塞 SPI 传输配合完成回调

## 架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      Lua 应用层                              │
│  lcd.clear() → lcd.print() → lcd.fill() → lcd.flush()       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   帧缓冲区 (25.6KB)                          │
│  160×80 像素 × 2 字节/像素 = 25,600 字节                     │
│  使用 MALLOC_CAP_DMA 分配，确保 DMA 控制器可访问              │
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
│  lcd.flush() 阻塞等待传输完成                                 │
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

### 为什么要阻塞等待信号量？

`lcd.flush()` 阻塞等待 DMA 完成的原因：

1. **数据完整性**：确保帧缓冲区在传输过程中不会被修改
2. **API 简洁**：Lua 脚本不需要处理异步复杂性
3. **无撕裂**：下一帧只有当前帧完全显示后才开始

## 内存布局

```
┌────────────────────────────────────────┐
│           帧缓冲区 (25.6KB)             │
│  ┌──────────────────────────────────┐  │
│  │  像素(0,0)  │ 像素(1,0) │ ...    │  │  第 0 行
│  ├──────────────────────────────────┤  │
│  │  像素(0,1)  │ 像素(1,1) │ ...    │  │  第 1 行
│  ├──────────────────────────────────┤  │
│  │              ...                  │  │
│  ├──────────────────────────────────┤  │
│  │  像素(0,79) │ 像素(1,79)│ ...    │  │  第 79 行
│  └──────────────────────────────────┘  │
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

1. **无真正双缓冲**：当前使用单缓冲+脏标记。如需动画，可考虑添加第二缓冲区。

2. **flush 阻塞**：`lcd.flush()` 会阻塞直到 DMA 完成。如需非阻塞操作，需要 Lua 回调支持。

3. **脏区域合并**：当前使用单一包围盒。四角的小更新会导致整个包围盒被传输。

## 未来改进方向

| 改进项 | 说明 |
|--------|------|
| 多脏区域追踪 | 分开追踪多个矩形，提高优化效率 |
| 非阻塞 flush | 立即返回，完成后回调通知 |
| 撕裂效应消除 | 使用 TE 引脚实现精确时机更新 |
| Lua 端缓冲区 | 允许 Lua 管理更小的缓冲区 |

## 参考资料

- [ESP-IDF LCD 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html)
- [ST7735 数据手册](https://www.displayfuture.com/DISPLAY/datasheet/controller/ST7735.pdf)
- [ESP32 SPI DMA](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/spi_master.html#dma-features)
