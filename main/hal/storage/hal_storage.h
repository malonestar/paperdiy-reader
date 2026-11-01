/*
 * paperdiy-reader — storage HAL.
 *
 * Two FAT volumes:
 *   - the internal ~3 MB 'storage' partition, mounted at /books (fallback
 *     when there is no card; books there move to the card at boot)
 *   - the microSD card, mounted at /sdcard (books live in /sdcard/books)
 *
 * Both are FatFs (reentrant); hal_storage_lock() serialises multi-step
 * operations between the main task and the web portal.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which physical volume a book lives on. */
typedef enum {
    STORE_INTERNAL = 0, /*!< internal SPI flash FAT partition */
    STORE_SD       = 1, /*!< microSD card */
    STORE_COUNT
} store_id_t;

/** @brief Mount point of the internal FAT partition. */
#define STORAGE_INTERNAL_BASE "/books"
/** @brief Mount point of the microSD card. */
#define STORAGE_SD_BASE "/sdcard"
/** @brief Books directory on the microSD card. */
#define STORAGE_SD_BOOKS STORAGE_SD_BASE "/books"

/**
 * @brief Mounts the internal FAT volume and, if a card is present, the microSD.
 *
 * Formats the internal partition on first boot. Always succeeds for the SD
 * portion — a missing or unreadable card simply leaves it unmounted.
 *
 * @return ESP_OK when at least the internal volume mounted.
 */
esp_err_t hal_storage_init(void);

/** @brief Attempts to mount the microSD card. Safe to call repeatedly. */
esp_err_t hal_storage_mount_sd(void);

/** @brief True when the microSD card is mounted and usable. */
bool hal_storage_sd_available(void);

/** @brief Called after a book was copied, before the original is removed. */
typedef void (*hal_storage_moved_cb)(const char *from, const char *to, void *ctx);

/**
 * @brief Moves every .epub/.txt from internal storage to the SD card's /books
 *        folder. A copy is only trusted (and the original removed) when every
 *        read, write and close succeeded and the sizes match; names already
 *        present on the card are left alone.
 * @param copied   receives the number of files moved (may be NULL)
 * @param on_moved optional hook to migrate per-book state (progress, caches)
 */
esp_err_t hal_storage_copy_books_to_sd(int *copied, hal_storage_moved_cb on_moved, void *ctx);

/**
 * @brief Directory holding books for a volume.
 * @return "/books" or "/sdcard/books", or NULL if that volume is unavailable.
 */
const char *hal_storage_books_dir(store_id_t store);

/**
 * @brief Directory holding the pagination cache for a volume.
 * @return "/books/.cache" or "/sdcard/books/.cache", or NULL if unavailable.
 */
const char *hal_storage_cache_dir(store_id_t store);

/** @brief Short human label for a volume ("Internal" / "SD card"). */
const char *hal_storage_label(store_id_t store);

/**
 * @brief Reports capacity for a volume.
 * @return true when the numbers are valid.
 */
bool hal_storage_space(store_id_t store, uint64_t *total_bytes, uint64_t *free_bytes);

/** @brief Ensures a directory exists (single level, no parents). */
esp_err_t hal_storage_mkdir(const char *path);

/** @brief Serialises filesystem access between the UI task and the HTTP server. */
void hal_storage_lock(void);
/** @brief Releases the storage lock. */
void hal_storage_unlock(void);

#ifdef __cplusplus
}
#endif
