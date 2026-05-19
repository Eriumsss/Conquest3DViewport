// CorpusReader.cpp
// =============================================================================
// LOAD CORPUS FROM DISK - VALIDATE MAGIC, VERSION, CRC; THEN HAND THE
// PRIORS' DESERIALIZER THE INNER BLOB. EVERY FAILURE MODE NAMED AFTER
// THE COWARD WHO COULD HAVE PREVENTED IT.
// =============================================================================
// Written by: Eriumsss

#include "CorpusFormatV1.h"
#include "CorpusPriors.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"

#include <cstring>

namespace Vespucci {
namespace Corpus {

namespace {
    bool RejectThisFuckingFileBecauseMagic(const CorpusHeader& h, const char* path) {
        if (h.magic != kCorpusMagic) {
            Core::Logging::Error("CorpusReader: '%s' has wrong magic 0x%08X (want 0x%08X) - "
                "either not a corpus file or someone fucked with the bytes",
                path, h.magic, kCorpusMagic);
            return true;
        }
        return false;
    }
    bool RejectThisFuckingFileBecauseVersion(const CorpusHeader& h, const char* path) {
        if (h.version != kCorpusVersion) {
            Core::Logging::Error("CorpusReader: '%s' is V%u, we only speak V%u - "
                "rebuild the corpus, you bitch",
                path, h.version, kCorpusVersion);
            return true;
        }
        return false;
    }
    bool RejectThisFuckingFileBecauseCrc(const CorpusHeader& h,
                                          const u8* body, usize bodyBytes,
                                          const char* path)
    {
        u32 c = CrcCorpusBlob(body, bodyBytes);
        if (c != h.crc32) {
            Core::Logging::Error("CorpusReader: '%s' CRC mismatch (have 0x%08X, expected 0x%08X) - "
                "file is fucking corrupt, refusing to load garbage into the ranker",
                path, c, h.crc32);
            return true;
        }
        return false;
    }
} // namespace

bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path) {
    if (!path || !*path) {
        Core::Logging::Warn("CorpusReader: empty path, skipping load");
        return false;
    }
    Core::Mapped m;
    if (!Core::MapReadOnly(path, m)) return false;

    bool ok = false;
    if (m.size >= (i64)sizeof(CorpusHeader)) {
        CorpusHeader hdr;
        std::memcpy(&hdr, m.data, sizeof(hdr));
        const u8* body = m.data + sizeof(hdr);
        usize bodyBytes = (usize)(m.size - (i64)sizeof(hdr));
        if (!RejectThisFuckingFileBecauseMagic(hdr, path) &&
            !RejectThisFuckingFileBecauseVersion(hdr, path) &&
            !RejectThisFuckingFileBecauseCrc(hdr, body, bodyBytes, path))
        {
            ok = outPriors.deserialize(body, bodyBytes);
            if (ok) {
                Core::Logging::Info("Corpus loaded from %s (triples=%u)",
                    path, (unsigned)hdr.tripleCount);
            } else {
                Core::Logging::Error("CorpusReader: '%s' header looked fine but priors.deserialize choked",
                    path);
            }
        }
    }
    Core::Unmap(m);
    return ok;
}

} // namespace Corpus
} // namespace Vespucci
