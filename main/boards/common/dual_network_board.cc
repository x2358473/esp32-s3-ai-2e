#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <ssid_manager.h>
#include <vector>

static const char *TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin, int32_t default_net_type) 
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_dtr_pin_(ml307_dtr_pin) {
    
    // 从Settings加载网络类型
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);

    // 检查手动切换标记：如果存在则本次启动禁止自动切换，然后清除标记
    {
        Settings settings("network", true);
        if (settings.GetInt("manual_sw", 0) == 1) {
            ESP_LOGI(TAG, "Manual switch flag detected, auto-switch disabled for this boot");
            auto_switch_allowed_ = false;
            settings.EraseKey("manual_sw");
        }
    }
    
    // 只初始化当前网络类型对应的板卡
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type); // 默认使用ML307 (1)
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchNetworkType() {
    auto display = GetDisplay();

    // 设置手动切换标记，下次启动时禁止自动切换
    {
        Settings settings("network", true);
        settings.SetInt("manual_sw", 1);
    }

    if (network_type_ == NetworkType::WIFI) {    
        SaveNetworkTypeToSettings(NetworkType::ML307);
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    } else {
        SaveNetworkTypeToSettings(NetworkType::WIFI);
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

void DualNetworkBoard::AutoSwitchNetworkType(NetworkType target) {
    if (target == network_type_) {
        return;
    }
    auto display = GetDisplay();
    SaveNetworkTypeToSettings(target);
    if (target == NetworkType::WIFI) {
        ESP_LOGI(TAG, "Auto-switching to WiFi");
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    } else {
        ESP_LOGI(TAG, "Auto-switching to 4G");
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

 
std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

bool DualNetworkBoard::ScanForSavedSsid() {
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        ESP_LOGI(TAG, "No saved SSIDs, skip WiFi scan");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed for scan: %s", esp_err_to_name(ret));
        return false;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "WiFi scan found %d APs", ap_count);

    bool found = false;
    if (ap_count > 0) {
        std::vector<wifi_ap_record_t> ap_records(ap_count);
        esp_wifi_scan_get_ap_records(&ap_count, ap_records.data());

        for (auto& ap : ap_records) {
            std::string scanned_ssid(reinterpret_cast<char*>(ap.ssid));
            for (auto& saved : ssid_list) {
                if (scanned_ssid == saved.ssid) {
                    ESP_LOGI(TAG, "Found saved SSID: %s (RSSI: %d)", scanned_ssid.c_str(), ap.rssi);
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    return found;
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();

    if (auto_switch_allowed_) {
        if (network_type_ == NetworkType::ML307) {
            display->SetStatus(Lang::Strings::SCANNING_WIFI);
            ESP_LOGI(TAG, "4G mode: scanning for saved WiFi SSIDs...");
            if (ScanForSavedSsid()) {
                ESP_LOGI(TAG, "Saved WiFi SSID found, auto-switching to WiFi");
                AutoSwitchNetworkType(NetworkType::WIFI);
                return;
            }
            ESP_LOGI(TAG, "No saved WiFi SSID found, staying on 4G");
        }
    }
    
    if (network_type_ == NetworkType::WIFI) {
        display->SetStatus(Lang::Strings::CONNECTING);
        if (auto_switch_allowed_) {
            auto& wifi_board = static_cast<WifiBoard&>(*current_board_);
            wifi_board.SetSkipConfigOnFail(true);
        }
    } else {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();

    if (auto_switch_allowed_ && network_type_ == NetworkType::WIFI) {
        auto& wifi_board = static_cast<WifiBoard&>(*current_board_);
        if (wifi_board.IsWifiConnectFailed()) {
            ESP_LOGI(TAG, "WiFi connection failed, auto-switching to 4G");
            AutoSwitchNetworkType(NetworkType::ML307);
            return;
        }
    }
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    current_board_->SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
