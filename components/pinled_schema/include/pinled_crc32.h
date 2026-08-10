#pragma once

/**
 * @file pinled_crc32.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief CRC-32 (IEEE 802.3) over stored documents.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Exists because filesystem consistency and write durability are different
 * promises (FR-CFG-15). LittleFS guarantees that the filesystem is coherent
 * after a power cut; it does not guarantee that the last document written
 * reached flash intact. A checksum over the payload is the cheap half of that
 * difference.
 *
 * Standard CRC-32/ISO-HDLC: reflected polynomial 0xEDB88320, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF. The canonical check value for the ASCII string
 * "123456789" is 0xCBF43926, which the host tests assert against — a bit-order
 * mistake here would otherwise only surface as an unreadable config months
 * later.
 *
 * Deliberately free of ESP-IDF: this whole component builds and is tested on
 * the host (`test/host`), which is the point of putting it here rather than in
 * `machine_config`. IDF has `esp_rom_crc32_le`, but using it would make the
 * schema layer untestable off-target for no gain — the documents are ~16 KB
 * and written at commissioning time, not in any hot path.
 */

#include <cstddef>
#include <cstdint>

namespace ooe::pinled
{
    /// CRC-32/ISO-HDLC over @p len bytes at @p data.
    uint32_t crc32(const void *data, size_t len);

    /**
     * @brief Continue a CRC across non-contiguous buffers.
     *
     * Seed the first call with `crc32_begin()` and finish with `crc32_end()`.
     * Used when the header and payload are checksummed without being copied
     * into one buffer first.
     */
    uint32_t crc32_update(uint32_t running, const void *data, size_t len);

    /// Initial value for an incremental CRC.
    constexpr uint32_t crc32_begin() { return 0xFFFFFFFFu; }

    /// Final transform for an incremental CRC.
    constexpr uint32_t crc32_end(uint32_t running) { return running ^ 0xFFFFFFFFu; }
} // namespace ooe::pinled
