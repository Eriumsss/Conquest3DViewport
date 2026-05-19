// SceneSnapshot.cpp
// =============================================================================
// Build snapshot rows from ImGuiGlueFrameArgs. Single-threaded; runs
// inside the editor's frame between Update() and DrawPanel(). Hot path,
// budget < 0.5 ms for 1500-entity Helm's Deep size levels.
// =============================================================================
// Written by: Eriumsss

#include "SceneSnapshot.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"
#include "../Core/StringPool.h"
#include "../Core/VespucciAssert.h"
#include "../Schema/ZETypeRegistry.h"

// We can't include the host header here (it lives outside Vespucci/),
// so we forward-declare the bits we read. The actual struct definition
// is provided by the .cpp's translation unit through Scene3D/imgui_glue.h.
#include "imgui_glue.h"

#include <cstring>

namespace Vespucci {
namespace Scene {

namespace {
    static SceneSnapshot* s_global = 0;

    // Round up to next power of two, min 16.
    u32 NextPow2(u32 v) {
        if (v < 16) return 16;
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
        return v + 1;
    }

    void LowerInto(const char* s, char* out, usize cap) {
        if (!s) { out[0] = 0; return; }
        usize n = std::strlen(s);
        if (n + 1 > cap) n = cap - 1;
        for (usize i = 0; i < n; ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[i] = c;
        }
        out[n] = 0;
    }
} // namespace

SceneSnapshot* GlobalSnapshot()                   { return s_global; }
void           SetGlobalSnapshot(SceneSnapshot* s) { s_global = s; }

SceneSnapshot::SceneSnapshot()
    : m_version(SnapshotVersion(0)),
      m_guidMask(0),
      m_pool(0),
      m_lastBuildMs(0.0f)
{
    m_pool = new Core::StringPool();
}

SceneSnapshot::~SceneSnapshot() {
    delete (Core::StringPool*)m_pool;
}

void SceneSnapshot::clear() {
    m_entities.clear();
    m_wires.clear();
    m_guidBuckets.clear();
    m_guidMask = 0;
    ((Core::StringPool*)m_pool)->reset();
}

bool SceneSnapshot::build(const ImGuiGlueFrameArgs& args) {
    HEAT_SCOPE("SceneSnapshot::build");

    u64 t0 = Core::NowTicks();

    if (args.gameObjCount <= 0 || !args.gameObjGuids ||
        !args.gameObjNames || !args.gameObjTypeNames) {
        clear();
        return false;
    }

    // Reset and reserve.
    Core::StringPool* pool = (Core::StringPool*)m_pool;
    pool->reset();
    m_entities.clear();
    m_wires.clear();
    m_entities.reserve((size_t)args.gameObjCount);
    m_wires.reserve((size_t)args.gameObjOutputsTotal);

    buildEntityRows(args);
    buildWireRows(args);
    rebuildGuidIndex();

    m_version = m_version.next();

    u64 ticks = Core::NowTicks() - t0;
    m_lastBuildMs = (f32)Core::TicksToNs(ticks) / 1000000.0f;
    if (m_lastBuildMs > Limits::kSnapshotRebuildBudgetMs * 4.0f) {
        Core::Logging::Warn("SceneSnapshot: rebuild took %.2f ms (budget %.2f), corpus pressure",
            m_lastBuildMs, Limits::kSnapshotRebuildBudgetMs);
    }
    return true;
}

void SceneSnapshot::buildEntityRows(const ImGuiGlueFrameArgs& args) {
    Core::StringPool* pool = (Core::StringPool*)m_pool;
    Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();

    for (i32 i = 0; i < args.gameObjCount; ++i) {
        EntityRow r;
        r.guid             = Guid(args.gameObjGuids[i]);
        r.parentGuid       = Guid(args.gameObjParentGuids ? args.gameObjParentGuids[i] : 0u);
        r.layerGuid        = LayerGuid(args.gameObjLayerGuids ? args.gameObjLayerGuids[i] : 0u);
        r.gamemodeMask     = (u32)(args.gameObjGamemodeMasks ? args.gameObjGamemodeMasks[i] : 0);
        r.position.x       = args.gameObjPosX ? args.gameObjPosX[i] : 0.0f;
        r.position.y       = args.gameObjPosY ? args.gameObjPosY[i] : 0.0f;
        r.position.z       = args.gameObjPosZ ? args.gameObjPosZ[i] : 0.0f;

        const char* nm = args.gameObjNames[i];
        const char* tn = args.gameObjTypeNames[i];
        r.name     = pool->intern(nm ? nm : "");
        r.typeName = pool->intern(tn ? tn : "");

        // Resolve TypeId via registry. Lower the type name first.
        r.typeId   = TypeId(0);
        r.traits   = 0;
        r.deprecated = false;
        r.isOutputEnvelope = false;
        if (reg && tn) {
            char canon[128];
            LowerInto(tn, canon, sizeof(canon));
            const Schema::TypeRecord* rec = reg->findByCanonical(canon);
            if (rec) {
                r.typeId    = rec->id;
                r.traits    = rec->traits;
                r.deprecated = (rec->traits & (u32)Schema::TRAIT_DEPRECATED) != 0;
                r.isOutputEnvelope = (rec->traits & (u32)Schema::TRAIT_HasOutputs) != 0;
            }
        }

        m_entities.push_back(r);
    }
}

void SceneSnapshot::buildWireRows(const ImGuiGlueFrameArgs& args) {
    if (!args.gameObjOutputsData || !args.gameObjOutputsOffsets ||
        !args.gameObjOutputsCounts) return;

    Core::StringPool* pool = (Core::StringPool*)m_pool;

    // First pass: build a quick GUID -> index mapping for resolution.
    // We do it here instead of using rebuildGuidIndex() because we need
    // the lookup BEFORE the official guid table exists.
    std::vector<i32> tmpBuckets;
    u32 mask;
    {
        u32 want = NextPow2((u32)(m_entities.size() * 2 + 8));
        mask = want - 1;
        tmpBuckets.assign(want, -1);
        for (i32 i = 0; i < (i32)m_entities.size(); ++i) {
            u32 h = m_entities[(size_t)i].guid.raw;
            u32 idx = h & mask;
            while (tmpBuckets[idx] != -1) idx = (idx + 1) & mask;
            tmpBuckets[idx] = i;
        }
    }
    auto findIdx = [&](u32 guid) -> i32 {
        if (guid == 0) return -1;
        u32 idx = guid & mask;
        while (tmpBuckets[idx] != -1) {
            i32 e = tmpBuckets[idx];
            if (m_entities[(size_t)e].guid.raw == guid) return e;
            idx = (idx + 1) & mask;
        }
        return -1;
    };

    // Walk every entity's Outputs[] and emit a WireRow per non-zero slot.
    for (i32 ei = 0; ei < args.gameObjCount; ++ei) {
        i32 off = args.gameObjOutputsOffsets[ei];
        i32 cnt = args.gameObjOutputsCounts[ei];
        for (i32 k = 0; k < cnt; ++k) {
            u32 outGuid = args.gameObjOutputsData[off + k];
            if (outGuid == 0) continue;
            i32 outIdx = findIdx(outGuid);
            if (outIdx < 0) continue;

            u32 tgtGuid = args.gameObjTargetGuids ? args.gameObjTargetGuids[outIdx] : 0u;
            i32 tgtIdx  = findIdx(tgtGuid);

            WireRow w;
            w.ownerIdx  = EntityIndex(ei);
            w.outputIdx = EntityIndex(outIdx);
            w.targetIdx = EntityIndex(tgtIdx);
            w.outputGuid = Guid(outGuid);
            w.targetGuid = Guid(tgtGuid);
            const char* ev = args.gameObjOutputEvents ? args.gameObjOutputEvents[outIdx] : 0;
            const char* ac = args.gameObjInputEvents  ? args.gameObjInputEvents[outIdx]  : 0;
            char evLow[64], acLow[64];
            LowerInto(ev, evLow, sizeof(evLow));
            LowerInto(ac, acLow, sizeof(acLow));
            w.eventName  = pool->intern(evLow);
            w.actionName = pool->intern(acLow);
            w.delay      = args.gameObjDelays ? args.gameObjDelays[outIdx] : 0.0f;
            w.sticky     = args.gameObjSticky ? (args.gameObjSticky[outIdx] != 0) : false;
            m_wires.push_back(w);
        }
    }
}

void SceneSnapshot::rebuildGuidIndex() {
    u32 want = NextPow2((u32)(m_entities.size() * 2 + 8));
    m_guidMask = want - 1;
    m_guidBuckets.assign(want, -1);
    for (i32 i = 0; i < (i32)m_entities.size(); ++i) {
        u32 h = m_entities[(size_t)i].guid.raw;
        u32 idx = h & m_guidMask;
        while (m_guidBuckets[idx] != -1) idx = (idx + 1) & m_guidMask;
        m_guidBuckets[idx] = i;
    }
}

EntityIndex SceneSnapshot::indexForGuid(Guid g) const {
    if (!g.valid() || m_guidBuckets.empty()) return EntityIndex();
    u32 idx = g.raw & m_guidMask;
    while (m_guidBuckets[idx] != -1) {
        i32 e = m_guidBuckets[idx];
        if (m_entities[(size_t)e].guid == g) return EntityIndex(e);
        idx = (idx + 1) & m_guidMask;
    }
    return EntityIndex();
}

i32 SceneSnapshot::entityCount() const { return (i32)m_entities.size(); }
i32 SceneSnapshot::wireCount()   const { return (i32)m_wires.size(); }

const EntityRow* SceneSnapshot::entityAt(EntityIndex i) const {
    if (!i.valid() || i.raw >= (i32)m_entities.size()) return 0;
    return &m_entities[(size_t)i.raw];
}
const WireRow* SceneSnapshot::wireAt(WireIndex i) const {
    if (!i.valid() || i.raw >= (i32)m_wires.size()) return 0;
    return &m_wires[(size_t)i.raw];
}

Span<EntityRow> SceneSnapshot::entities() const {
    return Span<EntityRow>(m_entities.empty() ? 0 : &m_entities[0], m_entities.size());
}
Span<WireRow> SceneSnapshot::wires() const {
    return Span<WireRow>(m_wires.empty() ? 0 : &m_wires[0], m_wires.size());
}

} // namespace Scene
} // namespace Vespucci
