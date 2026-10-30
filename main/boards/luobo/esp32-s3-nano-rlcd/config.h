#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/*
 * ESP32-S3-Nano (N16R8) + ST7305 400x300 1bpp 反射屏 + CST836U 单点触摸
 *                                 + ES7210(ADC) / ES8311(DAC) / NS4150B(功放) AEC 音频板
 *
 * 引脚来源：本机实测（屏/触摸）与 7210-SC..V1.pdf 原理图 H1 排针（音频）。
 * 已占用且不可挪动：17/18/21/38/48（屏）、6/7/11/12（触摸）。
 * 不可用：19/20（USB D±）、43/44（USB CDC/JTAG）、33~37（N16R8 Octal PSRAM）。
 *
 * 本板卡接官方服务端（24k）。若改用自部署 xiaozhi-server（仅 16k），
 * 需把下面两个采样率改成 16000，并在 config.json 里补 CONFIG_OTA_URL。
 */

// ---------------- 音频（H1 排针，双工单总线） ----------------
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_4   // H1-6  I2S_MCK，需由 ESP32 主出（ES7210/ES8311 共用）
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_8   // H1-7  I2S_BCK
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_9   // H1-9  I2S_WS
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10  // H1-10 I2S_DI，ES7210 SDOUT1/TDMOUT -> ESP32
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13  // H1-8  I2S_DO，ESP32 -> ES8311 DSDIN

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_14  // H1-3 PA_EN -> NS4150B CTRL
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_11  // H1-4 ES_I2C_SDA，与触摸共用
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_12  // H1-5 ES_I2C_SCL，与触摸共用

#define AUDIO_CODEC_ES8311_ADDR  0x18  // ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  0x41  // AD0 经 10k 上拉到 AU_3V3，非默认 0x40

// ES8311 的 OUTP/OUTN 经 R5/R7(0R) 硬件回接到 ES7210 的 MIC3 输入，构成 AEC 参考通道。
#define AUDIO_INPUT_REFERENCE    true

// ---------------- 显示（ST7305 400x300 1bpp，无背光无灰阶） ----------------
#define DISPLAY_WIDTH   400
#define DISPLAY_HEIGHT  300

#define DISPLAY_SPI_SCK_PIN  GPIO_NUM_48
#define DISPLAY_SPI_MOSI_PIN GPIO_NUM_38
#define DISPLAY_DC_PIN       GPIO_NUM_18
#define DISPLAY_SPI_CS_PIN   GPIO_NUM_21
#define DISPLAY_RST_PIN      GPIO_NUM_17

// MADCTL 0x48 已在驱动内部定型，这里全部交给驱动，勿再镜像/交换
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_BACKLIGHT_PIN         GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ---------------- 触摸（CST836U，I2C 0x15） ----------------
#define TOUCH_I2C_ADDR  0x15
#define TOUCH_RST_PIN   GPIO_NUM_6
#define TOUCH_INT_PIN   GPIO_NUM_7

// ---------------- 按键 ----------------
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#endif  // _BOARD_CONFIG_H_
