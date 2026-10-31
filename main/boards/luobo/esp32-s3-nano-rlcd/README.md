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

## 设备端 AEC 与 I2C 地址约定

- 实时打断依赖 `CONFIG_USE_DEVICE_AEC=y`，而该选项在 `main/Kconfig.projbuild` 里带板型白名单，
  本板已登记 `BOARD_TYPE_LUOBO_ESP32_S3_NANO_RLCD`。若白名单丢失，构建会告警
  `USE_DEVICE_AEC has user-set value y ... but is not visible`，随后静默退回 `kAecOff`，
  表现就是"能对话但无法实时打断"，排查时先看 `sdkconfig` 里有没有 `CONFIG_USE_DEVICE_AEC=y`。
- `AUDIO_CODEC_ES8311_ADDR` / `AUDIO_CODEC_ES7210_ADDR` 必须写 8 位形式
  （`ES8311_CODEC_DEFAULT_ADDR` = 0x30，ES7210 = `0x82` = `0x41<<1`）：`esp_codec_dev`
  内部会执行 `addr >> 1`，若写成 7 位地址，实际会访问不存在的从机，读写 100% 失败，
  但日志打印的仍是移位前的原值，容易误判成硬件问题。
- 两颗 codec 在冷启动（I2S/MCLK 未运行）阶段速率偏好相反：ES8311 只在 400kHz 通
  （10k/50k/100k 实测读 0/10、写 0/5，BSS138 电平转换与低速时序冲突），ES7210 只在
  100kHz 通。故探测阶段必须各用各的速率（ES7210 失败再回退 400kHz），
  `BoxAudioCodec` 初始化时 MCLK 已运行，统一用 400kHz。

## 底噪与唤醒优化记录（2026-10-31）

本板自查"唤醒经常失败、唤醒后对话正常"的过程与结论，供后续换版或回退对照。

### 现象与根因

- 待机时 MIC1 静默电平长期在 **-41dBFS** 附近，1 秒内可波动 20dB；30cm 说话约 -42dBFS，
  唤醒信噪比仅约 5dB，WakeNet 频繁漏检，但唤醒后走云端 ASR 仍能正常识别。
- 根因不在麦克风：36dB PGA 下折算到 ADC 输入约 -77.5dBFS，比模拟 MIC 自噪高 15~20dB，
  属于模拟前端被电源/地耦合污染。
- 逐项排除后确认主因是 **NS4150B 功放常开**：`AudioService::CheckAndUpdateAudioPowerState()`
  的关功放分支被 `duplex && input_enabled` 条件挡住（要保持 I2S TX 时钟给 RX），
  功放静态噪声持续经电源/地耦合回 MIC。

### 已落地的三项改动

| 改动 | 位置 | 效果 |
| :--- | :--- | :--- |
| 待机 15 秒后单独拉低 PA，不触碰 I2S TX | `audio_service.cc`、`box_audio_codec.cc` | 静默底噪 **-41 → -58dBFS**，降低约 16dB |
| WakeNet 判定模式改为 `DET_MODE_95` | `afe_audio_engine.cc` | 提高召回，实测未出现误唤醒 |
| 待机（只剩唤醒词检测）关闭 AEC | `afe_audio_engine.cc` | 底噪再降 2~3dB 且更平稳；此时参考通道是数字静音，AEC 无实际作用 |

### 实测数据（1 米，安静环境）

- 待机底噪：-56 ~ -62dBFS
- 说话时 ch0：-37 ~ -43dBFS，唤醒信噪比约 20dB
- 唤醒成功率：约 75~80%（12~13 次尝试中成功 10 次）
- 未触发的案例中最高电平达 **-36.4dBFS**，比多数成功案例更响，
  说明剩余漏检与电平无关，属于 WakeNet 模型对真人发音的泛化问题。

### 已知局限与后续选项（未实施）

- 当前模型 `wn9_nihaoxiaozhi_tts` 由 TTS 合成语料训练，对真人口音/语速泛化有限。
  可换 `wn9l_nihaoxiaozhi_tts3`（大模型，资源约 1.3 倍）验证；需在 `config.json` 的
  `sdkconfig_append` 里显式把旧模型置 `n`，否则 build 脚本会把两个模型都打进
  `srmodels.bin`，AFE 只取第一个，等于没换。
- 硬件侧仍有优化空间（板上为 0402/0603，不便手工改）：AEC 参考取自 ES8311 输出而非
  功放后级、参考通道无衰减网络、音频地与主控仅靠排针单点相连。
- 待机 15 秒关功放实现在 `AudioCodec` 抽象层，所有使用 `BoxAudioCodec` 的板卡同样生效。

### 临时诊断日志（定位完成后删除）

- `MicLevel`：`audio_service.cc` 的 `ReadAudioData`，每秒打印 AEC 前 ch0/ch1 电平
- `afe controls`：`afe_audio_engine.cc` 的 `ApplyAfeControls`，打印 WakeNet/AEC 开关状态
- `Dropped N audio packets outside speaking state`（`application.cc`）与
  `Decode queue full, dropped N`（`audio_service.cc`）：下行音频丢包统计
- 注：`speaking -> listening` 时固定出现一次的 `Dropped 1 audio packets (state: 5)` 是
  服务端 stop 之后的尾包，属正常现象。
