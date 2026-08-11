/**
 * @file test_doc_file.cpp
 * @brief Stored documents as real files (FR-CFG-7/15).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * These tests write to an actual filesystem. That is the point: `test_doc_frame`
 * covers the framing against buffers, and this covers what happens when the
 * buffer is a file that might be truncated, replaced, absent, or left behind by
 * an interrupted write.
 *
 * The one thing it cannot cover is that LittleFS's rename on real flash behaves
 * like POSIX's. That is exercised on the device by the store selftest
 * (`CONFIG_PINLED_STORE_SELFTEST`), because it is a property of the medium and
 * no amount of host testing can stand in for it.
 */

#include "harness.h"

#include "pinled_doc_file.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

using namespace ooe::pinled;

namespace
{
    /// Deliberately RELATIVE to the test's working directory.
    ///
    /// The first version of this file used an absolute path handed down from
    /// CMake, and every write failed: `kMaxDocPath` is 96 bytes and a build
    /// directory nested a few levels deep exceeds that on its own. The cap is
    /// correct — device paths are "/cfg/install.pb" — so the fix belongs here.
    /// Left as a comment because the same trap is waiting for the next person
    /// who adds a file-touching test.
    constexpr char kScratch[] = "scratch";

    std::string path_for(const char *name)
    {
        return std::string(kScratch) + "/" + name;
    }

    /// A distinct file per case, so a failure leaves evidence that no later
    /// case has overwritten.
    std::string fresh(const char *name)
    {
        const std::string p = path_for(name);
        std::remove(p.c_str());
        std::remove((p + ".tmp").c_str());
        return p;
    }

    std::vector<uint8_t> payload(size_t n, uint8_t seed = 0)
    {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; ++i)
            v[i] = static_cast<uint8_t>(seed + i * 7u + 1u);
        return v;
    }

    bool write_raw(const std::string &p, const uint8_t *data, size_t n)
    {
        FILE *f = std::fopen(p.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(data, 1, n, f) == n;
        std::fclose(f);
        return ok;
    }

    std::vector<uint8_t> read_raw(const std::string &p)
    {
        std::vector<uint8_t> out;
        FILE *f = std::fopen(p.c_str(), "rb");
        if (!f)
            return out;
        uint8_t chunk[256];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            out.insert(out.end(), chunk, chunk + n);
        std::fclose(f);
        return out;
    }
} // namespace

// ------------------------------------------------------------- round trip --

TEST(a_written_document_reads_back_identically)
{
    const std::string p = fresh("roundtrip.pb");
    const std::vector<uint8_t> in = payload(300);

    REQUIRE(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
            FileStatus::Ok);

    uint8_t buf[1024];
    DocKind kind = DocKind::Unknown;
    const uint8_t *out = nullptr;
    size_t len = 0;
    REQUIRE(doc_file_read(p.c_str(), buf, sizeof(buf), &kind, &out, &len) == FileStatus::Ok);

    CHECK(kind == DocKind::MachineProfile);
    CHECK_EQ(len, in.size());
    CHECK(std::memcmp(out, in.data(), in.size()) == 0);
}

TEST(an_empty_payload_round_trips)
{
    // Legitimate: an install config with every field defaulted encodes to zero
    // bytes in proto3, and must not be mistaken for a missing file.
    const std::string p = fresh("empty.pb");
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, nullptr, 0) == FileStatus::Ok);

    uint8_t buf[64];
    DocKind kind = DocKind::Unknown;
    const uint8_t *out = nullptr;
    size_t len = 1;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), &kind, &out, &len) == FileStatus::Ok);
    CHECK(kind == DocKind::InstallConfig);
    CHECK_EQ(len, static_cast<size_t>(0));
}

TEST(writing_twice_replaces_rather_than_appends)
{
    const std::string p = fresh("replace.pb");
    const std::vector<uint8_t> first = payload(400, 1);
    const std::vector<uint8_t> second = payload(50, 9);

    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, first.data(), first.size()) ==
          FileStatus::Ok);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, second.data(), second.size()) ==
          FileStatus::Ok);

    CHECK_EQ(read_raw(p).size(), doc_frame_size(second.size()));

    uint8_t buf[1024];
    const uint8_t *out = nullptr;
    size_t len = 0;
    REQUIRE(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, &out, &len) == FileStatus::Ok);
    CHECK_EQ(len, second.size());
    CHECK(std::memcmp(out, second.data(), second.size()) == 0);
}

TEST(a_successful_write_leaves_no_temporary_behind)
{
    const std::string p = fresh("notmp.pb");
    const std::vector<uint8_t> in = payload(64);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, in.data(), in.size()) ==
          FileStatus::Ok);
    CHECK(!doc_file_exists((p + ".tmp").c_str()));
    CHECK(doc_file_exists(p.c_str()));
}

// ------------------------------------------------------- absence vs faults --

TEST(a_missing_file_is_not_found_and_not_an_error)
{
    // THE case this enum shape exists for. A device that has never been
    // configured hits this on every boot, and it must be distinguishable from
    // a fault or the store boots looking healthy when it is broken (FR-CFG-4).
    const std::string p = fresh("absent.pb");
    uint8_t buf[64];
    DocStatus ds = DocStatus::Ok;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, nullptr, &ds) ==
          FileStatus::NotFound);
}

TEST(doc_file_exists_agrees_with_the_reader)
{
    const std::string p = fresh("exists.pb");
    CHECK(!doc_file_exists(p.c_str()));
    const std::vector<uint8_t> in = payload(8);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, in.data(), in.size()) ==
          FileStatus::Ok);
    CHECK(doc_file_exists(p.c_str()));
}

// ------------------------------------------------------------- corruption --

TEST(a_truncated_file_is_rejected_with_a_reason)
{
    // The exact failure the frame exists for: the write did not finish.
    const std::string p = fresh("truncated.pb");
    const std::vector<uint8_t> in = payload(200);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
          FileStatus::Ok);

    std::vector<uint8_t> raw = read_raw(p);
    raw.resize(raw.size() - 40);
    CHECK(write_raw(p, raw.data(), raw.size()));

    uint8_t buf[1024];
    DocStatus ds = DocStatus::Ok;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, nullptr, &ds) ==
          FileStatus::BadDocument);
    CHECK(ds == DocStatus::BadLength);
}

TEST(a_flipped_payload_bit_is_caught_by_the_crc)
{
    const std::string p = fresh("flipped.pb");
    const std::vector<uint8_t> in = payload(120);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
          FileStatus::Ok);

    std::vector<uint8_t> raw = read_raw(p);
    raw[kDocHeaderSize + 60] ^= 0x01u;
    CHECK(write_raw(p, raw.data(), raw.size()));

    uint8_t buf[1024];
    DocStatus ds = DocStatus::Ok;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, nullptr, &ds) ==
          FileStatus::BadDocument);
    CHECK(ds == DocStatus::BadCrc);
}

TEST(a_file_that_is_not_a_document_is_rejected)
{
    const std::string p = fresh("garbage.pb");
    const uint8_t junk[] = "this is not a pinled document, it is a text file";
    CHECK(write_raw(p, junk, sizeof(junk)));

    uint8_t buf[128];
    DocStatus ds = DocStatus::Ok;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, nullptr, &ds) ==
          FileStatus::BadDocument);
    CHECK(ds == DocStatus::BadMagic);
}

TEST(an_empty_file_is_rejected_not_read_as_an_empty_document)
{
    // A zero-length file is what an interrupted create leaves. It is NOT the
    // same as a document with an empty payload, which still has its 16-byte
    // header — and conflating them would boot on a truncated write.
    const std::string p = fresh("zero.pb");
    CHECK(write_raw(p, nullptr, 0));

    uint8_t buf[64];
    DocStatus ds = DocStatus::Ok;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, nullptr, &ds) ==
          FileStatus::BadDocument);
    CHECK(ds == DocStatus::ShortBuffer);
}

// ------------------------------------------------------------------ bounds --

TEST(a_file_larger_than_the_buffer_is_too_large_not_truncated)
{
    // Silently reading the first N bytes would produce a document that fails
    // its CRC, which reports the wrong cause. Worse, a shorter payload could
    // pass — so this must be its own answer.
    const std::string p = fresh("big.pb");
    const std::vector<uint8_t> in = payload(500);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
          FileStatus::Ok);

    uint8_t small[128];
    CHECK(doc_file_read(p.c_str(), small, sizeof(small), nullptr, nullptr, nullptr) ==
          FileStatus::TooLarge);
}

TEST(a_buffer_exactly_the_size_of_the_document_is_enough)
{
    // The boundary next to the case above: exactly-fits must NOT read as too
    // large, or the buffer sized from the schema would be one byte short of
    // every maximal document.
    const std::string p = fresh("exact.pb");
    const std::vector<uint8_t> in = payload(97);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
          FileStatus::Ok);

    std::vector<uint8_t> buf(doc_frame_size(in.size()));
    size_t len = 0;
    CHECK(doc_file_read(p.c_str(), buf.data(), buf.size(), nullptr, nullptr, &len) ==
          FileStatus::Ok);
    CHECK_EQ(len, in.size());
}

TEST(a_payload_past_the_ceiling_is_refused_rather_than_written)
{
    const std::string p = fresh("toobig.pb");
    const std::vector<uint8_t> in(kMaxDocBytes + 1, 0xAAu);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
          FileStatus::WriteError);
    CHECK(!doc_file_exists(p.c_str()));
}

TEST(the_write_ceiling_and_the_read_ceiling_are_the_same_ceiling)
{
    // Both are kMaxDocBytes, but one used to bound the payload and the other
    // the whole frame — which left a 16-byte band of documents that could be
    // written and then never read back. The largest writable document must be
    // readable, and one byte more must be refused.
    const std::string p = fresh("ceiling.pb");
    const std::vector<uint8_t> biggest(kMaxDocBytes - kDocHeaderSize, 0x5Au);

    REQUIRE(doc_file_write(p.c_str(), DocKind::MachineProfile,
                           biggest.data(), biggest.size()) == FileStatus::Ok);

    std::vector<uint8_t> buf(kMaxDocBytes);
    size_t len = 0;
    CHECK(doc_file_read(p.c_str(), buf.data(), buf.size(), nullptr, nullptr, &len) ==
          FileStatus::Ok);
    CHECK_EQ(len, biggest.size());

    const std::vector<uint8_t> one_more(biggest.size() + 1, 0x5Au);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile,
                         one_more.data(), one_more.size()) == FileStatus::WriteError);
}

TEST(a_missing_file_does_not_report_a_framing_verdict)
{
    // doc_status was pre-set to ShortBuffer, so every "absent" log line quoted
    // a framing error that had never been evaluated.
    const std::string p = fresh("nostatus.pb");
    DocStatus ds = DocStatus::BadCrc;
    uint8_t buf[64];
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, nullptr, &ds) ==
          FileStatus::NotFound);
    CHECK(ds == DocStatus::Ok);
}

TEST(a_path_too_long_for_a_temporary_is_refused)
{
    std::string p = path_for("x");
    p.append(kMaxDocPath, 'y');
    const std::vector<uint8_t> in = payload(8);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, in.data(), in.size()) ==
          FileStatus::WriteError);
}

// ----------------------------------------------------------- the atomicity --

TEST(a_failed_write_leaves_the_previous_document_intact)
{
    // The entire reason for write-then-rename. A configuration that survived
    // being overwritten badly is worth more than the one that failed to land.
    const std::string p = fresh("survivor.pb");
    const std::vector<uint8_t> good = payload(150, 3);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, good.data(), good.size()) ==
          FileStatus::Ok);

    // Fails at fopen: the temporary would have to live in a directory that is
    // not there. Nothing touches the destination before that point.
    const std::string bad = path_for("nonexistent-dir/doc.pb");
    const std::vector<uint8_t> other = payload(20, 8);
    CHECK(doc_file_write(bad.c_str(), DocKind::InstallConfig, other.data(), other.size()) ==
          FileStatus::WriteError);

    uint8_t buf[1024];
    const uint8_t *out = nullptr;
    size_t len = 0;
    REQUIRE(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, &out, &len) == FileStatus::Ok);
    CHECK_EQ(len, good.size());
    CHECK(std::memcmp(out, good.data(), good.size()) == 0);
}

TEST(a_leftover_temporary_from_an_interrupted_write_is_ignored)
{
    // What a power cut mid-write actually leaves on the medium: a stale .tmp
    // beside a good document. The document must still read, and the next write
    // must overwrite the temporary rather than trip over it.
    const std::string p = fresh("leftover.pb");
    const std::vector<uint8_t> good = payload(80, 5);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, good.data(), good.size()) ==
          FileStatus::Ok);

    const uint8_t junk[] = {0x50, 0x4C, 0x44, 0x31, 0xFF, 0xFF};
    CHECK(write_raw(p + ".tmp", junk, sizeof(junk)));

    uint8_t buf[1024];
    size_t len = 0;
    CHECK(doc_file_read(p.c_str(), buf, sizeof(buf), nullptr, nullptr, &len) == FileStatus::Ok);
    CHECK_EQ(len, good.size());

    const std::vector<uint8_t> next = payload(90, 6);
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, next.data(), next.size()) ==
          FileStatus::Ok);
    CHECK(!doc_file_exists((p + ".tmp").c_str()));
}

TEST(the_document_is_never_partially_visible_under_its_own_name)
{
    // Stated as an invariant rather than a race: at no point does `path` hold
    // anything but a complete document, because the only write to that name is
    // the rename. Checked by asserting the destination does not exist until the
    // write returns, on a name that has never been used.
    const std::string p = fresh("atomic.pb");
    CHECK(!doc_file_exists(p.c_str()));

    const std::vector<uint8_t> in = payload(1000);
    CHECK(doc_file_write(p.c_str(), DocKind::MachineProfile, in.data(), in.size()) ==
          FileStatus::Ok);

    const std::vector<uint8_t> raw = read_raw(p);
    CHECK_EQ(raw.size(), doc_frame_size(in.size()));
}

// ------------------------------------------------------------- null safety --

TEST(null_arguments_are_refused_rather_than_dereferenced)
{
    uint8_t buf[32];
    CHECK(doc_file_read(nullptr, buf, sizeof(buf), nullptr, nullptr, nullptr) ==
          FileStatus::ReadError);
    CHECK(doc_file_read("x", nullptr, 32, nullptr, nullptr, nullptr) == FileStatus::ReadError);
    CHECK(doc_file_read("x", buf, 0, nullptr, nullptr, nullptr) == FileStatus::ReadError);
    CHECK(doc_file_write(nullptr, DocKind::InstallConfig, buf, 1) == FileStatus::WriteError);
    CHECK(!doc_file_exists(nullptr));

    const std::string p = fresh("nullpay.pb");
    CHECK(doc_file_write(p.c_str(), DocKind::InstallConfig, nullptr, 8) ==
          FileStatus::WriteError);
}

int main()
{
    // Real files need somewhere to live. Created here rather than by CMake so
    // the suite works however it is invoked, including straight from a shell.
    ::mkdir(kScratch, 0755);
    return ooe::test::run_all();
}
