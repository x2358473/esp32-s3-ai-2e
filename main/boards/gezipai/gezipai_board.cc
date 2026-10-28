#include "dual_network_board.h"
#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display.h"

#if HAVE_LVGL
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#define TAG "gezipai"
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_READ_INTERVAL_MS (10 * 1000)  // 10 秒

class GezipaiBoard : public DualNetworkBoard {
private:
    Button boot_button_;
    Button boot_button_2_;
    Button volume_up_button_;
    Button volume_down_button_;

    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;

    adc_oneshot_unit_handle_t adc1_handle_ = nullptr;
    adc_cali_handle_t adc1_cali_handle_ = nullptr;
    bool adc_calibrated_ = false;

    esp_timer_handle_t battery_timer_ = nullptr;
    int last_battery_voltage_ = 0;
    int last_battery_percentage_ = -1;
    int last_adc_raw_ = 0;
    int last_chrg_level_ = -1;
    int last_done_level_ = -1;

    // 无屏板卡仍需初始化 LVGL，否则 Assets 创建 LvglCBinFont 时会因 TLSF 未初始化崩溃
    static void EnsureLvglInitialized() {
#if HAVE_LVGL
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            initialized = true;
            ESP_LOGI(TAG, "[DISPLAY] LVGL 已初始化（无屏，仅用于字体资源）");
        }
#endif
    }

    // 1 秒节拍：LED 翻转 + CHRG/DONE 边沿检测；每 10 拍读一次电池
    static void BatteryTimerCallback(void* arg) {
        auto* board = static_cast<GezipaiBoard*>(arg);
        static uint8_t tick = 0;
        tick++;

        int chrg = gpio_get_level(CHARGING_GPIO);
        int done = gpio_get_level(DONE_GPIO);
        if (chrg != board->last_chrg_level_ || done != board->last_done_level_) {
            ESP_LOGI(TAG, "[CHG] CHRG(IO%d)=%d DONE(IO%d)=%d (电平变化)",
                     static_cast<int>(CHARGING_GPIO), chrg,
                     static_cast<int>(DONE_GPIO), done);
            board->last_chrg_level_ = chrg;
            board->last_done_level_ = done;
        }

        if (tick >= BATTERY_READ_INTERVAL_MS / 1000) {
            board->UpdateBatteryStatus();
            tick = 0;
        }

        gpio_set_level(BUILTIN_LED_GPIO, tick % 2);
    }

    int ReadBatteryVoltage() {
        int adc_raw = 0;
        long long sum = 0;
        int voltage = 0;
        for (int i = 0; i < 10; i++) {
            ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle_, BATTERY_ADC_CHANNEL, &adc_raw));
            sum += adc_raw;
        }
        adc_raw = sum / 10;
        last_adc_raw_ = adc_raw;

        if (adc_calibrated_) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_handle_, adc_raw, &voltage));
        }
        // 模块内部分压，实测系数为 3
        return voltage * 3;
    }

    int CalculateBatteryPercentage(int voltage) {
        if (!adc_calibrated_ || voltage <= 0) {
            return -1;
        }
        const int min_voltage = 3300;
        const int max_voltage = 4200;
        if (voltage < min_voltage) {
            return 0;
        } else if (voltage > max_voltage) {
            return 100;
        }
        return (voltage - min_voltage) * 100 / (max_voltage - min_voltage);
    }

    void UpdateBatteryStatus() {
        last_battery_voltage_ = ReadBatteryVoltage();
        last_battery_percentage_ = CalculateBatteryPercentage(last_battery_voltage_);

        if (!adc_calibrated_) {
            ESP_LOGW(TAG, "电池ADC未校准，读数为估算值");
        }
        ESP_LOGD(TAG, "电池 raw=%d %dmV %d%% | CHRG(IO%d)=%d DONE(IO%d)=%d | 耳机(IO%d)=%d",
                 last_adc_raw_, last_battery_voltage_, last_battery_percentage_,
                 static_cast<int>(CHARGING_GPIO), gpio_get_level(CHARGING_GPIO),
                 static_cast<int>(DONE_GPIO), gpio_get_level(DONE_GPIO),
                 static_cast<int>(EARPHONE_CK_PIN), gpio_get_level(EARPHONE_CK_PIN));
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &codec_i2c_bus_));
    }

    void InitializeBatteryAdc() {
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc1_handle_));

        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle_, BATTERY_ADC_CHANNEL, &chan_cfg));

        adc_cali_handle_t handle = nullptr;
        esp_err_t ret = ESP_FAIL;
        adc_calibrated_ = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t curve_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = BATTERY_ADC_CHANNEL,
            .atten = BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&curve_cfg, &handle);
        if (ret == ESP_OK) {
            adc_calibrated_ = true;
        }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (!adc_calibrated_) {
            adc_cali_line_fitting_config_t line_cfg = {
                .unit_id = ADC_UNIT_1,
                .atten = BATTERY_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ret = adc_cali_create_scheme_line_fitting(&line_cfg, &handle);
            if (ret == ESP_OK) {
                adc_calibrated_ = true;
            }
        }
#endif

        adc1_cali_handle_ = handle;
        if (adc_calibrated_) {
            ESP_LOGI(TAG, "ADC校准成功");
        } else {
            ESP_LOGW(TAG, "ADC校准失败，电量精度下降");
        }
    }

    void InitializeTimer() {
        esp_timer_create_args_t args = {
            .callback = &BatteryTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_timer",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &battery_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer_, 1000 * 1000));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "[KEY] BOOT click");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigModeIfWifi();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[KEY] BOOT long press -> wifi config");
            EnterWifiConfigModeIfWifi();
        });

        // IO2 / IO18 为预留焊盘，实体键不存在；Button 对 GPIO_NUM_NC 有内建保护
        boot_button_2_.OnClick([this]() {
            ESP_LOGI(TAG, "[KEY] BOOT2 click");
            Application::GetInstance().ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            ESP_LOGI(TAG, "[KEY] VOL+ click");
            auto codec = GetAudioCodec();
            if (codec == nullptr) {
                return;
            }
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
        });

        volume_up_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[KEY] VOL+ long press");
            auto codec = GetAudioCodec();
            if (codec != nullptr) {
                codec->SetOutputVolume(100);
            }
        });

        volume_down_button_.OnClick([this]() {
            ESP_LOGI(TAG, "[KEY] VOL- click");
            auto codec = GetAudioCodec();
            if (codec == nullptr) {
                return;
            }
            int volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
        });

        volume_down_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[KEY] VOL- long press");
            auto codec = GetAudioCodec();
            if (codec != nullptr) {
                codec->SetOutputVolume(0);
            }
        });
    }

    void EnterWifiConfigModeIfWifi() {
        if (GetNetworkType() == NetworkType::WIFI) {
            auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
            wifi_board.EnterWifiConfigMode();
        } else {
            ESP_LOGW(TAG, "当前为 4G 网络，跳过 WiFi 配网");
        }
    }

public:
    GezipaiBoard()
        : DualNetworkBoard(Module_4G_TX_PIN, Module_4G_RX_PIN, Module_4G_DTR_PIN, 0),
          boot_button_(BOOT_BUTTON_GPIO),
          boot_button_2_(BOOT_BUTTON_2_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO),
          volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Initializing Gezipai Board");

        // 本板无屏，不会创建 LcdDisplay，因此 lv_init() 从未被调用。
        // 但 2.5.2 的 Assets::LvglStrategy::Apply 会在 display==nullptr 判断之前
        // 就构造 LvglCBinFont（内部调用 cbin_font_create -> lv_malloc），
        // TLSF 控制块未初始化时会空指针崩溃（block_locate_free）。
        // 这里提前初始化 LVGL，只为让字体资源能正常分配/释放，不创建任何界面。
        EnsureLvglInitialized();

        // LED 与 4G 电源脚：板上无 4G 模块，IO17 保持低电平断电
        gpio_config_t out_cfg = {
            .pin_bit_mask = (1ULL << BUILTIN_LED_GPIO) | (1ULL << MODULE_4G_POWER_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&out_cfg));
        gpio_set_level(MODULE_4G_POWER_GPIO, 0);
        gpio_set_level(BUILTIN_LED_GPIO, 0);

        // CHRG 为充电芯片开漏输出，需内部上拉；DONE 与 USB D-(IO19) 复用，不加上拉以免影响 USB
        gpio_config_t chrg_cfg = {
            .pin_bit_mask = 1ULL << CHARGING_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&chrg_cfg));

        // 耳机插入检测（低有效）
        gpio_config_t earphone_cfg = {
            .pin_bit_mask = 1ULL << EARPHONE_CK_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&earphone_cfg));

        InitializeCodecI2c();
        InitializeButtons();
        InitializeBatteryAdc();

#ifdef CONFIG_USE_DEVICE_AEC
        ESP_LOGI(TAG, "[SELFTEST] device AEC = ON");
#else
        ESP_LOGW(TAG, "[SELFTEST] device AEC = OFF (语音打断会退化)");
#endif
#ifdef CONFIG_USE_AFE_WAKE_WORD
        ESP_LOGI(TAG, "[SELFTEST] wake word = AFE");
#elif defined(CONFIG_USE_CUSTOM_WAKE_WORD)
        ESP_LOGI(TAG, "[SELFTEST] wake word = custom");
#else
        ESP_LOGW(TAG, "[SELFTEST] wake word = disabled");
#endif
        ESP_LOGI(TAG, "[SELFTEST] 4G power(IO%d)=%d, adc calibrated=%d",
                 static_cast<int>(MODULE_4G_POWER_GPIO), gpio_get_level(MODULE_4G_POWER_GPIO),
                 static_cast<int>(adc_calibrated_));

        UpdateBatteryStatus();
        InitializeTimer();
    }

    ~GezipaiBoard() override {
        if (battery_timer_ != nullptr) {
            esp_timer_stop(battery_timer_);
            esp_timer_delete(battery_timer_);
        }
        if (adc1_handle_ != nullptr) {
            adc_oneshot_del_unit(adc1_handle_);
        }
        if (adc_calibrated_ && adc1_cali_handle_ != nullptr) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_delete_scheme_curve_fitting(adc1_cali_handle_);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
            adc_cali_delete_scheme_line_fitting(adc1_cali_handle_);
#endif
        }
    }

    // 无屏幕：沿用 Board::GetDisplay() 的 NoDisplay，不覆写
    // LED：沿用 Board::GetLed() 的 NoLed，不覆写

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            true);
        return &audio_codec;
    }

    // CHRG 低电平表示充电中；DONE 与 USB D- 复用，插 USB 时不可信，不参与判断
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        level = last_battery_percentage_;
        charging = (gpio_get_level(CHARGING_GPIO) == 0);
        discharging = !charging;
        return true;
    }
};

DECLARE_BOARD(GezipaiBoard);
