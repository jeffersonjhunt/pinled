#pragma once

/**
 * @file pinled_doc_file.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Reading and writing stored documents as files (FR-CFG-7/15).
 * @version 0.4.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Why this is `<cstdio>` and not an ESP-IDF API
 * LittleFS is mounted through the IDF's VFS layer, so on the target
 * `fopen("/cfg/install.pb", "rb")` is the real interface — `esp_littlefs` has
 * no separate file API worth using here. Writing against stdio therefore costs
 * nothing on the device and buys something large off it: this file's behaviour
 * can be tested on the host against a temporary directory, with genuinely
 * truncated and genuinely corrupt files rather than simulated ones.
 *
 * The only part that is target-specific is the *mount*, which lives in
 * `machine_config`.
 *
 * @par Durability, precisely
 * `FR-CFG-15` exists because filesystem consistency and write durability are
 * different promises and LittleFS only makes the first. So a write goes to a
 * sibling temporary file, is flushed and synced, and only then replaced over
 * the destination by `rename()`. A power cut at any point leaves either the
 * previous document or the new one — never a half-written file that passes its
 * own length check.
 *
 * That is also why a failed write must leave the destination alone, and why
 * `doc_file_write` never truncates the destination itself.
 */

#include <cstddef>
#include <cstdint>

#include "pinled_doc_frame.h"

namespace ooe::pinled
{
    /**
     * @brief Outcome of a file-level operation.
     *
     * `NotFound` is deliberately its own value and not folded into an error:
     * on a device that has never been configured it is the *expected* result
     * and the correct response is Kconfig defaults (FR-CFG-4), whereas a
     * corrupt document is a fault worth shouting about. Collapsing "failed"
     * and "found nothing" into one answer is how a broken store boots looking
     * healthy.
     */
    enum class FileStatus
    {
        Ok = 0,
        NotFound,    ///< no such file — normal on a fresh device
        TooLarge,    ///< exceeds the caller's buffer or kMaxDocBytes
        ReadError,   ///< open/seek/read failed on a file that exists
        WriteError,  ///< open/write/sync/rename failed; destination untouched
        BadDocument, ///< framing or CRC rejected it; see the DocStatus out-param
    };

    /**
     * @brief Ceiling on a stored document, as a sanity bound.
     *
     * The largest legitimate document is a 128-lamp profile at roughly 9 KB
     * (`proto/pinled.options`). 32 KB is loose enough to never be the reason a
     * real document is refused, and tight enough that a corrupt length cannot
     * ask the boot path for an absurd allocation.
     */
    inline constexpr size_t kMaxDocBytes = 32 * 1024;

    /// Longest path this layer will handle, including the ".tmp" it appends.
    /// Fixed so that writing a document allocates nothing (NFR-4).
    inline constexpr size_t kMaxDocPath = 96;

    /**
     * @brief Read a framed document into @p buf and validate it.
     *
     * The whole file is read before anything is checked, then handed to
     * `doc_frame_read`, so every rejection path is the one already covered by
     * the frame tests rather than a second implementation of the same checks.
     *
     * @param path         file to read
     * @param buf          destination for the entire framed document
     * @param cap          capacity of @p buf
     * @param kind         receives the document kind; may be null
     * @param payload      receives a pointer INTO @p buf; may be null
     * @param payload_len  receives the payload length; may be null
     * @param doc_status   receives the framing verdict; may be null. Set for
     *                     every outcome, so a log line can say *why* a document
     *                     was rejected rather than only that it was.
     *
     * @return FileStatus::Ok when the payload is safe to hand to the decoder.
     *
     * @note On success @p payload aliases @p buf. Nothing is allocated here;
     *       the caller owns @p buf.
     */
    FileStatus doc_file_read(const char *path,
                             uint8_t *buf, size_t cap,
                             DocKind *kind,
                             const uint8_t **payload, size_t *payload_len,
                             DocStatus *doc_status = nullptr);

    /**
     * @brief Write a framed document, replacing @p path atomically.
     *
     * Writes `<path>.tmp`, flushes it to the medium, then renames it over the
     * destination. The header and payload are written separately so a large
     * payload is never copied merely to prepend sixteen bytes.
     *
     * @param path     destination
     * @param kind     which document this is
     * @param payload  encoded protobuf; may be null only when @p len is 0
     * @param len      payload length
     *
     * @return FileStatus::Ok, or WriteError with the destination unchanged.
     *
     * @note A leftover `<path>.tmp` from an interrupted write is harmless and
     *       is overwritten by the next attempt. It is never read.
     */
    FileStatus doc_file_write(const char *path, DocKind kind,
                              const uint8_t *payload, size_t len);

    /// True if @p path exists and can be opened for reading.
    bool doc_file_exists(const char *path);

    /// Human-readable status, for logs and test failure messages.
    const char *file_status_str(FileStatus s);
} // namespace ooe::pinled
