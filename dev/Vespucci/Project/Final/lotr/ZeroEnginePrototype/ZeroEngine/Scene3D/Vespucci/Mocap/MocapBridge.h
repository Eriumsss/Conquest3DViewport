// ============================================================================
// MocapBridge.h -- WHAM Subprocess Bridge for Real-World Motion Capture
// ============================================================================
//
// Pandemic Studios never had this. They hand-keyed animations in Maya like
// goddamn cavemen, or bought them from middleware vendors. We're BETTER now.
// This spawns a whole-ass Python subprocess running WHAM (World-grounded
// Human Avatar Motion estimation) and reads SMPL pose data through a Win32
// pipe. 24 joints * 3 axis-angle = 72 floats per frame of real human motion,
// streamed directly into a dead game's skeleton.
//
// You point a webcam at yourself, wave a sword around like an idiot, and
// this thing captures your motion and feeds it into Conquest's 62-bone rig.
// Pandemic would shit themselves. EA would send a cease-and-desist.
//
// The JSON parsing is hand-rolled because VS2005 can't have nice things
// like actual JSON libraries. We parse {"type":"smpl_frame","pose":[...]}
// one byte at a time through a motherfucking named pipe. It works.
// Don't touch the line buffer size or you'll truncate frames and spend
// 3 days debugging why joint 17 suddenly points at the sky.
//
// VS2005 compatible (C++03). Because this engine is a time capsule from
// 2008 and we're bolting a 2024 ML pipeline onto it. God help us.
// ============================================================================

#ifndef MOCAP_BRIDGE_H
#define MOCAP_BRIDGE_H

#include <windows.h>
#include <string>
#include <vector>

// Per-frame SMPL pose data (24 joints * 3 axis-angle = 72 floats)
struct MocapFrame
{
    int   frameIndex;
    float pose[72];       // 24 joints * 3 axis-angle (world coords)
    float trans[3];       // root translation XYZ (world coords)
    float contact[4];     // foot contact: L_heel, L_toe, R_heel, R_toe
    bool  hasContact;
};

// Processing state
enum MocapState
{
    MOCAP_IDLE = 0,
    MOCAP_LOADING,        // subprocess starting, models loading
    MOCAP_PROCESSING,     // WHAM running on video
    MOCAP_STREAMING,      // receiving per-frame data
    MOCAP_DONE,           // all frames received
    MOCAP_ERROR
};

class MocapBridge
{
public:
    MocapBridge();
    ~MocapBridge();

    // Start processing a video file
    // pythonExe: path to wham311 python.exe
    // whamDir: path to WHAM/ directory (contains wham_stream.py)
    // videoPath: path to video file
    bool StartProcessing(const char* pythonExe,
                         const char* whamDir,
                         const char* videoPath,
                         int subjectId = 0);

    // Call each frame to read any available pipe data
    // Returns true if new data was received
    bool Update();

    // Stop/cancel processing
    void Cancel();

    // Reset state for new video
    void Reset();

    // --- Accessors ---

    MocapState      GetState()        const { return m_state; }
    const char*     GetStatusMsg()    const { return m_statusMsg; }
    float           GetProgress()     const { return m_progress; }
    const char*     GetErrorMsg()     const { return m_errorMsg; }

    int             GetTotalFrames()  const { return m_totalFrames; }
    int             GetReceivedFrames() const { return (int)m_frames.size(); }
    float           GetFps()          const { return m_fps; }
    int             GetSubjectCount() const { return m_subjectCount; }
    const char*     GetPklPath()      const { return m_pklPath; }

    // Access frame data (valid after MOCAP_DONE or during MOCAP_STREAMING)
    const MocapFrame* GetFrame(int index) const;
    const std::vector<MocapFrame>& GetFrames() const { return m_frames; }

    // Playback helpers
    const MocapFrame* GetFrameAtTime(float timeSeconds) const;

private:
    // Subprocess handles
    HANDLE m_hProcess;
    HANDLE m_hThread;
    HANDLE m_hPipeRead;
    HANDLE m_hPipeWrite;

    // State
    MocapState m_state;
    char       m_statusMsg[256];
    float      m_progress;
    char       m_errorMsg[512];
    char       m_pklPath[512];

    // Video info
    int   m_totalFrames;
    float m_fps;
    int   m_subjectCount;

    // Received frames
    std::vector<MocapFrame> m_frames;

    // Line buffer for pipe reading
    char  m_lineBuf[8192];
    int   m_linePos;

    // Internal
    bool LaunchProcess(const char* cmdLine, const char* workDir);
    void ReadPipe();
    void ParseLine(const char* line);
    void CleanupProcess();

    // Simple JSON value extraction (no external deps)
    static bool  JsonGetString(const char* json, const char* key, char* out, int outSize);
    static bool  JsonGetFloat(const char* json, const char* key, float* out);
    static bool  JsonGetInt(const char* json, const char* key, int* out);
    static bool  JsonGetFloatArray(const char* json, const char* key, float* out, int count);
};

#endif // MOCAP_BRIDGE_H
