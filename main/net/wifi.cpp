/*
 * paperdiy-reader — minimal Wi-Fi control.
 *
 * SPDX-License-Identifier: MIT
 */
#include "net/wifi.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"

static const char* TAG = "wifi";

namespace net {

namespace {

constexpr EventBits_t BIT_GOT_IP = BIT0;
constexpr EventBits_t BIT_FAILED = BIT1;

bool s_init              = false;
bool s_started           = false;
esp_netif_t* s_sta       = nullptr;
esp_netif_t* s_ap        = nullptr;
EventGroupHandle_t s_ev  = nullptr;
volatile bool s_want_sta = false;
volatile int s_retries   = 0; /* written by the event task, read by joinSta */
volatile uint8_t s_reason = 0;

std::string ipString(esp_netif_t* nif)
{
    esp_netif_ip_info_t info;
    if (!nif || esp_netif_get_ip_info(nif, &info) != ESP_OK || info.ip.addr == 0) {
        return std::string();
    }
    char buf[16];
    inet_ntoa_r(info.ip.addr, buf, sizeof(buf));
    return buf;
}

void onEvent(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* d   = (wifi_event_sta_disconnected_t*)data;
        s_reason  = d ? d->reason : 0;
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        // Retry a few times while joining (routers often drop the first
        // attempt); give up quickly on a definite password failure.
        bool auth_fail = s_reason == WIFI_REASON_AUTH_FAIL || s_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                         s_reason == WIFI_REASON_HANDSHAKE_TIMEOUT || s_reason == WIFI_REASON_AUTH_EXPIRE;
        if (s_want_sta && !auth_fail && s_retries < 4) {
            s_retries = s_retries + 1;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

void radioOff()
{
    s_want_sta = false;
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = false;
    }
    esp_wifi_set_mode(WIFI_MODE_NULL);
}

bool radioOn(wifi_mode_t mode)
{
    radioOff();
    if (esp_wifi_set_mode(mode) != ESP_OK) {
        return false;
    }
    return true;
}

bool isHex64(const std::string& p)
{
    return p.size() == 64 && std::all_of(p.begin(), p.end(), [](char c) { return isxdigit((unsigned char)c); });
}

}  // namespace

bool validCredentials(const std::string& ssid, const std::string& pass)
{
    if (ssid.empty() || ssid.size() > 32) {
        return false;
    }
    return pass.empty() || (pass.size() >= 8 && pass.size() <= 63) || isHex64(pass);
}

const char* joinErrorText(JoinError e)
{
    switch (e) {
        case JoinError::None:
            return "";
        case JoinError::NotFound:
            return "network not found";
        case JoinError::BadPassword:
            return "wrong password";
        case JoinError::Timeout:
            return "no answer from the network";
        default:
            return "Wi-Fi error";
    }
}

bool init()
{
    if (s_init) {
        return true;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap  = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "paperdiy-reader");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // credentials live in our own NVS settings
    s_ev = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr, nullptr);
    s_init = true;
    return true;
}

std::vector<ScanResult> scan()
{
    std::vector<ScanResult> out;
    if (!init() || !radioOn(WIFI_MODE_STA) || esp_wifi_start() != ESP_OK) {
        return out;
    }
    s_started = true;

    wifi_scan_config_t sc = {};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return out;
    }
    uint16_t n = 24;
    wifi_ap_record_t recs[24];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return out;
    }
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid((const char*)recs[i].ssid, strnlen((const char*)recs[i].ssid, sizeof(recs[i].ssid)));
        if (ssid.empty()) {
            continue;
        }
        auto it = std::find_if(out.begin(), out.end(), [&](const ScanResult& r) { return r.ssid == ssid; });
        if (it != out.end()) {
            it->rssi = std::max(it->rssi, (int)recs[i].rssi);
            continue;
        }
        ScanResult r;
        r.ssid   = ssid;
        r.rssi   = recs[i].rssi;
        r.secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    ESP_LOGI(TAG, "scan: %u network(s)", (unsigned)out.size());
    return out;
}

bool joinSta(const char* ssid, const char* pass, uint32_t timeout_ms, std::string* ip, JoinError* err)
{
    if (err) {
        *err = JoinError::Internal;
    }
    std::string s = ssid ? ssid : "";
    std::string p = pass ? pass : "";
    if (!init() || !validCredentials(s, p) || !radioOn(WIFI_MODE_STA)) {
        return false;
    }

    wifi_config_t wc = {};
    memcpy(wc.sta.ssid, s.data(), std::min(s.size(), sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, p.data(), std::min(p.size(), sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = p.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wc.sta.scan_method        = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        return false;
    }

    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
    s_retries  = 0;
    s_reason   = 0;
    s_want_sta = true;
    if (esp_wifi_start() != ESP_OK) {
        s_want_sta = false;
        return false;
    }
    s_started = true;
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) {
        if (ip) {
            *ip = ipString(s_sta);
        }
        if (err) {
            *err = JoinError::None;
        }
        ESP_LOGI(TAG, "joined '%s' as %s", s.c_str(), ipString(s_sta).c_str());
        return true;
    }

    if (err) {
        switch (s_reason) {
            case WIFI_REASON_NO_AP_FOUND:
                *err = JoinError::NotFound;
                break;
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_AUTH_EXPIRE:
                *err = JoinError::BadPassword;
                break;
            default:
                *err = JoinError::Timeout;
                break;
        }
    }
    ESP_LOGW(TAG, "join '%s' failed (reason %u)", s.c_str(), (unsigned)s_reason);
    radioOff();
    return false;
}

bool startAp(const char* ssid, std::string* ip)
{
    if (!init() || !ssid || !radioOn(WIFI_MODE_AP)) {
        return false;
    }
    wifi_config_t wc = {};
    size_t n         = std::min(strlen(ssid), sizeof(wc.ap.ssid));
    memcpy(wc.ap.ssid, ssid, n);
    wc.ap.ssid_len       = (uint8_t)n;
    wc.ap.channel        = 6;
    wc.ap.authmode       = WIFI_AUTH_OPEN;
    wc.ap.max_connection = 4;
    if (esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK || esp_wifi_start() != ESP_OK) {
        radioOff();
        return false;
    }
    s_started = true;
    if (ip) {
        *ip = ipString(s_ap);
        if (ip->empty()) {
            *ip = "192.168.4.1";
        }
    }
    ESP_LOGI(TAG, "setup AP '%s' up", ssid);
    return true;
}

void stop()
{
    if (s_init) {
        radioOff();
        ESP_LOGI(TAG, "radio off");
    }
}

bool staConnected()
{
    return s_ev && (xEventGroupGetBits(s_ev) & BIT_GOT_IP);
}

std::string staIp()
{
    return staConnected() ? ipString(s_sta) : std::string();
}

}  // namespace net
