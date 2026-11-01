/*
 * paperdiy-reader — captive-portal DNS responder.
 *
 * While the setup access point is up, every A query is answered with the
 * reader's own address, so phones pop the setup page up by themselves.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace net {

/** @brief Starts the responder for @p ip (network byte order). */
bool dnsStart(uint32_t ip);

/** @brief Stops it; returns once the socket is closed and the task is gone. */
void dnsStop();

/**
 * @brief Builds the reply to one DNS query (exposed for host tests).
 * @return reply length, or 0 to send nothing.
 */
int dnsBuildReply(const uint8_t* req, int len, uint32_t ip, uint8_t* out, int out_cap);

}  // namespace net
