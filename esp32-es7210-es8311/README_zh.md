# ESP32 ES7210+ES8311+NS4150B 本地音频示例

（中文 | [English](README.md)）

本目录 `esp32-es7210-es8311/` 是一个面向本地音频验证的精简固件示例，不包含云端语音对话链路，重点用于 ES7210 + ES8311 + NS4150B 音频模块驱动与交互流程演示。

## 核心功能

1. **支持 ESP32-S3 / ESP32-P4 开发板驱动 ES7210 + ES8311 + NS4150B**  
   固件启动后完成 I2C/Codec/音频服务初始化，并直接播放本地嵌入 PCM 文件（`canon.pcm`）。

2. **支持设备端 AEC 场景下的实时语音打断（可调参数）**  
   当启用 `CONFIG_USE_DEVICE_AEC` 后，播放过程支持用户语音实时打断（barge-in）示例；相关阈值参数在 `main/example_es7210_es8311.c` 中可按需调整。

3. **支持板载 Boot 按键录音测试流程**  
   播放中按下 Boot 键可进入录音测试；再次按下 Boot 键结束录音并回放录音内容。  
   录音数据仅驻留内存，不落盘保存。

## 当前目录结构（与代码一致）

```text
esp32-es7210-es8311/
├─ main/
│  ├─ audio_application.h/.c         # 应用状态管理与运行入口 API（Run）
│  ├─ example_es7210_es8311.c        # app_main + 板级初始化 + 按键 + PCM自播 + 语音打断
│  ├─ config.h                       # 引脚、采样率、板级配置
│  ├─ Kconfig.projbuild              # 降噪/AEC/调试配置菜单
│  └─ audio/
│     ├─ audio_service.h/.c          # 本地音频服务（PCM直通队列）
│     ├─ audio_codec.h/.c            # AudioCodec 抽象基类
│     ├─ audio_processor.h           # 音频处理器接口
│     ├─ processors/                 # AFE 与 no-op 处理器实现
│     └─ codecs/                     # ES7210/ES8311 等 codec 驱动
├─ partitions/partitions.csv         # 单应用分区表（兼容 4MB）
├─ sdkconfig.defaults                # 通用默认配置
├─ sdkconfig.defaults.esp32s3        # ESP32-S3 默认配置
└─ sdkconfig.defaults.esp32p4        # ESP32-P4 默认配置
```

## 编译环境

- ESP-IDF `v5.4+`（当前工程按 `v5.5` 使用）
- 目标芯片：`esp32s3` 或 `esp32p4`
- 默认启用 `CONFIG_USE_AUDIO_PROCESSOR` 时，需要满足 Kconfig 中的 PSRAM 依赖

## 编译与烧录

```bash
cd esp32-es7210-es8311
idf.py set-target esp32s3   # 或 esp32p4
idf.py fullclean
idf.py build
idf.py flash monitor
```

## 运行逻辑说明

- `app_main` 位于 `main/example_es7210_es8311.c`
- `app_main -> Application_Run()` 后，会完成：
  - 板级 I2C/按钮初始化
  - `AudioCodec` 实例初始化（单例指针）
  - `AudioService` 启动（含 AFE/VAD 链路）
  - 本地 PCM 自播任务启动（若 `canon.pcm` 已嵌入）

## AEC / 打断参数调整

可在 `main/example_es7210_es8311.c` 调整：

- `kLocalBargeInVadThresholdUs`：触发“语音打断”前，VAD 连续判定为语音的最短持续时间（单位：微秒）。
- `kLocalBargeInStartupGuardUs`：播放启动后的保护期（单位：微秒），保护期内不触发打断，避免刚开播被误打断。
- `kLocalBargeInArmSilenceUs`：进入“可被打断状态”前要求的静音持续时间（单位：微秒）；静音越短越容易进入可打断状态。

当前代码默认值（`main/example_es7210_es8311.c`）：

- `kLocalBargeInVadThresholdUs = 280 * 1000`
- `kLocalBargeInStartupGuardUs = 1000 * 1000`
- `kLocalBargeInArmSilenceUs = 180 * 1000`

menuconfig 操作步骤：

1. 进入工程目录后执行：
   ```bash
   idf.py menuconfig
   ```
2. 打开菜单：`ESP32 ES7210 + ES8311 AEC`
3. 勾选 `Enable Audio Noise Reduction`
4. 在该项上按 Enter 进入子项，勾选 `Enable Device-Side AEC`
5. 保存退出后重新编译烧录：
   ```bash
   idf.py build flash monitor
   ```

## 常见问题（FAQ）

### 1) 为什么 `canon.pcm` 没播出来？

- 检查文件是否存在：`main/assets/canon.pcm`
- 构建日志中应看到 `Embedded test PCM: ...`；若看到 `canon.pcm not found`，说明未嵌入
- 重新执行 `idf.py fullclean && idf.py build`，确保 `EMBED_FILES` 生效
- 该文件应为单声道 `int16_t` PCM（示例按 16kHz 本地测试链路设计）

### 2) AEC 开启后如何调三个阈值？

在 `main/example_es7210_es8311.c` 中调整：

- `kLocalBargeInVadThresholdUs`：触发打断前，连续检测到语音的最短时长
- `kLocalBargeInStartupGuardUs`：启动播放后的保护期（保护期内不触发打断）
- `kLocalBargeInArmSilenceUs`：进入“可打断状态”前要求的静音时长
- 默认值分别为：`500ms`、`1000ms`、`300ms`（用户可按现场环境自行修改）

建议调参方式：

- 误触发多：增大 `kLocalBargeInVadThresholdUs`、`kLocalBargeInArmSilenceUs`
- 打断不灵敏：减小 `kLocalBargeInVadThresholdUs`
- 开播即被打断：增大 `kLocalBargeInStartupGuardUs`

### 3) Boot 键单击/双击行为是什么？

按当前代码逻辑：

- **单击 Boot**
  - 测试模式中：在“开始录音测试”和“停止录音并回放”之间切换
  - 非测试模式：切换本地录音测试开关
- **双击 Boot**
  - 仅在测试模式时生效：退出测试模式，并停止当前本地录音测试/回放流程

## 说明

- 项目默认按 **4MB Flash** 进行配置（分区表为 `partitions/partitions.csv` 的单应用简化方案），可兼容 **4MB 及以上** Flash。
- 若出现编译、链接、分区或烧录相关报错，请根据开发板实际 Flash 容量与需求，自行调整 `sdkconfig.defaults*` 和 `partitions/partitions.csv`。
- 录音测试数据只用于当前运行会话，不做文件持久化。  

## 技术支持

- 代码修改：**surfer**
- 店铺：`surferai.taobao.com`
