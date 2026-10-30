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

### 音频板（2.54mm 10Pin 排针，丝印 `GND VCC EN SDA SCL MCK BCK DO WS DI`）

引脚顺序以板上丝印为准，**最左侧 GND 为 1 脚**。原理图（`7210-SC..V1.pdf`）的网络名与丝印对应关系：
`PA_EN → EN`、`ES_I2C_SDA → SDA`、`ES_I2C_SCL → SCL`、`I2S_MCK → MCK`、`I2S_BCK → BCK`、`I2S_DO → DO`、`I2S_WS → WS`、`I2S_DI → DI`。

| 序号 | 板上丝印 | Nano | GPIO | 说明 |
| :---: | :--- | :---: | :---: | :--- |
| 1 | GND | GND | - | 最左，1 脚标记 |
| 2 | VCC | **5V** | - | 板载 XC6206 需压差；NS4150B 直取 VCC。**不可接 3V3** |
| 3 | EN | A7 | 14 | 功放使能（原理图 PA_EN），经 R10 10k 到 NS4150B CTRL |
| 4 | SDA | A4 | 11 | 与触摸 SDA **并联在同一节点** |
| 5 | SCL | A5 | 12 | 与触摸 SCL **并联在同一节点** |
| 6 | MCK | A3 | 4 | ESP32 主出，两片 codec 共用 |
| 7 | BCK | D5 | 8 | |
| 8 | DO | A6 | 13 | ESP32 → ES8311 DSDIN |
| 9 | WS | D6 | 9 | |
| 10 | DI | D7 | 10 | ES7210 SDOUT1/TDMOUT → ESP32 |

### 喇叭与麦克风

- **喇叭接口是 1.25mm 2P 座**（PH1.25 带线端子），不是 2.54mm。原理图按 4 脚画
  （OUTP/OUTN/GND/GND），实物只装 2P 座、引出差分两线，第 3/4 脚为 GND 焊盘。
  单声道，4Ω 3W 或 8Ω 均可；习惯上红线接 OUTP、黑线接 OUTN。
- 麦克风为**板载** MIC1 / MIC2 小板（由 ES7210 的 MICBIAS 供电），无需外接。

### 接线验证顺序

1. 只接 `GND / VCC / SDA / SCL` 四根，上电后 I2C 扫描应同时出现
   `0x15`（CST836U）、`0x18`（ES8311）、`0x41`（ES7210）。
2. 三个地址齐全后再补 `EN / MCK / BCK / DO / WS / DI` 六根，串口日志应出现
   `BoxAudioCodec: Duplex channels created`。
3. 最后插喇叭。NS4150B 满功率峰值近 1A，USB 供电不足时会掉压复位，
   此时给音频板单独供 5V 并与 Nano 共地。

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
5. **显示沿用微雪方案**：`custom_lcd_display.cc` 直接取自
   `boards/waveshare/esp32-s3-rlcd-4.2`（同为 ST7305 400×300 RLCD，已实机验证）。
   ST7305 显存是「每列 3 字节 = 2×4 像素块」的 1bpp 排布，整帧固定 15000 字节；
   构造时 `panel_io`/`panel` 传 NULL，自行 `lv_display_create` + flush 回调，
   用 400×300 的 index/bit 查找表把 RGB565 像素直写帧缓存，再整帧 SPI 刷出。
   不实现 `esp_lcd_panel_t`，避免与 IDF 版本相关的私有结构耦合。
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
