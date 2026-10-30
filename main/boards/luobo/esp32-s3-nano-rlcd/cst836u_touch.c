#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "cst836u_touch.h"

#define TAG "CST836U"

#define TP_RAW_X_MAX 400
#define TP_RAW_Y_MAX 300

static i2c_master_dev_handle_t s_tp_dev = NULL;

static esp_err_t tp_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_tp_dev, &reg, 1, buf, len, pdMS_TO_TICKS(50));
}

bool cst836u_touch_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret;

    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio config failed");
    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);

    ret = i2c_master_probe(bus, TOUCH_I2C_ADDR, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "probe 0x%02X failed: %s", TOUCH_I2C_ADDR, esp_err_to_name(ret));
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_tp_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(ret));
        s_tp_dev = NULL;
        return false;
    }

    ESP_LOGI(TAG, "touch found at 0x%02X", TOUCH_I2C_ADDR);
    return true;
}

bool cst836u_touch_read(int *x, int *y)
{
    if (s_tp_dev == NULL) {
        return false;
    }

    uint8_t d[11];
    if (tp_read_regs(0x00, d, sizeof(d)) != ESP_OK) {
        return false;
    }

    // 触点数只接受 1~2：空闲时 IC 会读回整片 0xFF，此时 d[2] & 0x0F == 0x0F
    uint8_t pts = d[2] & 0x0F;
    if (pts < 1 || pts > 2) {
        return false;
    }

    uint16_t rx = (uint16_t)(((d[3] & 0x0F) << 8) | d[4]);
    uint16_t ry = (uint16_t)(((d[5] & 0x0F) << 8) | d[6]);
    if (rx > TP_RAW_X_MAX || ry > TP_RAW_Y_MAX) {
        return false;  // 越界读数一律丢弃
    }

    int px = (int)((uint32_t)rx * (DISPLAY_WIDTH - 1) / TP_RAW_X_MAX);
    int py = (int)((uint32_t)ry * (DISPLAY_HEIGHT - 1) / TP_RAW_Y_MAX);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > DISPLAY_WIDTH - 1) px = DISPLAY_WIDTH - 1;
    if (py > DISPLAY_HEIGHT - 1) py = DISPLAY_HEIGHT - 1;

    *x = px;
    *y = py;
    return true;
}
