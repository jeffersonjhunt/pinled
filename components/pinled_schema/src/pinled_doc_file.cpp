/**
 * @file pinled_doc_file.cpp
 * @brief Reading and writing stored documents as files (FR-CFG-7/15).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_doc_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <unistd.h> // fsync

namespace ooe::pinled
{
    namespace
    {
        constexpr char kTmpSuffix[] = ".tmp";

        /// Build "<path>.tmp" into @p out. False if it would not fit, which is
        /// checked rather than truncated — a silently shortened temporary path
        /// could collide with a real document.
        bool tmp_path(char *out, size_t cap, const char *path)
        {
            const size_t n = std::strlen(path);
            const size_t suffix = sizeof(kTmpSuffix) - 1;
            if (n + suffix + 1 > cap)
                return false;
            std::memcpy(out, path, n);
            std::memcpy(out + n, kTmpSuffix, suffix + 1); // includes the NUL
            return true;
        }
    } // namespace

    FileStatus doc_file_read(const char *path,
                             uint8_t *buf, size_t cap,
                             DocKind *kind,
                             const uint8_t **payload, size_t *payload_len,
                             DocStatus *doc_status)
    {
        // Ok until a framing check actually says otherwise. Pre-setting a
        // verdict here would have every "file not found" log line quote a
        // framing error that was never evaluated.
        if (doc_status != nullptr)
            *doc_status = DocStatus::Ok;
        if (path == nullptr || buf == nullptr || cap == 0)
            return FileStatus::ReadError;

        FILE *f = std::fopen(path, "rb");
        if (f == nullptr)
            return (errno == ENOENT) ? FileStatus::NotFound : FileStatus::ReadError;

        // One ceiling covering both bounds: the caller's buffer and the sanity
        // limit. Reading up to it and then probing for one more byte is how a
        // file that is too large is distinguished from one that exactly fills
        // the buffer — a length from the filesystem would be a second source of
        // truth about the same thing.
        const size_t limit = (cap < kMaxDocBytes) ? cap : kMaxDocBytes;

        size_t total = 0;
        while (total < limit)
        {
            const size_t n = std::fread(buf + total, 1, limit - total, f);
            if (n == 0)
                break;
            total += n;
        }

        bool overflowed = false;
        if (total == limit)
        {
            uint8_t probe = 0;
            overflowed = std::fread(&probe, 1, 1, f) == 1;
        }

        const bool io_error = std::ferror(f) != 0;
        std::fclose(f);

        if (io_error)
            return FileStatus::ReadError;
        if (overflowed)
            return FileStatus::TooLarge;

        const DocStatus s = doc_frame_read(buf, total, kind, payload, payload_len);
        if (doc_status != nullptr)
            *doc_status = s;
        return (s == DocStatus::Ok) ? FileStatus::Ok : FileStatus::BadDocument;
    }

    FileStatus doc_file_write(const char *path, DocKind kind,
                              const uint8_t *payload, size_t len)
    {
        // Bounded by the FRAME, not the payload, so this means the same thing
        // as the ceiling doc_file_read applies. Bounding the payload here would
        // let a document be written and then never read back.
        if (path == nullptr || doc_frame_size(len) > kMaxDocBytes)
            return FileStatus::WriteError;

        uint8_t header[kDocHeaderSize];
        if (doc_header_write(header, kind, payload, len) != DocStatus::Ok)
            return FileStatus::WriteError;

        char tmp[kMaxDocPath];
        if (!tmp_path(tmp, sizeof(tmp), path))
            return FileStatus::WriteError;

        FILE *f = std::fopen(tmp, "wb");
        if (f == nullptr)
            return FileStatus::WriteError;

        bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
        if (ok && len != 0)
            ok = std::fwrite(payload, 1, len, f) == len;

        // Flush through the library AND the filesystem before the rename. The
        // rename is only atomic with respect to bytes that have actually
        // landed; without this the new file could be the empty one.
        if (ok)
            ok = std::fflush(f) == 0;
        if (ok)
            ok = ::fsync(::fileno(f)) == 0;

        if (std::fclose(f) != 0)
            ok = false;

        if (!ok)
        {
            std::remove(tmp); // leave the destination as it was
            return FileStatus::WriteError;
        }

        if (std::rename(tmp, path) != 0)
        {
            std::remove(tmp);
            return FileStatus::WriteError;
        }
        return FileStatus::Ok;
    }

    bool doc_file_exists(const char *path)
    {
        if (path == nullptr)
            return false;
        FILE *f = std::fopen(path, "rb");
        if (f == nullptr)
            return false;
        std::fclose(f);
        return true;
    }

    const char *file_status_str(FileStatus s)
    {
        switch (s)
        {
        case FileStatus::Ok:          return "ok";
        case FileStatus::NotFound:    return "not found";
        case FileStatus::TooLarge:    return "too large";
        case FileStatus::ReadError:   return "read error";
        case FileStatus::WriteError:  return "write error";
        case FileStatus::BadDocument: return "bad document";
        }
        return "unknown";
    }
} // namespace ooe::pinled
