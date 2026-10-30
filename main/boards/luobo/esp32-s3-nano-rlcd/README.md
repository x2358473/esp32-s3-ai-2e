# ESP32-S3-Nano RLCD（ST7305 + ES7210/ES8311 AEC）

自制桌面面板版：ESP32-S3-Nano（N16R8）+ 4.2 寸 ST7305 反射式 1bpp 屏（400×300）
+ CST836U 单点触摸 + SurferAI ES7210/ES8311/NS4150B AEC 音频板。

## 硬件接线

### 显示屏（SPI，ST7305）

| 信号 | Nano 引脚 | GPIO |
| :--- | :---: | :---: |
| RST | D8 | 17 |
| SCK | D13 | 48 |
| MOSI | D11 | 38 |
| DC | D9 | 18 |
| CS | D10 | 21 |

### 触摸（I2C 0x15，CST836U）

| 信号 | Nano 引脚 | GPIO |
| :--- | :---: | :---: |
| TP_RST | D3 | 6 |
| TP_INT | D4 | 7 |
| TP_SDA | A4 | 11 |
| TP_SCL | A5 | 12 |

### 音频板（H1 排针 PZ254V-11-10P，原理图 `7210-SC..V1.pdf`）

| H1 | 板侧信号 | Nano | GPIO | 说明 |
| :---: | :--- | :---: | :---: | :--- |
| 1 | GND | GND | - | |
| 2 | VCC | 5V | - | 板载 XC6206 需压差；NS4150B 直取 VCC |
| 3 | PA_EN | A7 | 14 | 经 R10 10k 到 NS4150B CTRL |
| 4 | ES_I2C_SDA | A4 | 11 | 与触摸共用总线 |
| 5 | ES_I2C_SCL | A5 | 12 | 与触摸共用总线 |
| 6 | I2S_MCK | A3 | 4 | ESP32 主出，两片 codec 共用 |
| 7 | I2S_BCK | D5 | 8 | |
| 8 | I2S_DO | A6 | 13 | ESP32 → ES8311 DSDIN |
| 9 | I2S_WS | D6 | 9 | |
| 10 | I2S_DI | D7 | 10 | ES7210 SDOUT1/TDMOUT → ESP32 |

## 适配要点

1. **接官方服务端，24k**：本板卡 `config.json` 不覆盖 `CONFIG_OTA_URL`，走官方默认
   `https://api.tenclass.net/xiaozhi/ota/`；采样率 24000/24000。若改用自部署
   xiaozhi-server（仅 16k），需把 `config.h` 两个采样率改成 16000 并补 OTA 地址。
2. **ES7210 地址 0x41**：板上 AD0 经 R3(10k) 上拉到 AU_3V3、AD1 经 R4(10k) 下拉，
   不是驱动默认的 `0x40`。
3. **AEC 参考通道**：ES8311 的 `OUTP/OUTN` 经 R5/R7(0R) 硬件回接到 ES7210 的
   `ADC_MIC3P/MIC3N`，因此开启 `AUDIO_INPUT_REFERENCE`。音频链路复用
   `BoxAudioCodec`（ES7210 四通道 TDM + ES8311 播放），与 ESP-BOX-3 同构。
4. **共用 I2C**：触摸与两片 codec 都在 SDA 11 / SCL 12 上，只创建一次
   `i2c_master_bus`。板上已有 BSS138 电平转换与 10k/2.2k 上拉。
5. **显示不是 TFT**：ST7305 显存是「每列 3 字节 = 2×4 像素块」的 1bpp 排布，
   整帧固定 15000 字节。`st7305_panel.c` 把 LVGL 的 RGB565 区域二值化后按定型公式
   写入帧缓存，并由后台任务限速整帧刷新（面板固有 16Hz，限到 ~10Hz）。
6. **无背光**：不实现 `GetBacklight()`（基类默认返回 nullptr）。

## 构建

```bash
python scripts/build.py luobo/esp32-s3-nano-rlcd --name esp32-s3-nano-rlcd
```

或在仓库 Actions 里手动触发 `Build Boards`，`board` 选 `luobo/esp32-s3-nano-rlcd`。

## 待实机验证

- ST7305 未经 esp_lcd 通路验证：重点看 `esp_lcd_panel_io_spi` 分包发送时 CS 是否
  中途拉高（Arduino 侧要求 CS 全程低），必要时把 `trans_queue_depth` 调小。
- `BoxAudioCodec` 内部把 `i2c_cfg.port` 写死为 `1`，但实际使用外部 `bus_handle`；
  若 codec 探测不到，先确认 esp_codec_dev 没有另起一条 legacy I2C。
- NS4150B 由 VCC(5V) 供电，满功率峰值接近 1A，USB 供电能力不足时会掉压复位。
