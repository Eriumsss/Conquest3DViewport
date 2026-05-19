// CorpusWriter.cpp
// =============================================================================
// SERIALIZE PRIORS TO DISK — V1 FORMAT, MAGIC-STAMPED, CRC-CHECKED
// =============================================================================
// Written by: Eriumsss
//
// Calls into CorpusPriors::serialize, prepends a CorpusHeader, computes
// a CRC over the body, and writes through Core::WriteAtomic so a crash
// mid-write does not poison the existing corpus file. Every step has
// a named failure helper because corpus-write goes wrong in eleven
// different fucked-up ways and the developer who hits one of those
// modes deserves a stack trace that names which one.
// =============================================================================

#include "CorpusFormatV1.h"
#include "CorpusPriors.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"
#include "../Core/PathUtils.h"
#include "../Schema/SchemaVersion.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace Vespucci {
namespace Corpus {

namespace {
    // CRC-32 ISO (reflected, init=0xFFFFFFFF, xorout=0xFFFFFFFF). The
    // table is generated lazily because corpus write is offline-only
    // and cold-start cost is irrelevant here.
    static u32 s_crcTable[256];
    static bool s_crcReady = false;
    void BurnTheCrcTableLazily() {
        if (s_crcReady) return;
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (i32 b = 0; b < 8; ++b) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            s_crcTable[i] = c;
        }
        s_crcReady = true;
    }

    bool ScreamAtNullPriorsAndBail(const CorpusPriors* p) {
        if (!p) {
            Core::Logging::Error("CorpusWriter: priors is NULL, refusing to write a goddamn empty file");
            return true;
        }
        return false;
    }
    bool ScreamAtMissingPathAndBail(const char* p) {
        if (!p || !*p) {
            Core::Logging::Error("CorpusWriter: empty path, this is what happens when somebody forgets to wire the UI");
            return true;
        }
        return false;
    }
} // namespace

u32 CrcCorpusBlob(const void* bytes, usize n) {
    BurnTheCrcTableLazily();
    const u8* p = (const u8*)bytes;
    u32 crc = 0xFFFFFFFFu;
    for (usize i = 0; i < n; ++i) {
        crc = s_crcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool WriteCorpusOrEatShit(const CorpusPriors& priors, const char* path) {
    if (ScreamAtNullPriorsAndBail(&priors)) return false;
    if (ScreamAtMissingPathAndBail(path)) return false;

    // 1) Serialize priors.
    usize bodyBytes = 0;
    void* body = priors.serialize(bodyBytes);
    if (!body || bodyBytes == 0) {
        Core::Logging::Error("CorpusWriter: priors.serialize returned nothing - empty corpus?");
        return false;
    }

    // 2) Build header.
    CorpusHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = kCorpusMagic;
    hdr.version       = kCorpusVersion;
    hdr.schemaVersion = Schema::PackSchemaVersion(Schema::CurrentSchemaVersion());
    hdr.createdAtUnix = (u32)std::time(0);
    // Fill counts from the body's first 6 u32s after its inner magic+version.
    const u32* bp = (const u32*)body;
    if (bp[0] == kCorpusMagic) {
        hdr.tripleCount      = bp[2];
        hdr.srcPairCount     = bp[3];
        hdr.eventPairCount   = bp[4];
        hdr.tripleTotalCount = bp[5];
        hdr.srcTotalCount    = bp[6];
        hdr.eventTotalCount  = bp[7];
    }
    // CRC over the body.
    hdr.crc32 = CrcCorpusBlob(body, bodyBytes);

    // 3) Concat header + body into one buffer for atomic write.
    usize totalBytes = sizeof(hdr) + bodyBytes;
    u8* out = (u8*)std::malloc(totalBytes);
    if (!out) {
        std::free(body);
        Core::Logging::Error("CorpusWriter: cannot malloc %zu bytes for blob, machine on its knees", totalBytes);
        return false;
    }
    std::memcpy(out, &hdr, sizeof(hdr));
    std::memcpy(out + sizeof(hdr), body, bodyBytes);
    std::free(body);

    // 4) Atomic write.
    bool ok = Core::WriteAtomic(path, out, (i64)totalBytes);
    std::free(out);
    if (!ok) {
        Core::Logging::Error("CorpusWriter: WriteAtomic failed for '%s'", path);
        return false;
    }
    Core::Logging::Info("Corpus written: %s (%zu bytes, %u triples)",
        path, totalBytes, (unsigned)hdr.tripleCount);
    return true;
}

} // namespace Corpus
} // namespace Vespucci
