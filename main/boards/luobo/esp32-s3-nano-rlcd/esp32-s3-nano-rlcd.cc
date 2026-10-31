#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "codecs/dummy_audio_codec.h"
#include "config.h"
#include "cst836u_touch.h"
#include "custom_lcd_display.h"

#define TAG "nano-rlcd"

/*
 * ESP32-S3-Nano (N16R8) + ST7305 反射屏 + CST836U 触摸 + ES7210/ES8311 AEC 音频板
 *
 * 关键约定：
 *  - 触摸与两个 codec 共用同一条 I2C 总线（SDA 11 / SCL 12），只创建一次 bus。
 *  - 音频走 duplex 单总线：ES7210 用 TDM 4 通道收，ES8311 用标准立体声发。
 *  - ES7210 地址是 0x41（AD0 上拉），不是驱动默认的 0x40。
 */
static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0;
    int y = 0;
    if (cst836u_touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

class Esp32S3NanoRlcdBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_ = nullptr;
    bool touch_ready_ = false;

    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                // 板上已有 10k/2.2k 上拉，这里保持开启只为容错，不会造成总线过载
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        ESP_LOGI(TAG, "i2c bus ready (sda=%d scl=%d)", AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
    }

    // probe 只要地址 ACK 就算通过，但 codec 在 I2C 上应答不代表寄存器可读写
    // （缺 MCLK、内部状态机未就绪时就会这样），所以这里做一次真实的寄存器读。
    // 默认 400kHz：本板在 100kHz 下实测 0/10，只有 400kHz 稳定，初判也统一走该速率。
    bool ReadCodecReg(uint8_t addr, const char* name, bool quiet = false, uint32_t speed_hz = 400000)
    {
        // addr 为 8 位形式（如 0x30/0x82），i2c_master 需要 7 位地址，与 esp_codec_dev 保持一致
        uint8_t addr7 = addr >> 1;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr7,
            .scl_speed_hz = speed_hz,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
            if (!quiet) {
                ESP_LOGE(TAG, "%s(0x%02X) add_device failed", name, addr7);
            }
            return false;
        }
        uint8_t reg = 0x00;
        uint8_t val = 0;
        esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100));
        i2c_master_bus_rm_device(dev);
        if (!quiet) {
            ESP_LOGD(TAG, "%s(0x%02X) reg0 read: %s val=0x%02X", name, addr7, esp_err_to_name(ret), val);
        }
        return ret == ESP_OK;
    }

    // esp_codec_dev 内部用的是 i2c_master_transmit（只写，不带读），
    // 与上面的 transmit_receive 是两条不同的驱动路径，必须单独验证。
    // 这里读出原值再原值写回，不改变 codec 任何配置。
    bool TestCodecWrite(uint8_t addr, uint32_t speed_hz)
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = (uint8_t)(addr >> 1),
            .scl_speed_hz = speed_hz,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
            return false;
        }
        uint8_t reg = 0x00;
        uint8_t val = 0;
        bool ok = false;
        if (i2c_master_transmit_receive(dev, &reg, 1, &val, 1, pdMS_TO_TICKS(100)) == ESP_OK) {
            uint8_t buf[2] = {reg, val};
            ok = (i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(100)) == ESP_OK);
        }
        i2c_master_bus_rm_device(dev);
        return ok;
    }

    void InitializeDisplay()
    {
        // 参考 waveshare/esp32-s3-rlcd-4.2：panel_io 与 panel 传 NULL，
        // 由 CustomLcdDisplay 自建 SPI 与 LVGL display，走 1bpp LUT 直写。
        spi_display_config_t spi_config = {};
        spi_config.mosi = DISPLAY_SPI_MOSI_PIN;
        spi_config.scl = DISPLAY_SPI_SCK_PIN;
        spi_config.dc = DISPLAY_DC_PIN;
        spi_config.cs = DISPLAY_SPI_CS_PIN;
        spi_config.rst = DISPLAY_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0,
                                        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                        spi_config, SPI3_HOST);
    }

    void InitializeTouch()
    {
        touch_ready_ = cst836u_touch_init(i2c_bus_);
        if (!touch_ready_) {
            ESP_LOGW(TAG, "touch not available, continue without it");
            return;
        }

        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touchpad_read_cb);
        lv_indev_set_disp(indev, lv_display_get_default());
        ESP_LOGI(TAG, "touch indev registered");
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    Esp32S3NanoRlcdBoard() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        ESP_LOGI(TAG, "board ready");
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        // Application 会在启动和进入配网模式时各调用一次。必须只判定一次：
        // 否则第二次判定若通过仍会走 BoxAudioCodec，而它内部是 assert，
        // init 失败即整机重启，表现为屏幕反复闪「正在初始化」。
        static AudioCodec* selected = nullptr;
        if (selected != nullptr) {
            return selected;
        }

        // 两颗 codec 在冷启动（I2S/MCLK 尚未运行）阶段的 I2C 速率偏好相反：
        // ES8311 只在 400kHz 通（100kHz 实测 0/10），ES7210 只在 100kHz 通，
        // 所以必须各按各的速率探测，不能共用一个默认值。
        bool es8311_ok = false;
        for (int i = 0; i < 5 && !es8311_ok; i++) {
            es8311_ok = ReadCodecReg(AUDIO_CODEC_ES8311_ADDR, "ES8311", true, 400000);
            if (!es8311_ok) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        // ES7210 先 100kHz，失败后回退 400kHz（BoxAudioCodec 初始化时 MCLK 已开，本就是 400kHz）
        bool es7210_ok = false;
        for (int i = 0; i < 3 && !es7210_ok; i++) {
            es7210_ok = ReadCodecReg(AUDIO_CODEC_ES7210_ADDR, "ES7210", true, 100000);
            if (!es7210_ok) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (!es7210_ok) {
            for (int i = 0; i < 3 && !es7210_ok; i++) {
                es7210_ok = ReadCodecReg(AUDIO_CODEC_ES7210_ADDR, "ES7210", true, 400000);
                if (!es7210_ok) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }
        if (!es7210_ok) {
            ESP_LOGW(TAG, "ES7210 probe failed: 100kHz x3 and 400kHz x3 all NACK");
        }

        // ES8311 实测：10k/50k/100k 下读写全挂，只有 400kHz 100% 成功（BSS138 与低速时序冲突），
        // 因此只按 400kHz 读写各 3 次做判定，并把 codec 强制跑在 400kHz。
        // 保留写测试是因为 esp_codec_dev 走的是 i2c_master_transmit（写路径），
        // 读通不代表写通；TestCodecWrite 只把读到的原值写回，不改变任何配置。
        int read_400k = 0;
        int write_400k = 0;
        for (int i = 0; i < 3; i++) {
            if (ReadCodecReg(AUDIO_CODEC_ES8311_ADDR, "ES8311", true, 400000)) {
                read_400k++;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (TestCodecWrite(AUDIO_CODEC_ES8311_ADDR, 400000)) {
                write_400k++;
            }
        }
        ESP_LOGD(TAG, "400kHz read=%d/3 write=%d/3", read_400k, write_400k);

        if (!es8311_ok || !es7210_ok || read_400k < 3 || write_400k < 3) {
            ESP_LOGE(TAG, "audio codec unreliable (8311=%d 7210=%d), dummy", es8311_ok, es7210_ok);
            static DummyAudioCodec dummy_audio(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
            selected = &dummy_audio;
            return selected;
        }

        // 参数含义：输入增益 30dB、参考通道增益不作用（channel=-1, gain=0dB）、I2C 400kHz。
        // 曾用 36dB 补偿偏弱的人声，但底噪问题（功放常开）修复后已无必要：
        // 播报时回采通道偏低噪，而 36dB 把回声残留一起抬高 6dB，服务端 VAD 判成用户插话，
        // 表现为播放中被"幻听"打断。此处回到 30dB，与 esp32-es7210-es8311 参考工程一致。
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE, 30.0f, -1, 0.0f, 400000);
        selected = &audio_codec;

        // 若 codec 初始化失败，此时 I2S 已在运行（MCLK/BCLK 正在翻转）。
        // 再测一次 400kHz，用于区分"I2S 时钟串扰"与"驱动内部问题"：
        //   补测 3/3 → 不是串扰，问题在 esp_codec_dev 内部
        //   补测 0/3 → 确认 I2S 干扰，需调整初始化顺序或走线
        if (!audio_codec.IsReady()) {
            int after_i2s = 0;
            for (int i = 0; i < 3; i++) {
                if (ReadCodecReg(AUDIO_CODEC_ES8311_ADDR, "ES8311", true, 400000)) {
                    after_i2s++;
                }
            }
            ESP_LOGW(TAG, "after i2s start, 400kHz read: %d/3", after_i2s);
        }
        return selected;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }
};

DECLARE_BOARD(Esp32S3NanoRlcdBoard);
