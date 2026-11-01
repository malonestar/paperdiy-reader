/*
 * paperdiy-reader — top-level application loop.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace reader_app {

/**
 * @brief Runs the reader. Never returns.
 *
 * Mounts storage, reopens the book that was open at power-off (NVS resume
 * record), and otherwise shows the library.
 */
void run();

}  // namespace reader_app
