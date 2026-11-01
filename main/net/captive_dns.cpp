/*
 * paperdiy-reader — captive-portal DNS responder.
 *
 * SPDX-License-Identifier: MIT
 */
#include "net/captive_dns.h"

#include <cerrno>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "dns";

namespace net {

namespace {

volatile bool s_run      = false;
SemaphoreHandle_t s_done = nullptr;
uint32_t s_ip            = 0;

void dnsTask(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct timeval tv = {1, 0};  // wake up every second to check s_run
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in addr     = {};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(53);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(TAG, "bind failed (%d)", errno);
            close(sock);
            sock = -1;
        }
    }

    static uint8_t rx[512];
    static uint8_t tx[528];
    while (sock >= 0 && s_run) {
        sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len        = recvfrom(sock, rx, sizeof(rx), 0, (sockaddr*)&from, &flen);
        if (len <= 0) {
            continue;  // timeout: re-check s_run
        }
        int n = dnsBuildReply(rx, len, s_ip, tx, sizeof(tx));
        if (n > 0) {
            sendto(sock, tx, (size_t)n, 0, (sockaddr*)&from, flen);
        }
    }
    if (sock >= 0) {
        close(sock);
    }
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

}  // namespace

bool dnsStart(uint32_t ip)
{
    if (s_run) {
        return true;
    }
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    s_ip  = ip;
    s_run = true;
    if (xTaskCreate(dnsTask, "dns", 3072, nullptr, 4, nullptr) != pdPASS) {
        s_run = false;
        return false;
    }
    return true;
}

void dnsStop()
{
    if (!s_run) {
        return;
    }
    s_run = false;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(3000));  // the task closes its own socket
}

}  // namespace net
