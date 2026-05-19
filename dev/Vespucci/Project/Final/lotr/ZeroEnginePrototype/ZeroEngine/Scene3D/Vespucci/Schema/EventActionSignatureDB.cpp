// EventActionSignatureDB.cpp
// =============================================================================
// Per-type event / action signature database. Hash-indexed by TypeId.
// Initialized from a built-in table embedded below; the corpus builder
// piles real-world frequency counts on top each time a level loads.
// =============================================================================
// Written by: Eriumsss

#include "EventActionSignatureDB.h"
#include "ZETypeRegistry.h"

#include "../Core/Arena.h"
#include "../Core/Hash.h"
#include "../Core/Logging.h"
#include "../Core/StringPool.h"
#include "../Core/VespucciAssert.h"

#include <cstdlib>
#include <cstring>

namespace Vespucci {
namespace Schema {

namespace {
    static const i32 kInitialBuckets = 256;
    static const i32 kInitialEntryCap = 16;
    static EventActionSignatureDB* s_global = 0;

    bool StrEqIgnoreCase(const char* a, const char* b) {
        if (!a || !b) return a == b;
        while (*a && *b) {
            char ca = *a; char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) return false;
            a++; b++;
        }
        return *a == 0 && *b == 0;
    }
} // namespace

EventActionSignatureDB* GlobalSignatureDB()                     { return s_global; }
void                    SetGlobalSignatureDB(EventActionSignatureDB* db) { s_global = db; }

EventActionSignatureDB::EventActionSignatureDB()
    : m_buckets(0), m_bucketCount(0), m_bucketMask(0),
      m_sigs(0), m_sigCount(0), m_sigCap(0),
      m_arena(0), m_pool(0)
{
    m_arena = new Core::Arena(64 * 1024);
    m_pool  = new Core::StringPool();

    m_bucketCount = kInitialBuckets;
    m_bucketMask  = m_bucketCount - 1;
    usize bytes = sizeof(Bucket) * (size_t)m_bucketCount;
    m_buckets = (Bucket*)std::malloc(bytes);
    VESPUCCI_ASSERT(m_buckets != 0, "SignatureDB initial bucket malloc failed");
    for (i32 i = 0; i < m_bucketCount; ++i) {
        m_buckets[i].id     = TypeId(0);
        m_buckets[i].sigIdx = -1;
    }

    m_sigCap = 256;
    m_sigs   = (TypeSignature*)std::malloc(sizeof(TypeSignature) * (size_t)m_sigCap);
    VESPUCCI_ASSERT(m_sigs != 0, "SignatureDB initial sig malloc failed");
}

EventActionSignatureDB::~EventActionSignatureDB() {
    if (m_buckets) std::free(m_buckets);
    if (m_sigs)    std::free(m_sigs);
    delete (Core::Arena*)m_arena;
    delete (Core::StringPool*)m_pool;
}

i32 EventActionSignatureDB::probe(TypeId id) const {
    i32 idx = (i32)(id.raw & (u32)m_bucketMask);
    while (m_buckets[idx].sigIdx != -1) {
        if (m_buckets[idx].id == id) return idx;
        idx = (idx + 1) & m_bucketMask;
    }
    return idx;
}

void EventActionSignatureDB::grow() {
    i32 oldCount = m_bucketCount;
    Bucket* old = m_buckets;
    m_bucketCount = oldCount * 2;
    m_bucketMask  = m_bucketCount - 1;
    m_buckets = (Bucket*)std::malloc(sizeof(Bucket) * (size_t)m_bucketCount);
    VESPUCCI_ASSERT(m_buckets != 0, "SignatureDB grow malloc failed");
    for (i32 i = 0; i < m_bucketCount; ++i) {
        m_buckets[i].id     = TypeId(0);
        m_buckets[i].sigIdx = -1;
    }
    for (i32 i = 0; i < oldCount; ++i) {
        if (old[i].sigIdx == -1) continue;
        i32 idx = (i32)(old[i].id.raw & (u32)m_bucketMask);
        while (m_buckets[idx].sigIdx != -1) {
            idx = (idx + 1) & m_bucketMask;
        }
        m_buckets[idx] = old[i];
    }
    std::free(old);
}

TypeSignature& EventActionSignatureDB::ensureSignature(TypeId id) {
    i32 bucketIdx = probe(id);
    if (m_buckets[bucketIdx].sigIdx != -1) {
        return m_sigs[m_buckets[bucketIdx].sigIdx];
    }

    if ((m_sigCount + 1) * 4 > m_bucketCount * 3) {
        grow();
        bucketIdx = probe(id);
    }
    if (m_sigCount >= m_sigCap) {
        m_sigCap *= 2;
        m_sigs = (TypeSignature*)std::realloc(m_sigs,
                                              sizeof(TypeSignature) * (size_t)m_sigCap);
        VESPUCCI_ASSERT(m_sigs != 0, "SignatureDB sig realloc failed");
    }

    TypeSignature& sig = m_sigs[m_sigCount];
    sig.typeId          = id;
    sig.emittedEvents   = 0;
    sig.emittedCount    = 0;
    sig.acceptedActions = 0;
    sig.acceptedCount   = 0;

    m_buckets[bucketIdx].id     = id;
    m_buckets[bucketIdx].sigIdx = m_sigCount;
    m_sigCount++;
    return sig;
}

SignatureEntry* EventActionSignatureDB::findEntry(SignatureEntry* arr, i32 count,
                                                   const char* canonicalName) const
{
    if (!arr || !canonicalName) return 0;
    for (i32 i = 0; i < count; ++i) {
        if (arr[i].name.size() == std::strlen(canonicalName) &&
            std::memcmp(arr[i].name.data(), canonicalName, arr[i].name.size()) == 0)
        {
            return &arr[i];
        }
    }
    return 0;
}

void EventActionSignatureDB::appendEntry(SignatureEntry*& arr, i32& count, i32& cap,
                                          const char* canonical, const char* display,
                                          const char* doc, u32 freq)
{
    // First call into a fresh TypeSignature has arr == NULL even though
    // `cap` arrives at 16 from the caller's NextCap(0) — caller's cap is
    // the *target* capacity, not the *current* one. Allocate when arr is
    // null, even if count+1 fits the target cap. Skip this and the next
    // line writes to NULL and we crater the editor on Init.
    if (!arr || count + 1 > cap) {
        i32 newCap = (count + 1 > cap) ? cap * 2 : cap;
        if (newCap < kInitialEntryCap) newCap = kInitialEntryCap;
        SignatureEntry* fresh = (SignatureEntry*)((Core::Arena*)m_arena)->alloc(
            sizeof(SignatureEntry) * (size_t)newCap, alignof(SignatureEntry));
        VESPUCCI_ASSERT(fresh != 0, "SignatureDB entry array alloc failed");
        if (arr && count > 0) {
            std::memcpy(fresh, arr, sizeof(SignatureEntry) * (size_t)count);
        }
        arr = fresh;
        cap = newCap;
    }
    Core::StringPool* pool = (Core::StringPool*)m_pool;
    SignatureEntry& e = arr[count];
    e.name        = pool->intern(canonical);
    e.displayName = pool->intern(display ? display : canonical);
    e.doc         = pool->intern(doc ? doc : "");
    e.frequency   = freq;
    count++;
}

namespace {
    // Per-signature in-place capacity counter. We don't store the
    // arr-cap on the TypeSignature itself; we track it adjacent to
    // the count through a parallel growth strategy: the array is
    // sized to max(16, next-power-of-two-above-count). This works
    // because we only grow on append, never shrink.
    i32 NextCap(i32 count) {
        i32 c = 16;
        while (c < count) c *= 2;
        return c;
    }
} // namespace

void EventActionSignatureDB::registerEvent(TypeId typeId,
                                            const char* canonicalName,
                                            const char* displayName,
                                            const char* doc,
                                            u32 freq)
{
    if (!typeId.valid() || !canonicalName) return;
    TypeSignature& sig = ensureSignature(typeId);
    SignatureEntry* existing = findEntry(sig.emittedEvents, sig.emittedCount, canonicalName);
    if (existing) {
        existing->frequency += freq;
        return;
    }
    i32 cap = NextCap(sig.emittedCount);
    appendEntry(sig.emittedEvents, sig.emittedCount, cap,
                canonicalName, displayName, doc, freq);
}

void EventActionSignatureDB::registerAction(TypeId typeId,
                                             const char* canonicalName,
                                             const char* displayName,
                                             const char* doc,
                                             u32 freq)
{
    if (!typeId.valid() || !canonicalName) return;
    TypeSignature& sig = ensureSignature(typeId);
    SignatureEntry* existing = findEntry(sig.acceptedActions, sig.acceptedCount, canonicalName);
    if (existing) {
        existing->frequency += freq;
        return;
    }
    i32 cap = NextCap(sig.acceptedCount);
    appendEntry(sig.acceptedActions, sig.acceptedCount, cap,
                canonicalName, displayName, doc, freq);
}

const TypeSignature* EventActionSignatureDB::findSignature(TypeId typeId) const {
    if (!typeId.valid() || m_sigCount == 0) return 0;
    i32 idx = probe(typeId);
    if (m_buckets[idx].sigIdx == -1) return 0;
    return &m_sigs[m_buckets[idx].sigIdx];
}

bool EventActionSignatureDB::typeEmitsEvent(TypeId typeId, const char* eventCanonicalName) const {
    const TypeSignature* sig = findSignature(typeId);
    if (!sig) return false;
    SignatureEntry* e = const_cast<EventActionSignatureDB*>(this)->findEntry(
        sig->emittedEvents, sig->emittedCount, eventCanonicalName);
    return e != 0;
}

bool EventActionSignatureDB::typeAcceptsAction(TypeId typeId, const char* actionCanonicalName) const {
    const TypeSignature* sig = findSignature(typeId);
    if (!sig) return false;
    SignatureEntry* e = const_cast<EventActionSignatureDB*>(this)->findEntry(
        sig->acceptedActions, sig->acceptedCount, actionCanonicalName);
    return e != 0;
}

const SignatureEntry* EventActionSignatureDB::eventsFor(TypeId typeId, i32* outCount) const {
    const TypeSignature* sig = findSignature(typeId);
    if (!sig) { if (outCount) *outCount = 0; return 0; }
    if (outCount) *outCount = sig->emittedCount;
    return sig->emittedEvents;
}

const SignatureEntry* EventActionSignatureDB::actionsFor(TypeId typeId, i32* outCount) const {
    const TypeSignature* sig = findSignature(typeId);
    if (!sig) { if (outCount) *outCount = 0; return 0; }
    if (outCount) *outCount = sig->acceptedCount;
    return sig->acceptedActions;
}

void EventActionSignatureDB::bumpEventFrequency(TypeId typeId, const char* canonicalName, u32 inc) {
    TypeSignature* sig = const_cast<TypeSignature*>(findSignature(typeId));
    if (!sig) return;
    SignatureEntry* e = findEntry(sig->emittedEvents, sig->emittedCount, canonicalName);
    if (e) e->frequency += inc;
}

void EventActionSignatureDB::bumpActionFrequency(TypeId typeId, const char* canonicalName, u32 inc) {
    TypeSignature* sig = const_cast<TypeSignature*>(findSignature(typeId));
    if (!sig) return;
    SignatureEntry* e = findEntry(sig->acceptedActions, sig->acceptedCount, canonicalName);
    if (e) e->frequency += inc;
}

EventActionSignatureDB::Stats EventActionSignatureDB::stats() const {
    Stats s = {0, 0, 0};
    s.typeCount = m_sigCount;
    for (i32 i = 0; i < m_sigCount; ++i) {
        s.totalEvents  += m_sigs[i].emittedCount;
        s.totalActions += m_sigs[i].acceptedCount;
    }
    return s;
}

void EventActionSignatureDB::resetForTests() {
    for (i32 i = 0; i < m_bucketCount; ++i) {
        m_buckets[i].id     = TypeId(0);
        m_buckets[i].sigIdx = -1;
    }
    m_sigCount = 0;
    ((Core::Arena*)m_arena)->reset();
    ((Core::StringPool*)m_pool)->reset();
}

// ── Built-in signature population ─────────────────────────────────────
// Hand-authored from observed wire patterns in LOTR:C maps. Frequency
// values are seeded at modest defaults; the corpus builder cranks them
// up with real counts on level load. SuppressUnusedWarning is silenced
// implicitly by the calls; the registry typeId returns are used.

namespace {
    void RegisterBuiltinSigs(EventActionSignatureDB& db, const ZETypeRegistry* reg) {
        if (!reg) return;

        const TypeRecord* CapturePoint        = reg->findByCanonical("capturepoint");
        const TypeRecord* TriggerVolume       = reg->findByCanonical("triggervolume");
        const TypeRecord* ScoreManager        = reg->findByCanonical("scoremanager");
        const TypeRecord* ObjectiveController = reg->findByCanonical("objectivecontroller");
        const TypeRecord* Director            = reg->findByCanonical("director");
        const TypeRecord* ScriptRelay         = reg->findByCanonical("scriptrelay");
        const TypeRecord* ScriptCounter       = reg->findByCanonical("scriptcounter");
        const TypeRecord* ScriptTimer         = reg->findByCanonical("scripttimer");
        const TypeRecord* ScriptToggle        = reg->findByCanonical("scripttoggle");
        const TypeRecord* GateController      = reg->findByCanonical("gatecontroller");
        const TypeRecord* DoorActivator       = reg->findByCanonical("dooractivator");
        const TypeRecord* VOTrigger           = reg->findByCanonical("votrigger");
        const TypeRecord* AudioManager        = reg->findByCanonical("audiomanager");
        const TypeRecord* MusicCue            = reg->findByCanonical("musiccue");
        const TypeRecord* AnimationController = reg->findByCanonical("animationcontroller");
        const TypeRecord* Spawner             = reg->findByCanonical("spawner");
        const TypeRecord* WaveSpawner         = reg->findByCanonical("wavespawner");
        const TypeRecord* DestructibleObject  = reg->findByCanonical("destructibleobject");
        const TypeRecord* CinematicEvent      = reg->findByCanonical("cinematicevent");
        const TypeRecord* Cutscene            = reg->findByCanonical("cutscene");
        const TypeRecord* Projectile          = reg->findByCanonical("projectile");
        const TypeRecord* MissileLauncher     = reg->findByCanonical("missilelauncher");
        const TypeRecord* ParticleEmitter     = reg->findByCanonical("particleemitter");
        const TypeRecord* WinCondition        = reg->findByCanonical("wincondition");
        const TypeRecord* LossCondition       = reg->findByCanonical("losscondition");

        // CapturePoint
        if (CapturePoint) {
            db.registerEvent(CapturePoint->id, "oncapture",     "OnCapture",     "Fired when the point becomes captured by a team.", 100);
            db.registerEvent(CapturePoint->id, "onneutralize",  "OnNeutralize",  "Fired when the point goes neutral.",                40);
            db.registerEvent(CapturePoint->id, "oncontested",   "OnContested",   "Fired when both teams are inside.",                  60);
            db.registerEvent(CapturePoint->id, "onfullycaptured","OnFullyCaptured","Fired when capture meter saturates.",              80);
            db.registerEvent(CapturePoint->id, "ondestroyed",   "OnDestroyed",   "Fired when the point asset is destroyed.",           20);
            db.registerAction(CapturePoint->id, "lock",         "Lock",          "Disable capture changes.",                           30);
            db.registerAction(CapturePoint->id, "unlock",       "Unlock",        "Re-enable capture changes.",                         30);
            db.registerAction(CapturePoint->id, "forceneutral", "ForceNeutral",  "Reset to neutral state.",                            10);
        }

        // TriggerVolume
        if (TriggerVolume) {
            db.registerEvent(TriggerVolume->id, "onenter",  "OnEnter",  "Fired when an entity enters the volume.", 200);
            db.registerEvent(TriggerVolume->id, "onexit",   "OnExit",   "Fired when an entity exits the volume.",  150);
            db.registerEvent(TriggerVolume->id, "onstay",   "OnStay",   "Fired each tick an entity is inside.",     30);
            db.registerAction(TriggerVolume->id, "enable",  "Enable",   "Activate the trigger.",                    50);
            db.registerAction(TriggerVolume->id, "disable", "Disable",  "Deactivate the trigger.",                  50);
            db.registerAction(TriggerVolume->id, "reset",   "Reset",    "Clear contained-entity tracking.",         15);
        }

        // ScoreManager
        if (ScoreManager) {
            db.registerAction(ScoreManager->id, "addscore",      "AddScore",      "Increment a team's score.",         300);
            db.registerAction(ScoreManager->id, "subtractscore", "SubtractScore", "Decrement a team's score.",          50);
            db.registerAction(ScoreManager->id, "resetscore",    "ResetScore",    "Set score to zero.",                 20);
            db.registerAction(ScoreManager->id, "settarget",     "SetTarget",     "Update the win-target threshold.",   10);
            db.registerEvent(ScoreManager->id,  "onthresholdhit","OnThresholdHit","Fired when a team reaches target.", 100);
        }

        // ObjectiveController
        if (ObjectiveController) {
            db.registerAction(ObjectiveController->id, "activate",   "Activate",   "Begin tracking the objective.",   80);
            db.registerAction(ObjectiveController->id, "complete",   "Complete",   "Mark objective complete.",        90);
            db.registerAction(ObjectiveController->id, "fail",       "Fail",       "Mark objective failed.",          40);
            db.registerEvent(ObjectiveController->id, "oncomplete", "OnComplete", "Fired on completion.",            90);
            db.registerEvent(ObjectiveController->id, "onfailed",   "OnFailed",   "Fired on failure.",               40);
            db.registerEvent(ObjectiveController->id, "onactivated","OnActivated","Fired when the objective starts.", 60);
        }

        // Director
        if (Director) {
            db.registerEvent(Director->id, "ongamewon",  "OnGameWon",  "Fired when the win condition triggers.", 30);
            db.registerEvent(Director->id, "ongamelost", "OnGameLost", "Fired when the loss condition triggers.",30);
            db.registerEvent(Director->id, "onstartmatch", "OnStartMatch", "Fired at match start.",              30);
            db.registerAction(Director->id, "endmatch", "EndMatch", "Force-end the match.",                      10);
            db.registerAction(Director->id, "showmessage","ShowMessage","Display a banner message.",             20);
        }

        // ScriptRelay - the wiring intermediary king
        if (ScriptRelay) {
            db.registerEvent(ScriptRelay->id, "ontrigger", "OnTrigger", "Fires every accepted input.",          400);
            db.registerAction(ScriptRelay->id, "trigger",  "Trigger",  "Fire OnTrigger immediately.",           400);
            db.registerAction(ScriptRelay->id, "enable",   "Enable",   "Allow forwarding.",                      50);
            db.registerAction(ScriptRelay->id, "disable",  "Disable",  "Block forwarding.",                      50);
        }

        // ScriptCounter
        if (ScriptCounter) {
            db.registerAction(ScriptCounter->id, "increment", "Increment", "Bump counter by 1.",                  80);
            db.registerAction(ScriptCounter->id, "reset",     "Reset",     "Set counter to 0.",                   30);
            db.registerEvent(ScriptCounter->id, "onthreshold","OnThreshold","Fires when counter reaches target.", 80);
        }

        // ScriptTimer
        if (ScriptTimer) {
            db.registerAction(ScriptTimer->id, "start", "Start", "Begin counting down.",                          60);
            db.registerAction(ScriptTimer->id, "stop",  "Stop",  "Halt the timer.",                                30);
            db.registerAction(ScriptTimer->id, "reset", "Reset", "Reload to initial duration.",                    30);
            db.registerEvent(ScriptTimer->id, "ontimerfire", "OnTimerFire", "Fires when the timer expires.",       60);
        }

        // ScriptToggle
        if (ScriptToggle) {
            db.registerAction(ScriptToggle->id, "togglestate", "ToggleState", "Flip on/off.",                       40);
            db.registerAction(ScriptToggle->id, "seton",        "SetOn",       "Force ON.",                           20);
            db.registerAction(ScriptToggle->id, "setoff",       "SetOff",      "Force OFF.",                          20);
            db.registerEvent(ScriptToggle->id, "ontoggleon",   "OnToggleOn",  "Fires when state becomes ON.",        20);
            db.registerEvent(ScriptToggle->id, "ontoggleoff",  "OnToggleOff", "Fires when state becomes OFF.",       20);
        }

        // GateController / DoorActivator
        if (GateController) {
            db.registerAction(GateController->id, "open",   "Open",   "Begin opening animation.",                    60);
            db.registerAction(GateController->id, "close",  "Close",  "Begin closing animation.",                    60);
            db.registerAction(GateController->id, "lock",   "Lock",   "Disallow open/close.",                        20);
            db.registerEvent(GateController->id, "onopened","OnOpened","Fired when fully open.",                     60);
            db.registerEvent(GateController->id, "onclosed","OnClosed","Fired when fully closed.",                   60);
        }
        if (DoorActivator) {
            db.registerAction(DoorActivator->id, "open",   "Open",   "Begin opening animation.",                     60);
            db.registerAction(DoorActivator->id, "close",  "Close",  "Begin closing animation.",                     60);
            db.registerEvent(DoorActivator->id, "onopened","OnOpened","Fired when fully open.",                      60);
            db.registerEvent(DoorActivator->id, "onclosed","OnClosed","Fired when fully closed.",                    60);
        }

        // VOTrigger / AudioManager / MusicCue
        if (VOTrigger) {
            db.registerAction(VOTrigger->id, "play", "Play", "Start playing the VO line.",                          100);
            db.registerEvent(VOTrigger->id, "onfinished", "OnFinished", "Fired when the line finishes.",            100);
        }
        if (AudioManager) {
            db.registerAction(AudioManager->id, "setmusic", "SetMusic", "Switch background music.",                  20);
            db.registerAction(AudioManager->id, "duckdialogue", "DuckDialogue", "Lower music when dialogue plays.", 30);
        }
        if (MusicCue) {
            db.registerAction(MusicCue->id, "playcue", "PlayCue", "Trigger an adaptive-music cue.",                  40);
        }

        // AnimationController
        if (AnimationController) {
            db.registerAction(AnimationController->id, "play",     "Play",     "Start playing an animation.",        80);
            db.registerAction(AnimationController->id, "stop",     "Stop",     "Halt the current animation.",        40);
            db.registerAction(AnimationController->id, "setstate", "SetState", "Switch to a named state.",           60);
            db.registerEvent(AnimationController->id, "onfinished","OnFinished","Fired when current anim finishes.", 60);
        }

        // Spawner / WaveSpawner
        if (Spawner) {
            db.registerAction(Spawner->id, "spawnunit",   "SpawnUnit",   "Spawn one unit at the spawner position.",  60);
            db.registerAction(Spawner->id, "destroyall",  "DestroyAll",  "Despawn all units this spawner created.",  20);
            db.registerEvent(Spawner->id, "onunitspawned","OnUnitSpawned","Fires per spawn.",                        60);
            db.registerEvent(Spawner->id, "onunitkilled", "OnUnitKilled", "Fires when a spawned unit dies.",         50);
        }
        if (WaveSpawner) {
            db.registerAction(WaveSpawner->id, "startwave", "StartWave", "Begin the next wave.",                      40);
            db.registerAction(WaveSpawner->id, "stopwave",  "StopWave",  "Halt wave spawning.",                       20);
            db.registerEvent(WaveSpawner->id, "onwavefinished","OnWaveFinished","Fired when a wave completes.",       40);
        }

        // DestructibleObject
        if (DestructibleObject) {
            db.registerEvent(DestructibleObject->id, "ondestroyed", "OnDestroyed", "Fired when destroyed.",          80);
            db.registerEvent(DestructibleObject->id, "ondamaged",   "OnDamaged",   "Fired when damaged but not killed.",70);
        }

        // CinematicEvent / Cutscene
        if (CinematicEvent) {
            db.registerAction(CinematicEvent->id, "play", "Play", "Begin the cinematic.",                           20);
            db.registerEvent(CinematicEvent->id, "onfinished", "OnFinished", "Fired when the cinematic ends.",      20);
        }
        if (Cutscene) {
            db.registerAction(Cutscene->id, "play", "Play", "Begin the cutscene.",                                  20);
            db.registerEvent(Cutscene->id, "onfinished", "OnFinished", "Fired when cutscene ends.",                 20);
        }

        // Projectile / MissileLauncher
        if (Projectile) {
            db.registerEvent(Projectile->id, "onhit",     "OnHit",     "Fired on collision.",                      150);
            db.registerEvent(Projectile->id, "onexpired", "OnExpired", "Fired when lifetime runs out without hit.", 30);
        }
        if (MissileLauncher) {
            db.registerAction(MissileLauncher->id, "fire", "Fire", "Launch a projectile.",                          50);
            db.registerEvent(MissileLauncher->id, "onfired", "OnFired", "Fired after the projectile leaves.",       50);
        }

        // ParticleEmitter
        if (ParticleEmitter) {
            db.registerAction(ParticleEmitter->id, "start", "Start", "Begin emitting particles.",                   80);
            db.registerAction(ParticleEmitter->id, "stop",  "Stop",  "Stop emitting particles.",                    80);
        }

        // WinCondition / LossCondition
        if (WinCondition) {
            db.registerEvent(WinCondition->id, "onsatisfied", "OnSatisfied", "Fired when the win predicate becomes true.", 30);
        }
        if (LossCondition) {
            db.registerEvent(LossCondition->id, "onsatisfied", "OnSatisfied", "Fired when the loss predicate becomes true.", 30);
        }

        // Suppress unused warning when StrEqIgnoreCase is not yet
        // referenced by the .cpp (we keep it for SignatureLoader_Lua).
        (void)&StrEqIgnoreCase;
    }
} // namespace

bool EventActionSignatureDB::init(const ZETypeRegistry* reg) {
    if (m_sigCount > 0) {
        Core::Logging::Debug("SignatureDB::init() called twice, ignoring");
        return true;
    }
    if (!reg) {
        Core::Logging::Error("SignatureDB::init called with NULL registry");
        return false;
    }
    RegisterBuiltinSigs(*this, reg);
    Stats s = stats();
    Core::Logging::Info("SignatureDB: %d types covered, %d events, %d actions",
        s.typeCount, s.totalEvents, s.totalActions);
    return s.typeCount > 0;
}

} // namespace Schema
} // namespace Vespucci
