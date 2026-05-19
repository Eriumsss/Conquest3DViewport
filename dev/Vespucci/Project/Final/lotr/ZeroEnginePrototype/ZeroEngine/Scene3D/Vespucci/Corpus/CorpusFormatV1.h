// CorpusFormatV1.h
// =============================================================================
// CORPUS BINARY FORMAT V1 - LITTLE-ENDIAN, MAGIC-STAMPED, CRC-CHECKED
// =============================================================================
// Written by: Eriumsss
//
// We could store priors as JSON. We do not, because the corpus is
// hot-loaded at editor startup and JSON parse on a 50 MB file would
// stall the boot for half a goddamn second per launch, every launch.
// Binary format with mmap path means the OS pages bytes in lazily,
// editor stays responsive, and the global corpus tool can shard out
// per-map and merge later without re-parsing.
//
// Layout (little-endian throughout):
//   u32 magic     = 'V'<<24 | 'E'<<16 | 'S'<<8 | 'P'      = 0x56455350
//   u32 version   = 1
//   u32 schemaVer = (Schema::CurrentSchemaVersion() packed)
//   u32 createdAt = unix seconds at write time
//   u32 entryCount[6]                                       (six histogram tables)
//   u32 crc32                                                (over EVERYTHING after this field)
//   <table 0..5 in order, contiguous>
//
// Anyone who fucks with this layout without bumping `version` deserves
// the runtime crash they get. Please do not be that asshole.
// =============================================================================

#ifndef VESPUCCI_CORPUS_CORPUSFORMATV1_H_
#define VESPUCCI_CORPUS_CORPUSFORMATV1_H_

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Corpus {

static const u32 kCorpusMagic   = 0x56455350u;  // "VESP"
static const u32 kCorpusVersion = 1;

struct CorpusHeader {
    u32 magic;
    u32 version;
    u32 schemaVersion;
    u32 createdAtUnix;
    u32 tripleCount;       // srcEventToTarget
    u32 srcPairCount;      // srcToTarget
    u32 eventPairCount;    // eventToTarget
    u32 tripleTotalCount;  // srcEventTotal
    u32 srcTotalCount;
    u32 eventTotalCount;
    u32 crc32;             // over header.contents (everything after this field)
};

// CRC-32 ISO routine over a byte buffer. Lives in CorpusWriter.cpp /
// CorpusReader.cpp; declared here so any consumer can spot-check.
u32 CrcCorpusBlob(const void* bytes, usize n);

} // namespace Corpus
} // namespace Vespucci

#endif // VESPUCCI_CORPUS_CORPUSFORMATV1_H_
