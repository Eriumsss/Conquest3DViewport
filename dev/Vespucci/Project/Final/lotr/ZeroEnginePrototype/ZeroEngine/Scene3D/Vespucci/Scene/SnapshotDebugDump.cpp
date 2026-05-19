// SnapshotDebugDump.cpp
// =============================================================================
// HUMAN-READABLE SNAPSHOT DUMP — FOR WHEN THE GODDAMN BUG IS IN THE INDEXER
// =============================================================================
// Written by: Eriumsss
//
// When the ranker spits out a wrong suggestion and the user yells, we
// need to know the snapshot was sane at that moment. This dump writes
// a plaintext file with every entity row, every wire row, every layer.
// It is SLOW. Do not call from the hot path. Wire it to a debug menu
// item and a hotkey, nothing else.
// =============================================================================

#include "SceneSnapshot.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"
#include "../Core/PathUtils.h"

#include <cstdio>
#include <string>

namespace Vespucci {
namespace Scene {

namespace {
    void AppendStr(std::string& out, const Core::StringRef& s) {
        out.append(s.data(), s.size());
    }

    // Voice-ware helper: when the dump path cannot write, this is the
    // failure escalator. Logs Error AND surfaces a string the caller
    // can show in a UI toast.
    bool BlameDiskGodsAndBail(const char* path, std::string& outErr) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "snapshot dump: cannot write '%s' — disk full, locked, or path missing",
            path ? path : "(null)");
        outErr.assign(buf);
        Core::Logging::Error("%s", buf);
        return false;
    }
} // namespace

bool DumpSnapshotToFile(const SceneSnapshot& snap,
                        const char* path,
                        std::string& outErr)
{
    if (!path || !*path) return BlameDiskGodsAndBail("(empty)", outErr);

    std::string buf;
    buf.reserve(64 * 1024);

    char hdr[256];
    std::snprintf(hdr, sizeof(hdr),
        "Vespucci snapshot dump\n"
        "  version=%u  entities=%d  wires=%d  build_ms=%.3f\n"
        "================================================================\n\n",
        (unsigned)snap.version().raw,
        snap.entityCount(),
        snap.wireCount(),
        snap.lastBuildMs());
    buf += hdr;

    Span<EntityRow> rows = snap.entities();
    buf += "ENTITIES:\n";
    for (usize i = 0; i < rows.size(); ++i) {
        const EntityRow& r = rows[i];
        char line[512];
        std::snprintf(line, sizeof(line),
            "  [%5zu] guid=0x%08X parent=0x%08X layer=0x%08X type=0x%08X "
            "pos=(%.2f,%.2f,%.2f) gmm=0x%X traits=0x%X%s%s\n",
            i,
            (unsigned)r.guid.raw,
            (unsigned)r.parentGuid.raw,
            (unsigned)r.layerGuid.raw,
            (unsigned)r.typeId.raw,
            r.position.x, r.position.y, r.position.z,
            (unsigned)r.gamemodeMask,
            (unsigned)r.traits,
            r.deprecated ? " DEPRECATED" : "",
            r.isOutputEnvelope ? " OUTPUT" : "");
        buf += line;
        buf += "         name=\"";    AppendStr(buf, r.name);     buf += "\"\n";
        buf += "         typeName=\""; AppendStr(buf, r.typeName); buf += "\"\n";
    }

    buf += "\nWIRES:\n";
    Span<WireRow> wires = snap.wires();
    for (usize i = 0; i < wires.size(); ++i) {
        const WireRow& w = wires[i];
        char line[256];
        std::snprintf(line, sizeof(line),
            "  [%5zu] owner=%d output=%d target=%d outGuid=0x%08X tgtGuid=0x%08X "
            "delay=%.2f sticky=%d\n",
            i,
            w.ownerIdx.raw, w.outputIdx.raw, w.targetIdx.raw,
            (unsigned)w.outputGuid.raw, (unsigned)w.targetGuid.raw,
            w.delay, w.sticky ? 1 : 0);
        buf += line;
        buf += "         event=\"";   AppendStr(buf, w.eventName);  buf += "\" ";
        buf += "action=\"";           AppendStr(buf, w.actionName); buf += "\"\n";
    }

    if (!Core::WriteAtomic(path, buf.data(), (i64)buf.size())) {
        return BlameDiskGodsAndBail(path, outErr);
    }
    Core::Logging::Info("snapshot dumped to %s (%zu bytes)", path, buf.size());
    return true;
}

} // namespace Scene
} // namespace Vespucci
