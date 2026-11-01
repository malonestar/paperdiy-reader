/*
 * paperdiy-reader — Wi-Fi & files portal.
 *
 * Opening "Wi-Fi & files" on the reader:
 *   - with a saved home network: joins it and serves the web UI on the LAN
 *     (http://reader.local or the IP shown on screen);
 *   - otherwise, or when the join fails: starts an open setup access point
 *     ("PaperDIY-XXXXXX") with a captive page where the home network can be
 *     chosen. Saving it switches the reader over to the home network.
 *
 * The web UI uploads books, manages files on the SD card, prepares
 * screensaver images (16 greys, in the browser) and configures Wi-Fi.
 * Leaving the screen switches the radio off.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace portal {

enum class Mode : uint8_t {
    Off,
    Joining, /*!< connecting to the home network */
    Lan,     /*!< on the home network */
    Setup,   /*!< setup access point */
};

/** @brief What the on-screen Wi-Fi page shows. */
struct Info {
    Mode mode = Mode::Off;
    std::string ap_ssid;  /*!< setup AP name */
    std::string url;      /*!< where to point a browser */
    std::string ssid;     /*!< home network (saved or joined) */
    std::string message;  /*!< last error / hint, may be empty */
};

/** @brief Brings the portal up (joins home Wi-Fi or starts the setup AP). */
esp_err_t start();

/** @brief Stops the server and switches the radio off. */
void stop();

/** @brief Re-runs setup: drops to the access point even with a saved network. */
void forceSetup();

/**
 * @brief Main-loop hook: performs queued Wi-Fi changes requested from the
 *        browser (they cannot run inside an HTTP handler).
 * @return true when the on-screen info changed and the page should redraw.
 */
bool poll();

/** @brief Current state for the screen. */
const Info& info();

bool isRunning();

/** @brief True once the browser pressed "Finish". */
bool finishRequested();

/** @brief Milliseconds since the last HTTP request (idle handling). */
uint32_t idleMs();

}  // namespace portal
