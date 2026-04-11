/*
 * Lua Runtime for ESP32
 *
 * SPIFFS-backed script storage + Lua 5.4 VM + C bindings for hardware.
 */

#include "lua_runtime.h"
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_spiffs.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_st7735.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_attr.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "lua_rt";

#define SPIFFS_BASE_PATH "/spiffs"
#define LUA_TASK_STACK   8192  /* 8KB for larger Lua scripts */
#define LUA_TASK_PRIO    3

static lua_State *L = NULL;
static TaskHandle_t lua_task_handle = NULL;
static volatile bool lua_task_running = false;
static volatile uint32_t lua_mem_current = 0;
static volatile uint32_t lua_mem_peak = 0;
static portMUX_TYPE lua_mem_mux = portMUX_INITIALIZER_UNLOCKED;

static void lua_mem_update(size_t old_size, size_t new_size)
{
    taskENTER_CRITICAL(&lua_mem_mux);
    uint32_t current = lua_mem_current;
    if (new_size >= old_size) {
        current += (uint32_t)(new_size - old_size);
    } else {
        uint32_t delta = (uint32_t)(old_size - new_size);
        current = (current > delta) ? (current - delta) : 0;
    }

    lua_mem_current = current;
    if (current > lua_mem_peak) {
        lua_mem_peak = current;
    }
    taskEXIT_CRITICAL(&lua_mem_mux);
}

static void *lua_tracking_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;

    if (nsize == 0) {
        free(ptr);
        if (ptr) {
            lua_mem_update(osize, 0);
        }
        return NULL;
    }

    void *new_ptr = realloc(ptr, nsize);
    if (!new_ptr) {
        // 如果realloc失败，我们不应该改变内存统计
        // 仅在成功分配后才更新内存统计
        return NULL;
    }

    lua_mem_update(ptr ? osize : 0, nsize);
    return new_ptr;
}

/* ── I2C bus state ─────────────────────────────────────────────── */

#define I2C_MAX_DEVICES  4
#define I2C_WRITE_BUF_SZ 256
#define I2C_READ_BUF_SZ  256
#define I2C_TIMEOUT_MS   100
#define I2C_SCAN_TIMEOUT_MS 50

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static uint32_t i2c_bus_freq = 400000;

static struct {
    uint16_t addr;
    i2c_master_dev_handle_t handle;
} i2c_dev_cache[I2C_MAX_DEVICES];
static int i2c_dev_count = 0;

/*
 * 把一个 Lua 值追加到字节缓冲区里。
 *
 * 这个函数是 SPI/I2C 写入路径的公共入口之一，
 * 用来把 Lua 传进来的不同数据类型统一转换成 uint8_t 字节流。
 *
 * 支持的输入类型：
 * 1. integer：追加 1 个字节
 * 2. string：按原始字符串字节逐个追加
 * 3. table：把数组形式 table 中的每个整数元素逐个追加
 */
static int lua_append_bytes_from_value(lua_State *L, int index, uint8_t *buf, int len, int max_len)
{
    /*
     * 如果缓冲区已经满了，直接返回当前长度。
     * 不再继续写，避免越界。
     */
    if (len >= max_len) {
        return len;
    }

    /*
     * 如果参数本身是整数，就把它当成一个字节写入。
     * 这里会做 uint8_t 强制转换，因此只保留低 8 位。
     */
    if (lua_isinteger(L, index)) {
        buf[len++] = (uint8_t)lua_tointeger(L, index);
        return len;
    }

    /*
     * 如果参数是字符串，就把字符串底层的每一个字节都复制进缓冲区。
     * 这适合写寄存器命令、原始二进制片段、文本等场景。
     */
    if (lua_isstring(L, index)) {
        size_t slen = 0;
        const char *str = lua_tolstring(L, index, &slen);
        for (size_t i = 0; i < slen && len < max_len; i++) {
            buf[len++] = (uint8_t)str[i];
        }
        return len;
    }

    if (lua_istable(L, index)) {
        /*
         * 先把 index 转成“绝对栈索引”。
         *
         * 原因是：下面会反复调用 lua_rawgeti，把元素压到 Lua 栈顶。
         * 如果继续使用相对索引，随着栈顶变化，原来的 table 位置也会“看起来变了”，
         * 后续访问可能取错对象。
         *
         * lua_absindex 会把当前 index 固定成一个不会因栈变化而漂移的位置。
         */
        int abs_index = lua_absindex(L, index);
        /*
         * 读取 table 的数组长度。
         * 这里假设传入的是一个按 1..N 排列的数组型 table。
         */
        int tlen = luaL_len(L, abs_index);
        for (int i = 1; i <= tlen && len < max_len; i++) {
            /*
             * 取出 table[i]，压到栈顶。
             * 取完后栈顶位置 -1 就是当前元素。
             */
            lua_rawgeti(L, abs_index, i);
            /*
             * 这里强制要求 table 中的每一个元素都是整数。
             * 这样可以保证 table -> byte buffer 的语义明确，避免混入字符串/布尔值/子 table。
             */
            if (!lua_isinteger(L, -1)) {
                /*
                 * 出错前先把刚刚压栈的值弹掉，保持栈平衡。
                 */
                lua_pop(L, 1);
                luaL_error(L, "byte table must contain integers");
            }
            /*
             * 把当前元素转成 1 个字节写入缓冲区。
             */
            buf[len++] = (uint8_t)lua_tointeger(L, -1);
            /*
             * 当前元素已经处理完，从栈顶弹出。
             */
            lua_pop(L, 1);
        }
        return len;
    }

    /*
     * 如果不是整数、字符串、table，就直接报错。
     * 这样上层接口的输入类型约束就保持一致。
     */
    luaL_error(L, "expected integer, string, or table");
    return len;
}

/*
 * 把一段 Lua 参数区间扁平化成连续字节缓冲区。
 *
 * 例如：
 * - (0x01, 0x02, 0x03)
 * - ("abc")
 * - ({0x01, 0x02}, "xy", 0xFF)
 *
 * 都会被转换成一段连续的字节数据。
 *
 * 这个函数被 I2C 和 SPI 的写路径共用，
 * 目的是让两套接口接受完全一致的输入格式。
 */
static int lua_build_byte_buffer(lua_State *L, int start_index, int end_index, uint8_t *buf, int max_len)
{
    int len = 0;
    /*
     * 依次处理 [start_index, end_index] 范围内的每个 Lua 参数，
     * 每个参数都交给 lua_append_bytes_from_value 继续展开。
     */
    for (int i = start_index; i <= end_index && len < max_len; i++) {
        len = lua_append_bytes_from_value(L, i, buf, len, max_len);
    }
    return len;
}

static i2c_master_dev_handle_t i2c_get_device(uint16_t addr)
{
    for (int i = 0; i < i2c_dev_count; i++) {
        if (i2c_dev_cache[i].addr == addr) return i2c_dev_cache[i].handle;
    }
    if (!i2c_bus_handle || i2c_dev_count >= I2C_MAX_DEVICES) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = i2c_bus_freq,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(i2c_bus_handle, &cfg, &dev) != ESP_OK) return NULL;

    i2c_dev_cache[i2c_dev_count].addr = addr;
    i2c_dev_cache[i2c_dev_count].handle = dev;
    i2c_dev_count++;
    return dev;
}

/* ── Default scripts (embedded) ─────────────────────────────────── */

extern const uint8_t default_di_container_lua_start[] asm("_binary_default_di_container_lua_start");
extern const uint8_t default_provider_ssd1306_lua_start[] asm("_binary_default_provider_ssd1306_lua_start");
extern const uint8_t default_provider_st7735_lua_start[] asm("_binary_default_provider_st7735_lua_start");
extern const uint8_t default_provider_sht40_lua_start[] asm("_binary_default_provider_sht40_lua_start");
extern const uint8_t default_bindings_lua_start[] asm("_binary_default_bindings_lua_start");
extern const uint8_t default_main_lua_start[] asm("_binary_default_main_lua_start");
extern const uint8_t default_main_lua_end[] asm("_binary_default_main_lua_end");
extern const uint32_t default_main_lua_length asm("default_main_lua_length");

/* ── SPIFFS helpers ─────────────────────────────────────────────── */

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = "storage",
        .max_files = 20,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", (int)used, (int)total);
    return ESP_OK;
}

static bool script_exists(const char *name)
{
    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);
    struct stat st;
    return (stat(path, &st) == 0);
}

static esp_err_t write_script_if_missing(const char *name, const char *content)
{
    if (script_exists(name)) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Writing default %s", name);

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create %s", name);
        return ESP_FAIL;
    }

    fputs(content, f);
    fclose(f);
    return ESP_OK;
}

static esp_err_t write_default_script(void)
{
    esp_err_t ret = write_script_if_missing("di_container.lua", (const char *)default_di_container_lua_start);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = write_script_if_missing("provider_ssd1306.lua", (const char *)default_provider_ssd1306_lua_start);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = write_script_if_missing("provider_st7735.lua", (const char *)default_provider_st7735_lua_start);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = write_script_if_missing("provider_sht40.lua", (const char *)default_provider_sht40_lua_start);
    if (ret != ESP_OK) {
        return ret;
    }
    

    ret = write_script_if_missing("bindings.lua", (const char *)default_bindings_lua_start);
    if (ret != ESP_OK) {
        return ret;
    }

    return write_script_if_missing("main.lua", (const char *)default_main_lua_start);
}

/* ── Lua C bindings: gpio ───────────────────────────────────────── */

static int l_gpio_setup(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    const char *mode_str = luaL_checkstring(L, 2);

    gpio_mode_t mode = GPIO_MODE_OUTPUT;
    if (strcmp(mode_str, "input") == 0) {
        mode = GPIO_MODE_INPUT;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return luaL_error(L, "gpio.setup failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_gpio_set(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    int level = luaL_checkinteger(L, 2);
    gpio_set_level(pin, level);
    return 0;
}

static int l_gpio_get(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

static const luaL_Reg gpio_lib[] = {
    {"setup", l_gpio_setup},
    {"set",   l_gpio_set},
    {"get",   l_gpio_get},
    {NULL, NULL}
};

/* ── Lua C bindings: time ───────────────────────────────────────── */

static int l_time_sleep_ms(lua_State *L)
{
    int ms = luaL_checkinteger(L, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static const luaL_Reg time_lib[] = {
    {"sleep_ms", l_time_sleep_ms},
    {NULL, NULL}
};

/* ── Lua C bindings: log ────────────────────────────────────────── */

static int l_log_info(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGI("lua", "%s", msg);
    return 0;
}

static int l_log_warn(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGW("lua", "%s", msg);
    return 0;
}

static int l_log_error(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGE("lua", "%s", msg);
    return 0;
}

static const luaL_Reg log_lib[] = {
    {"info",  l_log_info},
    {"warn",  l_log_warn},
    {"error", l_log_error},
    {NULL, NULL}
};

/* ── Lua C bindings: system ─────────────────────────────────────── */

static int l_system_heap_free(lua_State *L)
{
    lua_pushinteger(L, esp_get_free_heap_size());
    return 1;
}

static int l_system_heap_info(lua_State *L)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, info.total_free_bytes);
    lua_setfield(L, -2, "total_free");
    lua_pushinteger(L, info.largest_free_block);
    lua_setfield(L, -2, "largest_block");
    lua_pushinteger(L, info.total_allocated_bytes);
    lua_setfield(L, -2, "allocated");
    lua_pushinteger(L, info.minimum_free_bytes);
    lua_setfield(L, -2, "min_free");
    lua_pushinteger(L, info.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, info.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    return 1;
}

static int l_system_trim_heap(lua_State *L)
{
    (void)L;
    malloc_trim(0);
    ESP_LOGD(TAG, "Heap trimmed");
    return 0;
}

static int l_system_uptime(lua_State *L)
{
    lua_pushnumber(L, (double)esp_timer_get_time() / 1000000.0);
    return 1;
}

static const luaL_Reg system_lib[] = {
    {"heap_free",  l_system_heap_free},
    {"heap_info",  l_system_heap_info},
    {"trim_heap",  l_system_trim_heap},
    {"uptime",     l_system_uptime},
    {NULL, NULL}
};

/* ── Lua C bindings: wifi ───────────────────────────────────────── */

static int l_wifi_rssi(lua_State *L)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lua_pushinteger(L, ap.rssi);
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static const luaL_Reg wifi_lib[] = {
    {"rssi", l_wifi_rssi},
    {NULL, NULL}
};

/* ── Lua C bindings: i2c ────────────────────────────────────────── */

static int l_i2c_setup(lua_State *L)
{
    /* 第 1、2 个参数分别是 SDA / SCL 引脚号。 */
    int sda = luaL_checkinteger(L, 1);
    int scl = luaL_checkinteger(L, 2);
    /* 第 3 个参数是 I2C 频率，默认 400kHz。 */
    int freq = luaL_optinteger(L, 3, 400000);

    /* 当前实现把 I2C 主频上限限制在 1MHz。 */
    if (freq > 1000000) {
        freq = 1000000;
    }

    /*
     * 如果此前已经初始化过 I2C 总线，
     * 这里先把旧的 device 句柄和 bus 一起清掉，
     * 避免重新 setup 时残留旧配置。
     */
    if (i2c_bus_handle) {
        for (int i = 0; i < i2c_dev_count; i++) {
            if (i2c_dev_cache[i].handle) {
                i2c_master_bus_rm_device(i2c_dev_cache[i].handle);
            }
        }
        i2c_dev_count = 0;
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
    }

    /* 记录当前 I2C 总线频率，后续 scan / add device 都会复用这个值。 */
    i2c_bus_freq = freq;

    /* 构造 I2C 主机总线配置。 */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    /* 调用 ESP-IDF 创建新的 I2C master bus。 */
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus_handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.setup failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_i2c_write(lua_State *L)
{
    /* 第 1 个参数是 7 位 I2C 从设备地址。 */
    int addr = luaL_checkinteger(L, 1);
    /* 获取 Lua 传进来的总参数个数。 */
    int nargs = lua_gettop(L);

    /* 本地发送缓冲区，所有待发送数据最终都会扁平化到这里。 */
    uint8_t buf[I2C_WRITE_BUF_SZ];
    /*
     * 从第 2 个参数开始，把整数 / 字符串 / table 统一展开为连续字节流。
     * 这样 Lua 侧可以混合多种输入形式。
     */
    int len = lua_build_byte_buffer(L, 2, nargs, buf, I2C_WRITE_BUF_SZ);

    /* 如果没有任何有效字节，就直接返回，不发送空事务。 */
    if (len == 0) return 0;

    /* 获取或懒创建指定地址对应的 I2C 设备句柄。 */
    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    /* 把扁平化后的字节流一次性发给目标设备。 */
    esp_err_t ret = i2c_master_transmit(dev, buf, len, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.write failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_i2c_read(lua_State *L)
{
    /* 第 1 个参数是目标 I2C 地址。 */
    int addr = luaL_checkinteger(L, 1);
    /* 第 2 个参数是希望读取的字节数。 */
    int rlen = luaL_checkinteger(L, 2);
    /* 读取长度不能超过本地接收缓冲区容量。 */
    if (rlen > I2C_READ_BUF_SZ) rlen = I2C_READ_BUF_SZ;

    /* 获取设备句柄，没有就报错。 */
    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    /* 接收原始字节到本地缓冲区。 */
    uint8_t buf[I2C_READ_BUF_SZ];
    esp_err_t ret = i2c_master_receive(dev, buf, rlen, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.read failed: %s", esp_err_to_name(ret));
    }

    /* 把 C 侧字节数组转换成 Lua 数组 table 返回给脚本。 */
    lua_createtable(L, rlen, 0);
    for (int i = 0; i < rlen; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_i2c_write_read(lua_State *L)
{
    /* 第 1 个参数是设备地址。 */
    int addr = luaL_checkinteger(L, 1);

    /*
     * 第 2 个参数是写前导数据。
     * 常见用法是先写寄存器地址，再紧接着读返回值。
     */
    uint8_t wbuf[I2C_WRITE_BUF_SZ];
    int wlen = lua_build_byte_buffer(L, 2, 2, wbuf, I2C_WRITE_BUF_SZ);

    /* 第 3 个参数是希望读取的字节数。 */
    int rlen = luaL_checkinteger(L, 3);
    if (rlen > I2C_READ_BUF_SZ) rlen = I2C_READ_BUF_SZ;

    /* 获取设备句柄。 */
    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    /* 先发 wbuf，再连续读取 rbuf。 */
    uint8_t rbuf[I2C_READ_BUF_SZ];
    esp_err_t ret = i2c_master_transmit_receive(dev, wbuf, wlen, rbuf, rlen, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.write_read failed: %s", esp_err_to_name(ret));
    }

    /* 把读回来的字节包装成 Lua table 返回。 */
    lua_createtable(L, rlen, 0);
    for (int i = 0; i < rlen; i++) {
        lua_pushinteger(L, rbuf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_i2c_scan(lua_State *L)
{
    if (!i2c_bus_handle) {
        return luaL_error(L, "i2c not initialized");
    }

    lua_createtable(L, 0, 0);
    int found = 0;
    for (int addr = 1; addr < 127; addr++) {
        i2c_master_dev_handle_t dev = NULL;
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = i2c_bus_freq,
        };
        if (i2c_master_bus_add_device(i2c_bus_handle, &cfg, &dev) == ESP_OK) {
            esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, I2C_SCAN_TIMEOUT_MS);
            i2c_master_bus_rm_device(dev);
            if (ret == ESP_OK) {
                lua_pushinteger(L, addr);
                lua_rawseti(L, -2, ++found);
            }
        }
    }
    return 1;
}

/* ── Lua C bindings: spi ────────────────────────────────────────── */

#define SPI_WRITE_BUF_SZ 4096
#define SPI_READ_BUF_SZ 4096
#define SPI_TIMEOUT_MS  1000
#define SPI_MIN_FREQ_HZ 100000
#define SPI_MAX_FREQ_HZ 40000000

static spi_device_handle_t spi_handle = NULL;
static int spi_dc_pin = -1;
static int spi_res_pin = -1;
static bool spi_bus_initialized = false;
static int spi_bus_mosi_pin = -1;
static int spi_bus_clk_pin = -1;
static int spi_bus_max_transfer_sz = 0;

/* Keep SPI frequency limits consistent across spi.setup and lcd.setup. */
/*
 * 统一裁剪 SPI 频率范围。
 *
 * 这样 spi.setup 和 lcd.setup 不会各自维护一套上下限逻辑，
 * 后续调整频率范围时也只需要改一个地方。
 */
static int clamp_spi_freq(int freq)
{
    if (freq > SPI_MAX_FREQ_HZ) {
        return SPI_MAX_FREQ_HZ;
    }
    if (freq < SPI_MIN_FREQ_HZ) {
        return SPI_MIN_FREQ_HZ;
    }
    return freq;
}

static bool is_optional_output_gpio(int pin)
{
    return pin < 0 || GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

/*
 * 释放 spi.* Lua API 使用的通用 SPI device 句柄。
 *
 * 这里只释放“设备”，不释放“总线”。
 * 因为总线可能还会被 LCD 或后续的 SPI 设备继续复用。
 */
static esp_err_t spi_remove_device(void)
{
    if (!spi_handle) {
        return ESP_OK;
    }

    esp_err_t ret = spi_bus_remove_device(spi_handle);
    if (ret == ESP_OK) {
        spi_handle = NULL;
    }
    return ret;
}

/*
 * 确保共享 SPI 总线满足当前请求。
 *
 * 这个函数负责处理“是否需要复用已有总线”以及“是否必须重建总线”。
 *
 * 触发重建的条件：
 * 1. MOSI 引脚变了
 * 2. CLK 引脚变了
 * 3. 新请求需要更大的 max_transfer_sz，而当前总线不够用
 *
 * 只有在已有总线无法满足新请求时，才会真正执行 spi_bus_free + spi_bus_initialize。
 */
static esp_err_t spi_ensure_bus(int mosi, int clk, int max_transfer_sz)
{
    bool needs_reinit = false;

    /*
     * 如果当前总线已经初始化，就比较“当前配置”和“目标配置”是否兼容。
     * 不兼容才需要重建。
     */
    if (spi_bus_initialized) {
        needs_reinit = (spi_bus_mosi_pin != mosi) ||
                       (spi_bus_clk_pin != clk) ||
                       (spi_bus_max_transfer_sz < max_transfer_sz);
    }

    if (needs_reinit) {
        /*
         * 释放底层 SPI bus。
         * 注意：调用这个函数前，上层应先确保设备句柄已被移除。
         */
        esp_err_t ret = spi_bus_free(SPI2_HOST);
        if (ret != ESP_OK) {
            return ret;
        }
        /*
         * 释放成功后，把本地缓存状态一起清空。
         * 这样后续会进入重新初始化流程。
         */
        spi_bus_initialized = false;
        spi_bus_mosi_pin = -1;
        spi_bus_clk_pin = -1;
        spi_bus_max_transfer_sz = 0;
    }

    if (!spi_bus_initialized) {
        /*
         * 创建新的 SPI bus 配置。
         * 这里只配置总线级别的引脚与最大传输能力，
         * 具体设备参数由 spi.setup 或 lcd.setup 各自继续补充。
         */
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = mosi,
            .miso_io_num = -1,
            .sclk_io_num = clk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = max_transfer_sz,
        };

        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            return ret;
        }

        /*
         * 初始化成功后，记录当前总线的实际配置，
         * 供后续复用判断使用。
         */
        spi_bus_initialized = true;
        spi_bus_mosi_pin = mosi;
        spi_bus_clk_pin = clk;
        spi_bus_max_transfer_sz = max_transfer_sz;
    }

    return ESP_OK;
}

static int l_spi_setup(lua_State *L)
{
    /* 参数依次为 MOSI、CLK、CS、DC、RES、频率。 */
    int mosi = luaL_checkinteger(L, 1);
    int clk = luaL_checkinteger(L, 2);
    int cs = luaL_optinteger(L, 3, -1);
    int dc = luaL_optinteger(L, 4, -1);
    int res = luaL_optinteger(L, 5, -1);
    int freq = luaL_optinteger(L, 6, 1000000);

    /* MOSI 和 CLK 必须是合法输出引脚。 */
    if (!GPIO_IS_VALID_OUTPUT_GPIO(mosi)) {
        return luaL_error(L, "spi.setup invalid MOSI pin: %d", mosi);
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(clk)) {
        return luaL_error(L, "spi.setup invalid CLK pin: %d", clk);
    }
    /* CS / DC / RES 允许不传；如果传了，就必须是合法输出引脚。 */
    if (!is_optional_output_gpio(cs)) {
        return luaL_error(L, "spi.setup invalid CS pin: %d", cs);
    }
    if (!is_optional_output_gpio(dc)) {
        return luaL_error(L, "spi.setup invalid DC pin: %d", dc);
    }
    if (!is_optional_output_gpio(res)) {
        return luaL_error(L, "spi.setup invalid RES pin: %d", res);
    }
    /* 把频率裁剪到统一允许范围。 */
    freq = clamp_spi_freq(freq);

    /* 如果此前存在普通 SPI 设备句柄，先移除。 */
    esp_err_t ret = spi_remove_device();
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.setup remove device failed: %s", esp_err_to_name(ret));
    }

    /* 保存当前 DC / RES 引脚，供 spi.write / spi.dc 复用。 */
    spi_dc_pin = dc;
    spi_res_pin = res;

    /* 如果配置了 DC 引脚，就先初始化为输出，并默认拉低表示命令态。 */
    if (dc >= 0) {
        gpio_set_direction(dc, GPIO_MODE_OUTPUT);
        gpio_set_level(dc, 0);  /* DC=0 for command mode initially */
        ESP_LOGI(TAG, "SPI DC pin=%d initialized", dc);
    }
    /* 如果配置了 RES 引脚，就执行一次简单的硬件复位脉冲。 */
    if (res >= 0) {
        gpio_set_direction(res, GPIO_MODE_OUTPUT);
        gpio_set_level(res, 1);
        gpio_set_level(res, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(res, 1);
        ESP_LOGI(TAG, "SPI RES pin=%d initialized", res);
    }

    /* 先判断当前 bus 是否可以直接复用，便于后面打日志。 */
    bool reuse_bus = spi_bus_initialized &&
                     spi_bus_mosi_pin == mosi &&
                     spi_bus_clk_pin == clk &&
                     spi_bus_max_transfer_sz >= SPI_WRITE_BUF_SZ;
    /* 确保底层 SPI bus 已经准备好，必要时自动重建。 */
    ret = spi_ensure_bus(mosi, clk, SPI_WRITE_BUF_SZ);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.setup bus failed: %s", esp_err_to_name(ret));
    }
    if (reuse_bus) {
        ESP_LOGD(TAG, "SPI bus already initialized, reusing");
    }

    /* 这里配置的是“设备级”参数，而不是 bus 级参数。 */
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = freq,
        .input_delay_ns = 0,
        .spics_io_num = cs,
        .flags = 0,
        .queue_size = 1,
    };

    /* 把这个 SPI 设备挂到已经准备好的 SPI2_HOST 总线上。 */
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.setup add device failed: %s", esp_err_to_name(ret));
    }

    return 0;
}

static int l_spi_transfer(lua_State *L)
{
    /* transfer 需要先完成 spi.setup。 */
    if (!spi_handle) {
        return luaL_error(L, "spi not initialized");
    }

    /* 获取全部入参个数，这里所有参数都被视为待发送数据。 */
    int nargs = lua_gettop(L);
    uint8_t tx_buf[SPI_WRITE_BUF_SZ];
    /* 把 Lua 参数展开成连续发送缓冲区。 */
    int tx_len = lua_build_byte_buffer(L, 1, nargs, tx_buf, SPI_WRITE_BUF_SZ);

    /* 这里做全双工传输，接收长度与发送长度保持一致。 */
    int rx_len = tx_len;
    uint8_t rx_buf[SPI_READ_BUF_SZ];

    /* 配置一次同步 SPI 事务。length/rxlength 单位都是 bit。 */
    spi_transaction_t t = {
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
        .length = tx_len * 8,
        .rxlength = rx_len * 8,
    };

    /* 发起同步传输。 */
    esp_err_t ret = spi_device_transmit(spi_handle, &t);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.transfer failed: %s", esp_err_to_name(ret));
    }

    /* 把收到的字节流转换成 Lua table 返回。 */
    lua_createtable(L, rx_len, 0);
    for (int i = 0; i < rx_len; i++) {
        lua_pushinteger(L, rx_buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_spi_write(lua_State *L)
{
    /* write 需要先完成 spi.setup。 */
    if (!spi_handle) {
        return luaL_error(L, "spi not initialized");
    }

    /*
     * 第 1 个参数默认解释为 DC 电平：
     * - 0：命令
     * - 1：数据
     */
    int dc_value = luaL_optinteger(L, 1, 1);
    int nargs = lua_gettop(L);

    /* 如果配置过 DC 引脚，就在发送前先切换电平。 */
    if (spi_dc_pin >= 0) {
        gpio_set_level(spi_dc_pin, dc_value);
        ESP_LOGD(TAG, "spi.write: dc_pin=%d value=%d", spi_dc_pin, dc_value);
    }

    uint8_t tx_buf[SPI_WRITE_BUF_SZ];
    /* 从第 2 个参数开始才是真正要发出去的数据。 */
    int tx_len = lua_build_byte_buffer(L, 2, nargs, tx_buf, SPI_WRITE_BUF_SZ);

    /* write 是纯发送事务，不关心返回数据。 */
    spi_transaction_t t = {
        .tx_buffer = tx_buf,
        .length = tx_len * 8,
    };

    /* 执行同步发送。 */
    esp_err_t ret = spi_device_transmit(spi_handle, &t);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.write failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGD(TAG, "spi.write dc=%d len=%d", dc_value, tx_len);
    return 0;
}

static int l_spi_dc(lua_State *L)
{
    int value = luaL_checkinteger(L, 1);
    if (spi_dc_pin >= 0) {
        gpio_set_level(spi_dc_pin, value);
    }
    return 0;
}

/* ── Lua C bindings: lcd (ST7735) with double buffering ─────────── */

#define LCD_WIDTH  160
#define LCD_HEIGHT 80

static const st7735_lcd_init_cmd_t st7735_init_cmds[] = {
    {ST7735_SWRESET, NULL, 0, 150},
    {ST7735_SLPOUT, NULL, 0, 500},
    {ST7735_COLMOD, (uint8_t[]){0x05}, 1, 0},
    {ST7735_MADCTL, (uint8_t[]){0x68}, 1, 0},
    {ST7735_NORON, NULL, 0, 10},
    {ST7735_DISPON, NULL, 0, 100},
};

static int lcd_dc_pin = -1;
static int lcd_cs_pin = -1;
static int lcd_res_pin = -1;
static int lcd_bl_pin = -1;
static bool lcd_bl_pwm_initialized = false;
static int lcd_brightness_val = 255;  // Default to full brightness
static uint16_t *lcd_framebuf = NULL;
static volatile bool lcd_dma_busy = false;
static SemaphoreHandle_t lcd_flush_sem = NULL;
static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
static esp_lcd_panel_handle_t lcd_panel_handle = NULL;
static bool lcd_initialized = false;

static int dirty_x1 = LCD_WIDTH, dirty_y1 = LCD_HEIGHT, dirty_x2 = 0, dirty_y2 = 0;

static void fb_mark_dirty(int x, int y, int w, int h)
{
    if (x < dirty_x1) dirty_x1 = x;
    if (y < dirty_y1) dirty_y1 = y;
    if (x + w > dirty_x2) dirty_x2 = x + w;
    if (y + h > dirty_y2) dirty_y2 = y + h;
    if (dirty_x1 < 0) dirty_x1 = 0;
    if (dirty_y1 < 0) dirty_y1 = 0;
    if (dirty_x2 > LCD_WIDTH) dirty_x2 = LCD_WIDTH;
    if (dirty_y2 > LCD_HEIGHT) dirty_y2 = LCD_HEIGHT;
}

static void fb_clear_dirty(void)
{
    dirty_x1 = LCD_WIDTH;
    dirty_y1 = LCD_HEIGHT;
    dirty_x2 = 0;
    dirty_y2 = 0;
}

/*
 * 释放 LCD panel / panel_io 相关对象。
 *
 * 这里故意不处理 framebuffer，
 * 因为 panel/io 与 framebuffer 是两类不同资源：
 * - panel/io 属于外设驱动对象
 * - framebuffer/semaphore 属于显示缓存与同步资源
 *
 * 分开处理后，重试初始化或局部清理会更灵活。
 */
static void lcd_release_panel(void)
{
    if (lcd_panel_handle) {
        esp_lcd_panel_del(lcd_panel_handle);
        lcd_panel_handle = NULL;
    }
    if (lcd_io_handle) {
        esp_lcd_panel_io_del(lcd_io_handle);
        lcd_io_handle = NULL;
    }
    lcd_initialized = false;
    lcd_dma_busy = false;
    lcd_bl_pwm_initialized = false;
    lcd_dc_pin = -1;
    lcd_cs_pin = -1;
    lcd_res_pin = -1;
    fb_clear_dirty();
}

/*
 * 释放 LCD 相关缓存资源。
 *
 * 这里主要负责：
 * 1. 释放 DMA framebuffer
 * 2. 释放 flush 同步信号量
 * 3. 重置 DMA busy 和 dirty 区域状态
 */
static void lcd_free_buffers(void)
{
    if (lcd_framebuf) {
        heap_caps_free(lcd_framebuf);
        lcd_framebuf = NULL;
    }
    if (lcd_flush_sem) {
        vSemaphoreDelete(lcd_flush_sem);
        lcd_flush_sem = NULL;
    }
    lcd_dma_busy = false;
    fb_clear_dirty();
}

static bool fb_has_dirty(void)
{
    return dirty_x2 > dirty_x1 && dirty_y2 > dirty_y1;
}

static bool lcd_alloc_framebuf(void)
{
    if (lcd_framebuf) return true;
    
    if (!lcd_initialized || !lcd_panel_handle) {
        return false;
    }
    
    lcd_framebuf = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_DMA);
    if (!lcd_framebuf) {
        ESP_LOGE(TAG, "Failed to allocate lazy frame buffer");
        return false;
    }
    
    lcd_flush_sem = xSemaphoreCreateBinary();
    if (!lcd_flush_sem) {
        heap_caps_free(lcd_framebuf);
        lcd_framebuf = NULL;
        return false;
    }
    
    memset(lcd_framebuf, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    fb_clear_dirty();
    ESP_LOGI(TAG, "Lazy framebuffer allocated: %d bytes", LCD_WIDTH * LCD_HEIGHT * 2);
    return true;
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    lcd_dma_busy = false;
    if (lcd_flush_sem) {
        xSemaphoreGive(lcd_flush_sem);
    }
    return false;
}

/* 8x8 font (95 characters: 0x20-0x7E) - const goes to flash by default on ESP32 */
static const uint8_t FONT_8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00},
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00},
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    {0xFE,0xC0,0xC0,0xFC,0x06,0xC6,0x7C,0x00},
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00},
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3A,0x00},
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xCE,0x7C,0x0E},
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    {0x7C,0xC6,0xE0,0x70,0x1C,0xC6,0x7C,0x00},
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00},
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00},
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    {0x38,0x6C,0x64,0xF0,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C},
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00},
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00},
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00},
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC},
    {0x00,0x00,0x7E,0x4C,0x18,0x32,0x7E,0x00},
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* Frame buffer helper functions */
static inline uint16_t swap_bytes(uint16_t val)
{
    return (val >> 8) | (val << 8);
}

static inline void fb_set_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT && lcd_framebuf) {
        lcd_framebuf[y * LCD_WIDTH + x] = swap_bytes(color);
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    
    uint16_t swapped = swap_bytes(color);
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            lcd_framebuf[py * LCD_WIDTH + px] = swapped;
        }
    }
    fb_mark_dirty(x, y, w, h);
}

static void fb_draw_char(int x, int y, char c, uint16_t fg, int has_bg, uint16_t bg, int scale)
{
    int idx = c - ' ';
    if (idx < 0 || idx >= 95) return;
    
    int char_w = 8 * scale;
    int char_h = 8 * scale;
    
    for (int row = 0; row < 8; row++) {
        uint8_t line = FONT_8X8[idx][row];
        for (int col = 0; col < 8; col++) {
            int is_fg = (line & (0x80 >> col)) != 0;
            
            if (!is_fg && !has_bg) continue;
            
            uint16_t color = is_fg ? fg : bg;
            int px = x + col * scale;
            int py = y + row * scale;
            
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fb_set_pixel(px + sx, py + sy, color);
                }
            }
        }
    }
    fb_mark_dirty(x, y, char_w, char_h);
}

static int l_lcd_setup(lua_State *L)
{
    /* 参数依次为 MOSI、CLK、CS、DC、RES、BL、频率。 */
    int mosi = luaL_checkinteger(L, 1);
    int clk = luaL_checkinteger(L, 2);
    int cs = luaL_optinteger(L, 3, -1);
    int dc = luaL_optinteger(L, 4, 10);
    int res = luaL_optinteger(L, 5, 6);
    int bl = luaL_optinteger(L, 6, 11);
    int freq = luaL_optinteger(L, 7, 20000000);

    /* LCD 基于 SPI，所以这些引脚都要求是合法输出引脚。 */
    if (!GPIO_IS_VALID_OUTPUT_GPIO(mosi)) {
        return luaL_error(L, "lcd.setup invalid MOSI pin: %d", mosi);
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(clk)) {
        return luaL_error(L, "lcd.setup invalid CLK pin: %d", clk);
    }
    if (!is_optional_output_gpio(cs)) {
        return luaL_error(L, "lcd.setup invalid CS pin: %d", cs);
    }
    if (!is_optional_output_gpio(dc)) {
        return luaL_error(L, "lcd.setup invalid DC pin: %d", dc);
    }
    if (!is_optional_output_gpio(res)) {
        return luaL_error(L, "lcd.setup invalid RES pin: %d", res);
    }
    if (!is_optional_output_gpio(bl)) {
        return luaL_error(L, "lcd.setup invalid BL pin: %d", bl);
    }
    /* 对 LCD SPI 时钟做统一裁剪。 */
    freq = clamp_spi_freq(freq);

    /* 记录 LCD 引脚配置，后续亮度/刷新流程可能会用到。 */
    lcd_cs_pin = cs;
    lcd_dc_pin = dc;
    lcd_res_pin = res;
    lcd_bl_pin = bl;

    /* 打印一次完整配置，方便串口日志排查。 */
    ESP_LOGI(TAG, "ST7735: mosi=%d clk=%d cs=%d dc=%d res=%d bl=%d freq=%d", mosi, clk, cs, dc, res, bl, freq);

    /*
     * LCD 和普通 SPI API 共用 SPI bus。
     * 所以这里先把 spi.* 可能残留的通用 device 句柄移除掉。
     */
    esp_err_t ret = spi_remove_device();
    if (ret != ESP_OK) {
        return luaL_error(L, "lcd.setup remove spi device failed: %s", esp_err_to_name(ret));
    }

    /* 再释放旧的 LCD panel / io 对象，准备重新初始化。 */
    lcd_release_panel();

    /* 判断当前 SPI bus 是否可以直接复用。 */
    bool reuse_bus = spi_bus_initialized &&
                     spi_bus_mosi_pin == mosi &&
                     spi_bus_clk_pin == clk &&
                     spi_bus_max_transfer_sz >= LCD_WIDTH * LCD_HEIGHT * 2;
    /* 确保 LCD 所需的底层 SPI bus 已准备完成。 */
    ret = spi_ensure_bus(mosi, clk, LCD_WIDTH * LCD_HEIGHT * 2);
    if (ret != ESP_OK) {
        return luaL_error(L, "lcd.setup bus failed: %s", esp_err_to_name(ret));
    }
    if (reuse_bus) {
        ESP_LOGW(TAG, "ST7735: SPI bus already initialized, reusing");
    }

    /* 配置 LCD 的 panel_io，也就是 LCD 命令/参数传输层。 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = dc,
        .cs_gpio_num = cs,
        .spi_mode = 0,
        .pclk_hz = freq,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
    };

    /* 创建 SPI 版 LCD panel_io。 */
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &lcd_io_handle);
    if (ret != ESP_OK) {
        lcd_release_panel();
        return luaL_error(L, "lcd.setup panel io failed: %s", esp_err_to_name(ret));
    }

    /* 提供 ST7735 的上电初始化命令表。 */
    st7735_vendor_config_t vendor_config = {
        .init_cmds = st7735_init_cmds,
        .init_cmds_size = sizeof(st7735_init_cmds) / sizeof(st7735_lcd_init_cmd_t),
    };

    /* 配置 LCD panel 实例。 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = res,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .flags.reset_active_high = false,
        .vendor_config = &vendor_config,
    };

    /* 创建 ST7735 panel 对象。 */
    ret = esp_lcd_new_panel_st7735(lcd_io_handle, &panel_cfg, &lcd_panel_handle);
    if (ret != ESP_OK) {
        lcd_release_panel();
        return luaL_error(L, "lcd.setup panel failed: %s", esp_err_to_name(ret));
    }

    /* 按顺序执行 LCD 硬复位、初始化、设置显示偏移。 */
    ret = esp_lcd_panel_reset(lcd_panel_handle);
    if (ret != ESP_OK) {
        lcd_release_panel();
        return luaL_error(L, "lcd.setup reset failed: %s", esp_err_to_name(ret));
    }

    ret = esp_lcd_panel_init(lcd_panel_handle);
    if (ret != ESP_OK) {
        lcd_release_panel();
        return luaL_error(L, "lcd.setup init failed: %s", esp_err_to_name(ret));
    }

    ret = esp_lcd_panel_set_gap(lcd_panel_handle, 0, 24);
    if (ret != ESP_OK) {
        lcd_release_panel();
        return luaL_error(L, "lcd.setup set gap failed: %s", esp_err_to_name(ret));
    }

    /*
     * 初始化背光控制。
     * 优先尝试 PWM 调光；如果失败，就回退到普通 GPIO 开关模式。
     */
    if (lcd_bl_pin >= 0) {
        ledc_channel_config_t ledc_conf = {
            .channel = LEDC_CHANNEL_0,
            .duty = 0,
            .gpio_num = lcd_bl_pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .hpoint = 0,
            .timer_sel = LEDC_TIMER_0,
        };
        esp_err_t ret = ledc_channel_config(&ledc_conf);
        if (ret == ESP_OK) {
            ledc_timer_config_t timer_conf = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .duty_resolution = LEDC_TIMER_8_BIT,
                .timer_num = LEDC_TIMER_0,
                .freq_hz = 1000,
                .clk_cfg = LEDC_AUTO_CLK,
            };
            ret = ledc_timer_config(&timer_conf);
            if (ret == ESP_OK) {
                lcd_bl_pwm_initialized = true;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                ESP_LOGI(TAG, "ST7735: Backlight PWM initialized (pin=%d)", lcd_bl_pin);
            } else {
                ESP_LOGW(TAG, "ST7735: Backlight timer config failed: %s, using GPIO", esp_err_to_name(ret));
                goto bl_gpio_fallback;
            }
        } else {
        bl_gpio_fallback:
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << lcd_bl_pin),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&io_conf);
            gpio_set_level(lcd_bl_pin, 1);
            ESP_LOGI(TAG, "ST7735: Backlight ON (GPIO mode)");
        }
    }

    /*
     * 如果还没有创建 framebuffer / semaphore，就在这里分配。
     * framebuffer 使用 DMA-capable 内存，便于后续直接刷屏。
     */
    if (lcd_framebuf == NULL) {
        lcd_flush_sem = xSemaphoreCreateBinary();
        if (lcd_flush_sem == NULL) {
            lcd_release_panel();
            return luaL_error(L, "lcd.setup: semaphore create failed");
        }
        lcd_framebuf = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_DMA);
        if (lcd_framebuf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer");
            lcd_free_buffers();
            lcd_release_panel();
            return luaL_error(L, "lcd.setup: frame buffer allocation failed");
        }
        memset(lcd_framebuf, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    }

    /* 到这里说明 LCD 所需对象都已准备完成。 */
    lcd_initialized = true;
    ESP_LOGI(TAG, "ST7735 LCD initialized (single buffer: %d bytes)", LCD_WIDTH * LCD_HEIGHT * 2);
    return 0;
}

static int l_lcd_clear(lua_State *L)
{
    int color = luaL_optinteger(L, 1, 0);
    if (!lcd_framebuf) {
        if (!lcd_alloc_framebuf()) return 0;
    }
    
    uint16_t swapped = swap_bytes(color);
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        lcd_framebuf[i] = swapped;
    }
    fb_mark_dirty(0, 0, LCD_WIDTH, LCD_HEIGHT);
    return 0;
}

static int l_lcd_fill(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    
    if (!lcd_framebuf) {
        if (!lcd_alloc_framebuf()) return 0;
    }
    
    fb_fill_rect(x, y, w, h, color);
    return 0;
}

static int l_lcd_pixel(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int color = luaL_checkinteger(L, 3);
    
    if (!lcd_framebuf) {
        if (!lcd_alloc_framebuf()) return 0;
    }
    
    fb_set_pixel(x, y, color);
    fb_mark_dirty(x, y, 1, 1);
    return 0;
}

static int l_lcd_print(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    
    size_t len = 0;
    const char *lua_str = luaL_checklstring(L, 3, &len);
    
    if (!lcd_framebuf) {
        if (!lcd_alloc_framebuf()) return 0;
    }
    
    char local_buf[64];
    if (len >= sizeof(local_buf)) {
        len = sizeof(local_buf) - 1;
    }
    memcpy(local_buf, lua_str, len);
    local_buf[len] = '\0';
    
    int fg_color = luaL_optinteger(L, 4, 0xFFFF);
    int has_bg = !lua_isnoneornil(L, 5);
    int bg_color = has_bg ? luaL_checkinteger(L, 5) : 0;
    int scale = luaL_optinteger(L, 6, 1);
    
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    
    int cursor_x = x;
    char *str = local_buf;
    while (*str) {
        fb_draw_char(cursor_x, y, *str, fg_color, has_bg, bg_color, scale);
        cursor_x += 8 * scale;
        str++;
    }
    return 0;
}

static int l_lcd_draw_pixels(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    size_t data_len;
    const char *data = luaL_checklstring(L, 5, &data_len);
    
    int expected = w * h * 2;
    if (data_len < expected) {
        return luaL_error(L, "pixel data too short: need %d bytes, got %d", expected, (int)data_len);
    }
    
    if (!lcd_framebuf) {
        if (!lcd_alloc_framebuf()) {
            return luaL_error(L, "LCD not initialized");
        }
    }
    
    int clipped_x = x;
    int clipped_y = y;
    int clipped_w = w;
    int clipped_h = h;
    
    if (clipped_x < 0) { clipped_w += clipped_x; clipped_x = 0; }
    if (clipped_y < 0) { clipped_h += clipped_y; clipped_y = 0; }
    if (clipped_x + clipped_w > LCD_WIDTH) clipped_w = LCD_WIDTH - clipped_x;
    if (clipped_y + clipped_h > LCD_HEIGHT) clipped_h = LCD_HEIGHT - clipped_y;
    if (clipped_w <= 0 || clipped_h <= 0) return 0;
    
    int src_start = (clipped_y - y) * w * 2 + (clipped_x - x) * 2;
    
    for (int row = 0; row < clipped_h; row++) {
        int dst_idx = (clipped_y + row) * LCD_WIDTH + clipped_x;
        int src_idx = src_start + row * w * 2;
        memcpy(&lcd_framebuf[dst_idx], data + src_idx, clipped_w * 2);
    }
    
    fb_mark_dirty(clipped_x, clipped_y, clipped_w, clipped_h);
    return 0;
}

static int l_lcd_flush(lua_State *L)
{
    (void)L;
    if (!lcd_panel_handle) return 0;
    
    if (!lcd_framebuf) {
        if (!lcd_alloc_framebuf()) return 0;
    }

    if (!fb_has_dirty()) {
        return 0;
    }

    if (lcd_dma_busy && lcd_flush_sem) {
        if (xSemaphoreTake(lcd_flush_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "LCD DMA timeout, skipping flush");
            return 0;
        }
    }

    lcd_dma_busy = true;
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, dirty_x1, dirty_y1, dirty_x2, dirty_y2, lcd_framebuf);

    fb_clear_dirty();
    return 0;
}

static int l_lcd_brightness(lua_State *L)
{
    int brightness = luaL_optinteger(L, 1, 255);
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    lcd_brightness_val = brightness;

    if (!lcd_bl_pwm_initialized) {
        if (lcd_bl_pin >= 0) {
            gpio_set_level(lcd_bl_pin, brightness > 127 ? 1 : 0);
            ESP_LOGW(TAG, "ST7735: PWM not initialized, using GPIO on/off");
        }
        lua_pushinteger(L, brightness);
        return 1;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGD(TAG, "ST7735: Brightness set to %d", brightness);
    lua_pushinteger(L, brightness);
    return 1;
}

static int l_lcd_get_brightness(lua_State *L)
{
    lua_pushinteger(L, lcd_brightness_val);
    return 1;
}

static const luaL_Reg lcd_lib[] = {
    {"setup",       l_lcd_setup},
    {"clear",       l_lcd_clear},
    {"fill",        l_lcd_fill},
    {"pixel",       l_lcd_pixel},
    {"print",       l_lcd_print},
    {"draw_pixels", l_lcd_draw_pixels},
    {"flush",       l_lcd_flush},
    {"brightness",  l_lcd_brightness},
    {"get_brightness", l_lcd_get_brightness},
    {NULL, NULL}
};

static const luaL_Reg spi_lib[] = {
    {"setup",    l_spi_setup},
    {"transfer", l_spi_transfer},
    {"write",    l_spi_write},
    {"dc",       l_spi_dc},
    {NULL, NULL}
};

static const luaL_Reg i2c_lib[] = {
    {"setup",      l_i2c_setup},
    {"write",      l_i2c_write},
    {"read",       l_i2c_read},
    {"write_read", l_i2c_write_read},
    {"scan",       l_i2c_scan},
    {NULL, NULL}
};

/* ── Register all C libraries into a Lua state ──────────────────── */

static void register_libs(lua_State *L)
{
    luaL_newlib(L, gpio_lib);   lua_setglobal(L, "gpio");
    luaL_newlib(L, time_lib);   lua_setglobal(L, "time");
    luaL_newlib(L, log_lib);    lua_setglobal(L, "log");
    luaL_newlib(L, system_lib); lua_setglobal(L, "system");
    luaL_newlib(L, wifi_lib);   lua_setglobal(L, "wifi");
    luaL_newlib(L, i2c_lib);    lua_setglobal(L, "i2c");
    luaL_newlib(L, spi_lib);    lua_setglobal(L, "spi");
    luaL_newlib(L, lcd_lib);    lua_setglobal(L, "lcd");
}

/* ── Lua VM lifecycle ───────────────────────────────────────────── */

static lua_State* create_vm(void)
{
    lua_mem_current = 0;
    lua_mem_peak = 0;

    lua_State *state = lua_newstate(lua_tracking_alloc, NULL);
    if (!state) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return NULL;
    }
    luaL_openlibs(state);
    register_libs(state);
    return state;
}

static void destroy_vm(lua_State *state)
{
    if (state) {
        lua_close(state);
    }
}

/* ── Load script with fallback (SPIFFS → embedded) ───────────────── */

static int load_script_with_fallback(lua_State *L, const char *spiffs_path,
                                     const char *default_script, size_t default_len)
{
    int ret = luaL_loadfile(L, spiffs_path);
    if (ret == LUA_OK) {
        ret = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (ret == LUA_OK) {
            ESP_LOGI(TAG, "Loaded from SPIFFS: %s", spiffs_path);
            return LUA_OK;
        }
        const char *err = lua_tostring(L, -1);
        ESP_LOGW(TAG, "SPIFFS %s failed: %s, falling back to embedded",
                 spiffs_path, err ? err : "unknown");
        lua_pop(L, 1);
    } else {
        ESP_LOGW(TAG, "SPIFFS %s not found or load error, using embedded", spiffs_path);
    }

    ret = luaL_loadbuffer(L, default_script, default_len, "embedded_main");
    if (ret == LUA_OK) {
        ret = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (ret == LUA_OK) {
            ESP_LOGI(TAG, "Loaded from embedded default");
            return LUA_OK;
        }
    }
    return ret;
}

/* ── Lua task (runs main.lua) ───────────────────────────────────── */

static void lua_task(void *pvParameters)
{
    lua_task_running = true;
    ESP_LOGI(TAG, "Lua task started, executing main.lua");

    int ret = load_script_with_fallback(L, SPIFFS_BASE_PATH "/main.lua",
                                        (const char *)default_main_lua_start,
                                        default_main_lua_length);
    if (ret != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        ESP_LOGE(TAG, "main.lua error: %s", err ? err : "unknown");
        lua_pop(L, 1);
    }

    ESP_LOGI(TAG, "Lua task finished (main.lua returned)");
    lua_task_running = false;
    lua_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t lua_runtime_init(void)
{
    esp_err_t ret = spiffs_init();
    if (ret != ESP_OK) return ret;

    ret = write_default_script();
    if (ret != ESP_OK) return ret;

    L = create_vm();
    if (!L) return ESP_FAIL;

    ESP_LOGI(TAG, "Lua runtime initialized");
    return ESP_OK;
}

esp_err_t lua_runtime_start(void)
{
    if (lua_task_handle) {
        ESP_LOGW(TAG, "Lua task already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(lua_task, "lua_task", LUA_TASK_STACK,
                                  NULL, LUA_TASK_PRIO, &lua_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Lua task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lua_runtime_restart(void)
{
    ESP_LOGI(TAG, "Restarting Lua VM");

    /* Stop running task */
    if (lua_task_handle) {
        vTaskDelete(lua_task_handle);
        lua_task_handle = NULL;
        lua_task_running = false;
        /* Give a tick for task cleanup */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Release hardware resources before destroying VM */
    lcd_free_buffers();
    lcd_release_panel();
    spi_remove_device();

    /* Destroy and recreate VM (task is dead, safe to access directly) */
    destroy_vm(L);
    L = create_vm();

    if (!L) {
        ESP_LOGE(TAG, "Failed to recreate Lua VM");
        return ESP_FAIL;
    }

    /* Restart task */
    return lua_runtime_start();
}

esp_err_t lua_runtime_exec(const char *code, char *result, size_t max_len)
{
    if (!L || !code || !result) return ESP_ERR_INVALID_ARG;

    /* Stop running task so we can safely access the VM */
    bool was_running = false;
    if (lua_task_handle) {
        vTaskDelete(lua_task_handle);
        lua_task_handle = NULL;
        lua_task_running = false;
        was_running = true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int ret = luaL_dostring(L, code);
    if (ret != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        snprintf(result, max_len, "error: %s", err ? err : "unknown");
        lua_pop(L, 1);
        if (was_running) lua_runtime_start();
        return ESP_FAIL;
    }

    /* Capture return value from top of stack */
    if (lua_gettop(L) > 0) {
        const char *s = luaL_tolstring(L, -1, NULL);
        snprintf(result, max_len, "%s", s ? s : "nil");
        lua_pop(L, 2);  /* pop tolstring result + original value */
    } else {
        snprintf(result, max_len, "ok");
    }

    /* Resume main.lua if it was running */
    if (was_running) lua_runtime_start();
    return ESP_OK;
}

esp_err_t lua_runtime_get_script(const char *name, char *buf, size_t max_len)
{
    return lua_runtime_get_script_chunk(name, buf, max_len, 0, 0);
}

esp_err_t lua_runtime_get_script_chunk(const char *name, char *buf, size_t max_len, size_t offset, size_t limit)
{
    if (!name || !buf) return ESP_ERR_INVALID_ARG;

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, max_len, "Script not found: %s", name);
        return ESP_ERR_NOT_FOUND;
    }

    // Skip offset bytes
    if (offset > 0) {
        fseek(f, offset, SEEK_SET);
    }

    size_t read_limit = (limit > 0 && limit < max_len - 1) ? limit : (max_len - 1);
    size_t total = 0;
    while (total < read_limit) {
        size_t n = fread(buf + total, 1, read_limit - total, f);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    fclose(f);
    return ESP_OK;
}

esp_err_t lua_runtime_push_script(const char *name, const char *content, bool append)
{
    if (!name || !content) return ESP_ERR_INVALID_ARG;

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    const char *mode = append ? "a" : "w";
    FILE *f = fopen(path, mode);
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for %s", path, mode);
        return ESP_FAIL;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fflush(f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Write failed: expected %d, wrote %d", (int)len, (int)written);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Script %s: %s (%d bytes)", name, append ? "appended" : "written", (int)len);

    /* Incremental GC to free accumulated memory from previous scripts */
    if (L) {
        lua_gc(L, LUA_GCSTEP, 10);
    }

    return ESP_OK;
}

esp_err_t lua_runtime_list_scripts(char *buf, size_t max_len)
{
    if (!buf) return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) {
        snprintf(buf, max_len, "Failed to open SPIFFS directory");
        return ESP_FAIL;
    }

    int offset = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && offset < (int)max_len - 1) {
        /* Get file size */
        char path[280];
        snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", entry->d_name);
        struct stat st;
        int size = 0;
        if (stat(path, &st) == 0) {
            size = (int)st.st_size;
        }
        offset += snprintf(buf + offset, max_len - offset,
                           "%s (%d bytes)\n", entry->d_name, size);
    }
    closedir(dir);

    if (offset == 0) {
        snprintf(buf, max_len, "(no scripts)");
    }
    return ESP_OK;
}

esp_err_t lua_runtime_get_memory_usage(uint32_t *current_bytes, uint32_t *peak_bytes)
{
    if (!current_bytes || !peak_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!L) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&lua_mem_mux);
    *current_bytes = lua_mem_current;
    *peak_bytes = lua_mem_peak;
    taskEXIT_CRITICAL(&lua_mem_mux);
    return ESP_OK;
}

void* lua_runtime_get_lua_state(void)
{
    return (void*)L;
}
