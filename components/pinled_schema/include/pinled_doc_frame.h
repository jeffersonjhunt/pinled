#pragma once

/**
 * @file pinled_doc_frame.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief On-disk framing for a stored configuration document.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * A stored document is a protobuf payload (`proto/pinled.proto`) wrapped in a
 * fixed 16-byte header carrying a magic, a kind, a length and a CRC.
 *
 * @par Why the frame is not itself a protobuf message
 * The obvious design is a `StoredDocument` message with `crc` and `payload`
 * fields. It is wrong: you would have to run the protobuf decoder over
 * possibly-corrupt bytes in order to discover that they are corrupt. A raw
 * header is checked with three comparisons and an arithmetic pass, and the
 * decoder never sees a payload that has not already been vouched for.
 *
 * @par Why a length as well as a CRC
 * The file length is not the payload length once anything appends, and a
 * truncated write is the exact failure this guards. Length is validated before
 * the CRC is computed, so a corrupt length cannot walk the checksum off the
 * end of the buffer.
 *
 * @par Layout (little-endian, 16-byte header)
 * @verbatim
 *   offset  size  field
 *   0       4     magic          "PLD1"
 *   4       1     frame_version  1
 *   5       1     kind           DocKind
 *   6       2     reserved       0
 *   8       4     payload_len
 *   12      4     crc32          over payload only
 *   16      ...   payload        protobuf
 * @endverbatim
 *
 * `frame_version` is the framing, not the schema. Schema evolution is
 * protobuf's job (FR-CFG-13) and must not bump this; this changes only if the
 * header layout itself ever does.
 *
 * No ESP-IDF, no allocation, no exceptions — see `pinled_crc32.h`.
 */

#include <cstddef>
#include <cstdint>

namespace ooe::pinled
{
    /// Which document a frame carries. Values are on-disk and permanent.
    enum class DocKind : uint8_t
    {
        Unknown = 0,
        InstallConfig = 1,
        MachineProfile = 2,
        Version = 3, ///< off-device only; the board stores no history (FR-CFG-10)
    };

    enum class DocStatus
    {
        Ok = 0,
        ShortBuffer,  ///< output buffer too small, or input shorter than a header
        BadMagic,     ///< not a pinled document
        BadVersion,   ///< framing version this build does not understand
        BadLength,    ///< declared payload length does not fit the input
        BadCrc,       ///< header intact, payload does not match its checksum
    };

    /// Size of the fixed header, in bytes.
    inline constexpr size_t kDocHeaderSize = 16;

    /// Total on-disk size of a frame carrying @p payload_len bytes.
    inline constexpr size_t doc_frame_size(size_t payload_len)
    {
        return kDocHeaderSize + payload_len;
    }

    /**
     * @brief Write a framed document into @p out.
     *
     * @param out        destination buffer
     * @param out_cap    capacity of @p out
     * @param kind       which document this is
     * @param payload    encoded protobuf; may be null only when @p len is 0
     * @param len        payload length
     * @param written    receives the total bytes written; may be null
     *
     * @return DocStatus::Ok, or ShortBuffer if @p out_cap is too small.
     */
    DocStatus doc_frame_write(uint8_t *out, size_t out_cap,
                              DocKind kind,
                              const uint8_t *payload, size_t len,
                              size_t *written);

    /**
     * @brief Validate a framed document and locate its payload.
     *
     * Checks magic, framing version, declared length and CRC — in that order,
     * so a corrupt length is rejected before it can be used as a bound.
     *
     * @param in           buffer as read from storage
     * @param len          bytes available in @p in
     * @param kind         receives the document kind; may be null
     * @param payload      receives a pointer INTO @p in; may be null
     * @param payload_len  receives the payload length; may be null
     *
     * @return DocStatus::Ok when the payload is safe to hand to the decoder.
     *
     * @note On success @p payload aliases @p in. Nothing is copied and nothing
     *       is allocated; the caller owns the lifetime of @p in.
     */
    DocStatus doc_frame_read(const uint8_t *in, size_t len,
                             DocKind *kind,
                             const uint8_t **payload, size_t *payload_len);

    /// Human-readable form of a status, for logs and test failure messages.
    const char *doc_status_str(DocStatus s);
} // namespace ooe::pinled
