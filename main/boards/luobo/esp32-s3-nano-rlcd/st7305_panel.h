#ifndef _ST7305_PANEL_H_
#define _ST7305_PANEL_H_

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ST7305 400x300 1bpp 反射屏的 esp_lcd panel 驱动。
 *
 * 与常规 TFT 驱动不同：显存不是逐像素线性排布，而是「每列 3 字节 = 2x4 像素块」，
 * 整帧固定 15000 字节（25 列 x 200 行 x 3 字节）。draw_bitmap 收到的是 LVGL 的
 * RGB565 区域，内部二值化后按定型公式写入本地帧缓存，再由后台任务限速整帧刷新
 * （RLCD 面板固有 16Hz，频繁刷新只耗电不提升观感）。
 */
esp_err_t st7305_new_panel(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                           esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif

#endif  // _ST7305_PANEL_H_
