#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "st7305_panel.h"

#define TAG "ST7305"

#define LCD_W          400
#define LCD_H          300
#define LCD_FB_STRIDE  (LCD_H >> 2)                  // 75 字节
#define LCD_FB_BYTES   ((LCD_W / 4) * (LCD_H / 2))   // 15000 字节

#define CASET_START    0x12
#define CASET_END      0x2A
#define RASET_START    0x00
#define RASET_END      0xC7

// 面板固有 16Hz，合并刷新：最快 ~10Hz 即可，再快只是白白耗电
#define REFRESH_PERIOD_MS 80

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t rst_gpio;
    uint8_t *fb;        // LVGL 侧写入的帧缓存
    uint8_t *tx_buf;    // 发送快照，避免刷新期间与 LVGL 争用
    SemaphoreHandle_t lock;
    volatile bool dirty;
    volatile bool sleep_in;
    TaskHandle_t refresh_task;
} st7305_panel_t;

static esp_err_t panel_tx_param(st7305_panel_t *p, uint8_t cmd, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(p->io, cmd, data, len);
}

static esp_err_t panel_tx_cmd(st7305_panel_t *p, uint8_t cmd)
{
    return esp_lcd_panel_io_tx_param(p->io, cmd, NULL, 0);
}

static esp_err_t st7305_reset(esp_lcd_panel_t *panel)
{
    st7305_panel_t *p = (st7305_panel_t *)panel;
    if (p->rst_gpio != GPIO_NUM_NC) {
        gpio_set_level(p->rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(p->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

static esp_err_t st7305_init(esp_lcd_panel_t *panel)
{
    st7305_panel_t *p = (st7305_panel_t *)panel;

    // 序列与 Arduino 侧真机验证版本逐条一致，禁止随意增删
    uint8_t d6[] = {0x17, 0x02};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xD6, d6, sizeof(d6)), TAG, "D6");
    uint8_t d1[] = {0x01};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xD1, d1, sizeof(d1)), TAG, "D1");

    uint8_t c0[] = {0x11, 0x04};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xC0, c0, sizeof(c0)), TAG, "C0");
    uint8_t c1[] = {0x69, 0x69, 0x69, 0x69};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xC1, c1, sizeof(c1)), TAG, "C1");
    uint8_t c2[] = {0x19, 0x19, 0x19, 0x19};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xC2, c2, sizeof(c2)), TAG, "C2");
    uint8_t c4[] = {0x4B, 0x4B, 0x4B, 0x4B};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xC4, c4, sizeof(c4)), TAG, "C4");
    uint8_t c5[] = {0x19, 0x19, 0x19, 0x19};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xC5, c5, sizeof(c5)), TAG, "C5");
    uint8_t d8[] = {0x80, 0xE9};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xD8, d8, sizeof(d8)), TAG, "D8");

    uint8_t b2[] = {0x02};  // 16Hz：原厂原始值
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB2, b2, sizeof(b2)), TAG, "B2");
    uint8_t b3[] = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB3, b3, sizeof(b3)), TAG, "B3");
    uint8_t b4[] = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB4, b4, sizeof(b4)), TAG, "B4");
    uint8_t s62[] = {0x32, 0x03, 0x1F};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0x62, s62, sizeof(s62)), TAG, "62");
    uint8_t b7[] = {0x13};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB7, b7, sizeof(b7)), TAG, "B7");

    uint8_t b0[] = {0x64};  // 408 行
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB0, b0, sizeof(b0)), TAG, "B0");
    ESP_RETURN_ON_ERROR(panel_tx_cmd(p, 0x11), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t c9[] = {0x00};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xC9, c9, sizeof(c9)), TAG, "C9");
    uint8_t madctl[] = {0x48};  // 横屏
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0x36, madctl, sizeof(madctl)), TAG, "MADCTL");
    uint8_t colmod[] = {0x11};  // 1bpp 打包（每格 3 字节）
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0x3A, colmod, sizeof(colmod)), TAG, "COLMOD");
    uint8_t b9[] = {0x20};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB9, b9, sizeof(b9)), TAG, "B9");
    uint8_t b8[] = {0x29};  // 双行交错
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xB8, b8, sizeof(b8)), TAG, "B8");
    ESP_RETURN_ON_ERROR(panel_tx_cmd(p, 0x21), TAG, "INVON");  // 帧缓存位 1 = 白

    uint8_t caset[] = {CASET_START, CASET_END};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0x2A, caset, sizeof(caset)), TAG, "CASET");
    uint8_t raset[] = {RASET_START, RASET_END};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0x2B, raset, sizeof(raset)), TAG, "RASET");

    uint8_t te[] = {0x00};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0x35, te, sizeof(te)), TAG, "TEON");
    uint8_t d0[] = {0xFF};
    ESP_RETURN_ON_ERROR(panel_tx_param(p, 0xD0, d0, sizeof(d0)), TAG, "D0");
    ESP_RETURN_ON_ERROR(panel_tx_cmd(p, 0x38), TAG, "HPM");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(panel_tx_cmd(p, 0x29), TAG, "DISPON");
    vTaskDelay(pdMS_TO_TICKS(120));

    xSemaphoreTake(p->lock, portMAX_DELAY);
    memset(p->fb, 0xFF, LCD_FB_BYTES);
    xSemaphoreGive(p->lock);
    p->dirty = true;
    p->sleep_in = false;

    ESP_LOGI(TAG, "panel initialized (%dx%d, 1bpp, %d bytes fb)", LCD_W, LCD_H, LCD_FB_BYTES);
    return ESP_OK;
}

static void st7305_refresh_task(void *arg)
{
    st7305_panel_t *p = (st7305_panel_t *)arg;
    while (true) {
        if (p->dirty && !p->sleep_in) {
            p->dirty = false;
            xSemaphoreTake(p->lock, portMAX_DELAY);
            memcpy(p->tx_buf, p->fb, LCD_FB_BYTES);
            xSemaphoreGive(p->lock);

            uint8_t caset[] = {CASET_START, CASET_END};
            uint8_t raset[] = {RASET_START, RASET_END};
            panel_tx_param(p, 0x2A, caset, sizeof(caset));
            panel_tx_param(p, 0x2B, raset, sizeof(raset));
            esp_lcd_panel_io_tx_color(p->io, 0x2C, p->tx_buf, LCD_FB_BYTES);
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_PERIOD_MS));
    }
}

static esp_err_t st7305_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                    const void *color_data)
{
    st7305_panel_t *p = (st7305_panel_t *)panel;
    if (color_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x_start < 0 || y_start < 0 || x_end > LCD_W || y_end > LCD_H || x_start >= x_end || y_start >= y_end) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t *src = (const uint16_t *)color_data;
    const int width = x_end - x_start;

    xSemaphoreTake(p->lock, portMAX_DELAY);
    for (int y = y_start; y < y_end; y++) {
        const int inv_y = (LCD_H - 1) - y;
        const int row_base = inv_y >> 2;
        const int bit_row_shift = (inv_y & 3) << 1;
        const uint16_t *line = src + (size_t)(y - y_start) * width;
        for (int x = x_start; x < x_end; x++) {
            uint16_t c = line[x - x_start];
            // RGB565 -> 亮度，二值化（RLCD 无灰阶）
            int r8 = (c >> 11) & 0x1F;
            int g8 = (c >> 5) & 0x3F;
            int b8 = c & 0x1F;
            r8 = (r8 << 3) | (r8 >> 2);
            g8 = (g8 << 2) | (g8 >> 4);
            b8 = (b8 << 3) | (b8 >> 2);
            int lum = (r8 * 77 + g8 * 151 + b8 * 28) >> 8;

            const int idx = (x >> 1) * LCD_FB_STRIDE + row_base;
            const int bit = 7 - (bit_row_shift | (x & 1));
            if (lum >= 128) {
                p->fb[idx] |= (uint8_t)(1u << bit);
            } else {
                p->fb[idx] &= (uint8_t)~(1u << bit);
            }
        }
    }
    xSemaphoreGive(p->lock);

    p->dirty = true;
    return ESP_OK;
}

static esp_err_t st7305_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    // 方向由 MADCTL 0x48 定型，运行期不允许改
    (void)panel;
    (void)mirror_x;
    (void)mirror_y;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t st7305_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    (void)panel;
    (void)swap_axes;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t st7305_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    (void)panel;
    (void)x_gap;
    (void)y_gap;
    return ESP_OK;
}

static esp_err_t st7305_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    (void)panel;
    (void)invert_color_data;
    return ESP_OK;
}

static esp_err_t st7305_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7305_panel_t *p = (st7305_panel_t *)panel;
    return panel_tx_cmd(p, on_off ? 0x29 : 0x28);
}

static esp_err_t st7305_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7305_panel_t *p = (st7305_panel_t *)panel;
    p->sleep_in = sleep;
    return panel_tx_cmd(p, sleep ? 0x10 : 0x11);
}

static esp_err_t st7305_del(esp_lcd_panel_t *panel)
{
    st7305_panel_t *p = (st7305_panel_t *)panel;
    if (p->refresh_task) {
        vTaskDelete(p->refresh_task);
        p->refresh_task = NULL;
    }
    if (p->lock) {
        vSemaphoreDelete(p->lock);
    }
    free(p->fb);
    free(p->tx_buf);
    free(p);
    return ESP_OK;
}

esp_err_t st7305_new_panel(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                           esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    st7305_panel_t *p = (st7305_panel_t *)calloc(1, sizeof(st7305_panel_t));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no mem for panel");
    p->fb = (uint8_t *)malloc(LCD_FB_BYTES);
    p->tx_buf = (uint8_t *)malloc(LCD_FB_BYTES);
    ESP_RETURN_ON_FALSE(p->fb && p->tx_buf, ESP_ERR_NO_MEM, TAG, "no mem for fb");
    p->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(p->lock, ESP_ERR_NO_MEM, TAG, "no mem for lock");

    p->io = io;
    p->rst_gpio = (gpio_num_t)panel_dev_config->reset_gpio_num;
    memset(p->fb, 0xFF, LCD_FB_BYTES);

    if (p->rst_gpio != GPIO_NUM_NC) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << p->rst_gpio),
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio config failed");
    }

    p->base.reset = st7305_reset;
    p->base.init = st7305_init;
    p->base.del = st7305_del;
    p->base.draw_bitmap = st7305_draw_bitmap;
    p->base.mirror = st7305_mirror;
    p->base.swap_xy = st7305_swap_xy;
    p->base.set_gap = st7305_set_gap;
    p->base.invert_color = st7305_invert_color;
    p->base.disp_on_off = st7305_disp_on_off;
    p->base.disp_sleep = st7305_disp_sleep;

    *ret_panel = &p->base;

    xTaskCreate(st7305_refresh_task, "st7305_refresh", 4096, p, 4, &p->refresh_task);
    ESP_LOGI(TAG, "panel created, refresh task started");
    return ESP_OK;
}
