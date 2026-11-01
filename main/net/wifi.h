/*
 * paperdiy-reader — minimal Wi-Fi control.
 *
 * Only what the reader needs: scan, join a home network (station), run a
 * setup access point, and switch the radio fully off. The radio is off
 * whenever the Wi-Fi screen is not open.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace net {

struct ScanResult {
    std::string ssid;
    int rssi    = 0;
    bool secure = false;
};

/** @brief Why the last join failed (for a useful on-screen message). */
enum class JoinError : uint8_t {
    None,
    NotFound,    /*!< network not in range */
    BadPassword, /*!< authentication failed */
    Timeout,     /*!< associated but no IP, or no answer */
    Internal,
};

/** @brief One-time setup of netif, event loop and the Wi-Fi driver. */
bool init();

/** @brief Scans for networks (station mode, ~2-3 s). Strongest first, deduplicated. */
std::vector<ScanResult> scan();

/**
 * @brief Joins @p ssid as a station (no access point).
 * @return true once an IP address was obtained within @p timeout_ms.
 */
bool joinSta(const char* ssid, const char* pass, uint32_t timeout_ms, std::string* ip, JoinError* err);

/** @brief Starts an open setup access point. @p ip receives its address. */
bool startAp(const char* ssid, std::string* ip);

/** @brief Disconnects and switches the radio off. */
void stop();

bool staConnected();
std::string staIp();

/** @brief True for credentials the driver will accept (SSID 1-32 bytes;
 *         password empty, 8-63 characters, or 64 hex digits). */
bool validCredentials(const std::string& ssid, const std::string& pass);

const char* joinErrorText(JoinError e);

}  // namespace net
