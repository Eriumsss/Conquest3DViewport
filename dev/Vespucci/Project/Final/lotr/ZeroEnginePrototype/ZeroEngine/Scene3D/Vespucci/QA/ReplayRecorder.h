// ReplayRecorder.h
// =============================================================================
// RECORD EDITOR-SESSION ACTIONS FOR DETERMINISTIC REPLAY
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked: 'replay benchmark from real editor sessions' is the
// AAA acceptance-rate substitute for the single-user product. We
// log every snapshot version, every ranker query, every Apply event
// to a binary file. The replay player feeds them back in for
// regression testing and benchmark stability checks.
// =============================================================================

#ifndef VESPUCCI_QA_REPLAYRECORDER_H_
#define VESPUCCI_QA_REPLAYRECORDER_H_

#include "../Core/VespucciTypes.h"
#include "../Suggest/SuggestionTypes.h"

namespace Vespucci {
namespace QA {

enum ReplayEventKind {
    REPLAY_SnapshotChanged   = 0,
    REPLAY_RankerQuery        = 1,
    REPLAY_ApplyFired         = 2,
    REPLAY_UndoFired          = 3,
    REPLAY_PreviewStaged      = 4,
    REPLAY_LevelLoaded        = 5
};

struct ReplayEvent {
    ReplayEventKind kind;
    u64             unixSec;
    u32             snapshotVersion;
    Guid            guidA;
    Guid            guidB;
    char            payload[64];
};

void  StartRecordingToFile(const char* path);
void  StopRecording();
bool  IsRecording();
void  AppendReplayEvent(const ReplayEvent& evt);
i32   GetReplayEventsThisSession();

} // namespace QA
} // namespace Vespucci

#endif // VESPUCCI_QA_REPLAYRECORDER_H_
