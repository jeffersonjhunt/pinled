/**
 * @file pinled_crc32.cpp
 * @brief CRC-32/ISO-HDLC, nibble-table implementation.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * A 16-entry nibble table rather than the usual 256-entry byte table: 64 bytes
 * of rodata instead of 1 KB, for two table lookups per byte instead of one.
 * The documents this runs over are ~16 KB and are checksummed at boot and on
 * save, so the throughput difference is invisible and the flash is not.
 */

#include "pinled_crc32.h"

namespace ooe::pinled
{
    namespace
    {
        /// Reflected CRC-32 polynomial 0xEDB88320, low nibble first.
        constexpr uint32_t kNibble[16] = {
            0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
            0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
            0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
            0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    } // namespace

    uint32_t crc32_update(uint32_t running, const void *data, size_t len)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        if (p == nullptr)
            return running;

        for (size_t i = 0; i < len; ++i)
        {
            running ^= p[i];
            running = (running >> 4) ^ kNibble[running & 0x0Fu];
            running = (running >> 4) ^ kNibble[running & 0x0Fu];
        }
        return running;
    }

    uint32_t crc32(const void *data, size_t len)
    {
        return crc32_end(crc32_update(crc32_begin(), data, len));
    }
} // namespace ooe::pinled
