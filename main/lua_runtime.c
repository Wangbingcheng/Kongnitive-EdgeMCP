/*
 * Lua Runtime for ESP32
 *
 * SPIFFS-backed script storage + Lua 5.4 VM + C bindings for hardware.
 */

#include "lua_runtime.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "lua_rt";

#define SPIFFS_BASE_PATH "/spiffs"
#define LUA_TASK_STACK   8192
#define LUA_TASK_PRIO    5

static lua_State *L = NULL;
static TaskHandle_t lua_task_handle = NULL;
static volatile bool lua_task_running = false;
static volatile uint32_t lua_mem_current = 0;
static volatile uint32_t lua_mem_peak = 0;

static void lua_mem_update(size_t old_size, size_t new_size)
{
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

/* ── SPIFFS helpers ─────────────────────────────────────────────── */

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = "storage",
        .max_files = 6,
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

static int l_system_uptime(lua_State *L)
{
    lua_pushnumber(L, (double)esp_timer_get_time() / 1000000.0);
    return 1;
}

static const luaL_Reg system_lib[] = {
    {"heap_free", l_system_heap_free},
    {"uptime",    l_system_uptime},
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
    int sda = luaL_checkinteger(L, 1);
    int scl = luaL_checkinteger(L, 2);
    int freq = luaL_optinteger(L, 3, 400000);

    /* Clean up existing bus */
    if (i2c_bus_handle) {
        for (int i = 0; i < i2c_dev_count; i++) {
            i2c_master_bus_rm_device(i2c_dev_cache[i].handle);
        }
        i2c_dev_count = 0;
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
    }

    i2c_bus_freq = freq;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus_handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.setup failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_i2c_write(lua_State *L)
{
    int addr = luaL_checkinteger(L, 1);
    int nargs = lua_gettop(L);

    uint8_t buf[I2C_WRITE_BUF_SZ];
    int len = 0;

    for (int i = 2; i <= nargs && len < I2C_WRITE_BUF_SZ; i++) {
        if (lua_isinteger(L, i)) {
            buf[len++] = (uint8_t)lua_tointeger(L, i);
        } else if (lua_isstring(L, i)) {
            size_t slen;
            const char *s = lua_tolstring(L, i, &slen);
            for (size_t j = 0; j < slen && len < I2C_WRITE_BUF_SZ; j++) {
                buf[len++] = (uint8_t)s[j];
            }
        } else if (lua_istable(L, i)) {
            int tlen = luaL_len(L, i);
            for (int j = 1; j <= tlen && len < I2C_WRITE_BUF_SZ; j++) {
                lua_rawgeti(L, i, j);
                buf[len++] = (uint8_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
    }

    if (len == 0) return 0;

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    esp_err_t ret = i2c_master_transmit(dev, buf, len, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.write failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_i2c_read(lua_State *L)
{
    int addr = luaL_checkinteger(L, 1);
    int rlen = luaL_checkinteger(L, 2);
    if (rlen > I2C_READ_BUF_SZ) rlen = I2C_READ_BUF_SZ;

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    uint8_t buf[I2C_READ_BUF_SZ];
    esp_err_t ret = i2c_master_receive(dev, buf, rlen, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.read failed: %s", esp_err_to_name(ret));
    }

    lua_createtable(L, rlen, 0);
    for (int i = 0; i < rlen; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_i2c_write_read(lua_State *L)
{
    int addr = luaL_checkinteger(L, 1);

    uint8_t wbuf[I2C_WRITE_BUF_SZ];
    int wlen = 0;

    if (lua_istable(L, 2)) {
        int tlen = luaL_len(L, 2);
        for (int j = 1; j <= tlen && wlen < I2C_WRITE_BUF_SZ; j++) {
            lua_rawgeti(L, 2, j);
            wbuf[wlen++] = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    } else if (lua_isinteger(L, 2)) {
        wbuf[wlen++] = (uint8_t)lua_tointeger(L, 2);
    }

    int rlen = luaL_checkinteger(L, 3);
    if (rlen > I2C_READ_BUF_SZ) rlen = I2C_READ_BUF_SZ;

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    uint8_t rbuf[I2C_READ_BUF_SZ];
    esp_err_t ret = i2c_master_transmit_receive(dev, wbuf, wlen, rbuf, rlen, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.write_read failed: %s", esp_err_to_name(ret));
    }

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
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = i2c_bus_freq,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(i2c_bus_handle, &cfg, &dev) == ESP_OK) {
            esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, I2C_SCAN_TIMEOUT_MS);
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

static spi_device_handle_t spi_handle = NULL;
static int spi_dc_pin = -1;
static int spi_res_pin = -1;

static int l_spi_setup(lua_State *L)
{
    int mosi = luaL_checkinteger(L, 1);
    int clk = luaL_checkinteger(L, 2);
    int cs = luaL_optinteger(L, 3, -1);
    int dc = luaL_optinteger(L, 4, -1);
    int res = luaL_optinteger(L, 5, -1);
    int freq = luaL_optinteger(L, 6, 1000000);

    if (spi_handle) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }

    spi_dc_pin = dc;
    spi_res_pin = res;

    if (dc >= 0) {
        gpio_set_direction(dc, GPIO_MODE_OUTPUT);
        gpio_set_level(dc, 0);  /* DC=0 for command mode initially */
        ESP_LOGI(TAG, "SPI DC pin=%d initialized", dc);
    }
    if (res >= 0) {
        gpio_set_direction(res, GPIO_MODE_OUTPUT);
        gpio_set_level(res, 1);
        gpio_set_level(res, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(res, 1);
        ESP_LOGI(TAG, "SPI RES pin=%d initialized", res);
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = -1,
        .sclk_io_num = clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_WRITE_BUF_SZ,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.setup bus failed: %s", esp_err_to_name(ret));
    }

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

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.setup add device failed: %s", esp_err_to_name(ret));
    }

    return 0;
}

static int l_spi_transfer(lua_State *L)
{
    if (!spi_handle) {
        return luaL_error(L, "spi not initialized");
    }

    int nargs = lua_gettop(L);
    uint8_t tx_buf[SPI_WRITE_BUF_SZ];
    int tx_len = 0;

    for (int i = 1; i <= nargs && tx_len < SPI_WRITE_BUF_SZ; i++) {
        if (lua_isinteger(L, i)) {
            tx_buf[tx_len++] = (uint8_t)lua_tointeger(L, i);
        } else if (lua_isstring(L, i)) {
            size_t slen;
            const char *s = lua_tolstring(L, i, &slen);
            for (size_t j = 0; j < slen && tx_len < SPI_WRITE_BUF_SZ; j++) {
                tx_buf[tx_len++] = (uint8_t)s[j];
            }
        } else if (lua_istable(L, i)) {
            int tlen = luaL_len(L, i);
            for (int j = 1; j <= tlen && tx_len < SPI_WRITE_BUF_SZ; j++) {
                lua_rawgeti(L, i, j);
                tx_buf[tx_len++] = (uint8_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
    }

    int rx_len = tx_len;
    uint8_t rx_buf[SPI_READ_BUF_SZ];

    spi_transaction_t t = {
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
        .length = tx_len * 8,
        .rxlength = rx_len * 8,
    };

    esp_err_t ret = spi_device_transmit(spi_handle, &t);
    if (ret != ESP_OK) {
        return luaL_error(L, "spi.transfer failed: %s", esp_err_to_name(ret));
    }

    lua_createtable(L, rx_len, 0);
    for (int i = 0; i < rx_len; i++) {
        lua_pushinteger(L, rx_buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_spi_write(lua_State *L)
{
    if (!spi_handle) {
        return luaL_error(L, "spi not initialized");
    }

    int dc_value = luaL_optinteger(L, 1, 1);
    int nargs = lua_gettop(L);

    if (spi_dc_pin >= 0) {
        gpio_set_level(spi_dc_pin, dc_value);
        ESP_LOGD(TAG, "spi.write: dc_pin=%d value=%d", spi_dc_pin, dc_value);
    }

    uint8_t tx_buf[SPI_WRITE_BUF_SZ];
    int tx_len = 0;

    for (int i = 2; i <= nargs && tx_len < SPI_WRITE_BUF_SZ; i++) {
        if (lua_isinteger(L, i)) {
            tx_buf[tx_len++] = (uint8_t)lua_tointeger(L, i);
        } else if (lua_isstring(L, i)) {
            size_t slen;
            const char *s = lua_tolstring(L, i, &slen);
            for (size_t j = 0; j < slen && tx_len < SPI_WRITE_BUF_SZ; j++) {
                tx_buf[tx_len++] = (uint8_t)s[j];
            }
        } else if (lua_istable(L, i)) {
            int tlen = luaL_len(L, i);
            for (int j = 1; j <= tlen && tx_len < SPI_WRITE_BUF_SZ; j++) {
                lua_rawgeti(L, i, j);
                tx_buf[tx_len++] = (uint8_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
    }

    spi_transaction_t t = {
        .tx_buffer = tx_buf,
        .length = tx_len * 8,
    };

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

static int lcd_dc_pin = -1;
static int lcd_cs_pin = -1;
static int lcd_res_pin = -1;
static int lcd_bl_pin = -1;
static uint16_t *lcd_framebuf = NULL;

/* 8x8 font (95 characters: 0x20-0x7E) */
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
}

static void fb_draw_char(int x, int y, char c, uint16_t fg, int has_bg, uint16_t bg, int scale)
{
    int idx = c - ' ';
    if (idx < 0 || idx >= 95) return;
    
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
}

static void lcd_send_cmd(uint8_t cmd)
{
    if (lcd_cs_pin >= 0) gpio_set_level(lcd_cs_pin, 0);
    gpio_set_level(lcd_dc_pin, 0);
    spi_transaction_t t = {
        .tx_buffer = &cmd,
        .length = 8,
    };
    esp_err_t ret = spi_device_transmit(spi_handle, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd_send_cmd failed: %d", ret);
    }
}

static void lcd_send_data(uint8_t data)
{
    if (lcd_cs_pin >= 0) gpio_set_level(lcd_cs_pin, 0);
    gpio_set_level(lcd_dc_pin, 1);
    spi_transaction_t t = {
        .tx_buffer = &data,
        .length = 8,
    };
    esp_err_t ret = spi_device_transmit(spi_handle, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd_send_data failed: %d", ret);
    }
    if (lcd_cs_pin >= 0) gpio_set_level(lcd_cs_pin, 1);
}

static void lcd_send_buffer(const uint8_t *data, int len)
{
    if (lcd_cs_pin >= 0) gpio_set_level(lcd_cs_pin, 0);
    gpio_set_level(lcd_dc_pin, 1);
    spi_transaction_t t = {
        .tx_buffer = data,
        .length = len * 8,
    };
    spi_device_transmit(spi_handle, &t);
    if (lcd_cs_pin >= 0) gpio_set_level(lcd_cs_pin, 1);
}

static void lcd_set_addr(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    uint8_t x_offset = 0;
    uint8_t y_offset = 24;
    uint8_t data[4];

    lcd_send_cmd(0x2A);
    data[0] = 0; data[1] = x + x_offset;
    data[2] = 0; data[3] = x + w - 1 + x_offset;
    for (int i = 0; i < 4; i++) lcd_send_data(data[i]);

    lcd_send_cmd(0x2B);
    data[0] = 0; data[1] = y + y_offset;
    data[2] = 0; data[3] = y + h - 1 + y_offset;
    for (int i = 0; i < 4; i++) lcd_send_data(data[i]);

    lcd_send_cmd(0x2C);
}

static int l_lcd_setup(lua_State *L)
{
    int mosi = luaL_checkinteger(L, 1);
    int clk = luaL_checkinteger(L, 2);
    int cs = luaL_optinteger(L, 3, -1);
    int dc = luaL_optinteger(L, 4, 10);
    int res = luaL_optinteger(L, 5, 6);
    int bl = luaL_optinteger(L, 6, 11);
    int freq = luaL_optinteger(L, 7, 20000000);
    (void)freq;

    lcd_cs_pin = cs;
    lcd_dc_pin = dc;
    lcd_res_pin = res;
    lcd_bl_pin = bl;

    ESP_LOGI(TAG, "ST7735: mosi=%d clk=%d cs=%d dc=%d res=%d bl=%d", mosi, clk, cs, dc, res, bl);

    if (spi_handle) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }

    if (dc >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << dc),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(dc, 0);
    }
    if (res >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << res),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }
    if (bl >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << bl),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = -1,
        .sclk_io_num = clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32768,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return luaL_error(L, "lcd.setup bus failed: %s", esp_err_to_name(ret));
    }

    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = 20000000,
        .input_delay_ns = 0,
        .spics_io_num = cs,
        .flags = 0,
        .queue_size = 1,
    };

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "lcd.setup add device failed: %s", esp_err_to_name(ret));
    }

    gpio_set_level(res, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(res, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "ST7735: RESET done");

    lcd_send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "ST7735: SWRESET done");

    lcd_send_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "ST7735: SLPOUT done");

    lcd_send_cmd(0x3A);
    lcd_send_data(0x05);
    ESP_LOGI(TAG, "ST7735: COLMOD done");

    lcd_send_cmd(0x36);
    lcd_send_data(0x68);
    ESP_LOGI(TAG, "ST7735: MADCTL done");

    lcd_send_cmd(0x13);
    ESP_LOGI(TAG, "ST7735: NORON done");

    lcd_send_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "ST7735: DISPON done");

    vTaskDelay(pdMS_TO_TICKS(100));

    if (lcd_bl_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << lcd_bl_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(lcd_bl_pin, 1);
        ESP_LOGI(TAG, "ST7735: Backlight ON");
    }

    if (lcd_framebuf == NULL) {
        lcd_framebuf = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_DMA);
        if (lcd_framebuf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer");
            return luaL_error(L, "lcd.setup: frame buffer allocation failed");
        }
        memset(lcd_framebuf, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    }
    
    ESP_LOGI(TAG, "ST7735 LCD initialized (frame buffer: %d bytes)", LCD_WIDTH * LCD_HEIGHT * 2);
    return 0;
}

static int l_lcd_clear(lua_State *L)
{
    int color = luaL_optinteger(L, 1, 0);
    if (!lcd_framebuf) return 0;
    
    uint16_t swapped = swap_bytes(color);
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        lcd_framebuf[i] = swapped;
    }
    return 0;
}

static int l_lcd_fill(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    
    fb_fill_rect(x, y, w, h, color);
    return 0;
}

static int l_lcd_pixel(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int color = luaL_checkinteger(L, 3);
    
    fb_set_pixel(x, y, color);
    return 0;
}

static int l_lcd_print(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    int fg_color = luaL_optinteger(L, 4, 0xFFFF);
    int has_bg = !lua_isnoneornil(L, 5);
    int bg_color = has_bg ? luaL_checkinteger(L, 5) : 0;
    int scale = luaL_optinteger(L, 6, 1);
    
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    
    int cursor_x = x;
    while (*str) {
        fb_draw_char(cursor_x, y, *str, fg_color, has_bg, bg_color, scale);
        cursor_x += 8 * scale;
        str++;
    }
    return 0;
}

static int l_lcd_flush(lua_State *L)
{
    (void)L;
    if (!lcd_framebuf) return 0;
    
    lcd_set_addr(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_send_buffer((uint8_t*)lcd_framebuf, LCD_WIDTH * LCD_HEIGHT * 2);
    if (lcd_cs_pin >= 0) gpio_set_level(lcd_cs_pin, 1);
    return 0;
}

static const luaL_Reg lcd_lib[] = {
    {"setup",     l_lcd_setup},
    {"clear",     l_lcd_clear},
    {"fill",      l_lcd_fill},
    {"pixel",     l_lcd_pixel},
    {"print",     l_lcd_print},
    {"flush",     l_lcd_flush},
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

/* ── Lua task (runs main.lua) ───────────────────────────────────── */

static void lua_task(void *pvParameters)
{
    lua_task_running = true;
    ESP_LOGI(TAG, "Lua task started, executing main.lua");

    int ret = luaL_dofile(L, SPIFFS_BASE_PATH "/main.lua");
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
    if (!name || !buf) return ESP_ERR_INVALID_ARG;

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, max_len, "Script not found: %s", name);
        return ESP_ERR_NOT_FOUND;
    }

    size_t total = 0;
    while (total < max_len - 1) {
        size_t n = fread(buf + total, 1, max_len - 1 - total, f);
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

    FILE *f = fopen(path, append ? "a" : "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    fputs(content, f);
    fclose(f);
    ESP_LOGI(TAG, "Script %s: %s (%d bytes)", append ? "appended" : "written",
             name, (int)strlen(content));
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

    *current_bytes = lua_mem_current;
    *peak_bytes = lua_mem_peak;
    return ESP_OK;
}
