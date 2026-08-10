/**
 * @file pinled_doc_frame.cpp
 * @brief On-disk framing for stored configuration documents.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_doc_frame.h"

#include "pinled_crc32.h"

namespace ooe::pinled
{
    namespace
    {
        constexpr uint8_t kMagic[4] = {'P', 'L', 'D', '1'};
        constexpr uint8_t kFrameVersion = 1;

        void put_u32le(uint8_t *p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v & 0xFFu);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        }

        uint32_t get_u32le(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }
    } // namespace

    DocStatus doc_frame_write(uint8_t *out, size_t out_cap,
                              DocKind kind,
                              const uint8_t *payload, size_t len,
                              size_t *written)
    {
        const size_t total = doc_frame_size(len);
        if (out == nullptr || out_cap < total)
            return DocStatus::ShortBuffer;
        if (payload == nullptr && len != 0)
            return DocStatus::ShortBuffer;

        out[0] = kMagic[0];
        out[1] = kMagic[1];
        out[2] = kMagic[2];
        out[3] = kMagic[3];
        out[4] = kFrameVersion;
        out[5] = static_cast<uint8_t>(kind);
        out[6] = 0;
        out[7] = 0;
        put_u32le(out + 8, static_cast<uint32_t>(len));
        put_u32le(out + 12, crc32(payload, len));

        for (size_t i = 0; i < len; ++i)
            out[kDocHeaderSize + i] = payload[i];

        if (written != nullptr)
            *written = total;
        return DocStatus::Ok;
    }

    DocStatus doc_frame_read(const uint8_t *in, size_t len,
                             DocKind *kind,
                             const uint8_t **payload, size_t *payload_len)
    {
        if (in == nullptr || len < kDocHeaderSize)
            return DocStatus::ShortBuffer;

        if (in[0] != kMagic[0] || in[1] != kMagic[1] ||
            in[2] != kMagic[2] || in[3] != kMagic[3])
            return DocStatus::BadMagic;

        if (in[4] != kFrameVersion)
            return DocStatus::BadVersion;

        const uint32_t declared = get_u32le(in + 8);

        // Validate the length against what we actually hold BEFORE it is used
        // to bound anything. A corrupt length is the one field that can turn a
        // bad read into an out-of-bounds one.
        if (declared > len - kDocHeaderSize)
            return DocStatus::BadLength;

        const uint8_t *body = in + kDocHeaderSize;
        if (crc32(body, declared) != get_u32le(in + 12))
            return DocStatus::BadCrc;

        if (kind != nullptr)
            *kind = static_cast<DocKind>(in[5]);
        if (payload != nullptr)
            *payload = body;
        if (payload_len != nullptr)
            *payload_len = declared;
        return DocStatus::Ok;
    }

    const char *doc_status_str(DocStatus s)
    {
        switch (s)
        {
        case DocStatus::Ok:          return "ok";
        case DocStatus::ShortBuffer: return "short buffer";
        case DocStatus::BadMagic:    return "bad magic";
        case DocStatus::BadVersion:  return "bad frame version";
        case DocStatus::BadLength:   return "bad length";
        case DocStatus::BadCrc:      return "bad crc";
        }
        return "unknown";
    }
} // namespace ooe::pinled
