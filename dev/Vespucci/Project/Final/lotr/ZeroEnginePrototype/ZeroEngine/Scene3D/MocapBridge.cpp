// ============================================================================
// MocapBridge.cpp -- The Unholy Union of Win32 and Machine Learning
// ============================================================================
//
// This file launches a Python process, creates anonymous pipes, and reads
// SMPL pose data line by line through PeekNamedPipe/ReadFile. It's a
// CreateProcessA call away from actual motion capture in a dead 2009 game.
//
// The pipe reading is non-blocking so the game loop doesn't stall while
// WHAM crunches through a video. Every frame we peek the pipe, pull any
// complete JSON lines, and parse them with the world's shittiest JSON
// parser (4 static methods, no allocations, no dependencies). It works
// for our exact format and will explode on anything else. Don't care.
//
// PYTHONPATH gets mutated to include ViTPose because WHAM needs it and
// Python packaging is a fucking nightmare. If the subprocess dies we
// drain the remaining pipe data and report the exit code. If it hangs
// we TerminateProcess it like the merciful gods we are.
//
// Pandemic's original animation pipeline was Maya -> custom exporter ->
// .anim binary. We replaced Maya with a webcam. Progress.
//
// VS2005 compatible (C++03). Hand-rolled everything.
// ============================================================================

#include "MocapBridge.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma warning(disable: 4996) // sprintf, strcpy

// ============================================================
// Construction / Destruction
// ============================================================

MocapBridge::MocapBridge()
    : m_hProcess(NULL), m_hThread(NULL),
      m_hPipeRead(NULL), m_hPipeWrite(NULL),
      m_state(MOCAP_IDLE), m_progress(0.0f),
      m_totalFrames(0), m_fps(30.0f), m_subjectCount(0),
      m_linePos(0)
{
    m_statusMsg[0] = 0;
    m_errorMsg[0] = 0;
    m_pklPath[0] = 0;
    m_lineBuf[0] = 0;
}

MocapBridge::~MocapBridge()
{
    Cancel();
}

// ============================================================
// Start Processing
// ============================================================

bool MocapBridge::StartProcessing(const char* pythonExe,
                                   const char* whamDir,
                                   const char* videoPath,
                                   int subjectId)
{
    // Clean up any previous run
    Cancel();
    Reset();

    // Build command line
    char cmdLine[4096];
    sprintf(cmdLine,
        "\"%s\" \"%s\\wham_stream.py\" --video \"%s\" --subject %d",
        pythonExe, whamDir, videoPath, subjectId);

    m_state = MOCAP_LOADING;
    strcpy(m_statusMsg, "Starting WHAM...");
    m_progress = 0.0f;

    if (!LaunchProcess(cmdLine, whamDir))
    {
        m_state = MOCAP_ERROR;
        sprintf(m_errorMsg, "Failed to launch Python subprocess");
        return false;
    }

    return true;
}

// ============================================================
// Update — call each frame
// ============================================================

bool MocapBridge::Update()
{
    if (m_state == MOCAP_IDLE || m_state == MOCAP_DONE || m_state == MOCAP_ERROR)
        return false;

    // Check if process is still alive
    if (m_hProcess)
    {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(m_hProcess, &exitCode))
        {
            if (exitCode != STILL_ACTIVE)
            {
                // Process ended — read remaining pipe data
                ReadPipe();
                CleanupProcess();

                if (m_state != MOCAP_DONE && m_state != MOCAP_ERROR)
                {
                    m_state = MOCAP_ERROR;
                    if (m_errorMsg[0] == 0)
                        sprintf(m_errorMsg, "WHAM process exited with code %lu", exitCode);
                }
                return true;
            }
        }
    }

    // Read available pipe data (non-blocking)
    ReadPipe();

    return m_frames.size() > 0;
}

// ============================================================
// Cancel
// ============================================================

void MocapBridge::Cancel()
{
    if (m_hProcess)
    {
        TerminateProcess(m_hProcess, 1);
        CleanupProcess();
    }
    if (m_state == MOCAP_LOADING || m_state == MOCAP_PROCESSING || m_state == MOCAP_STREAMING)
    {
        m_state = MOCAP_IDLE;
        strcpy(m_statusMsg, "Cancelled");
    }
}

// ============================================================
// Reset
// ============================================================

void MocapBridge::Reset()
{
    m_state = MOCAP_IDLE;
    m_statusMsg[0] = 0;
    m_errorMsg[0] = 0;
    m_pklPath[0] = 0;
    m_progress = 0.0f;
    m_totalFrames = 0;
    m_fps = 30.0f;
    m_subjectCount = 0;
    m_frames.clear();
    m_linePos = 0;
}

// ============================================================
// Frame Access
// ============================================================

const MocapFrame* MocapBridge::GetFrame(int index) const
{
    if (index < 0 || index >= (int)m_frames.size())
        return NULL;
    return &m_frames[index];
}

const MocapFrame* MocapBridge::GetFrameAtTime(float timeSeconds) const
{
    if (m_frames.empty() || m_fps <= 0.0f)
        return NULL;
    int f = (int)(timeSeconds * m_fps);
    if (f < 0) f = 0;
    if (f >= (int)m_frames.size()) f = (int)m_frames.size() - 1;
    return &m_frames[f];
}

// ============================================================
// Subprocess Launch
// ============================================================

bool MocapBridge::LaunchProcess(const char* cmdLine, const char* workDir)
{
    // Create pipe for stdout
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&m_hPipeRead, &m_hPipeWrite, &sa, 0))
        return false;

    // Don't inherit the read end
    SetHandleInformation(m_hPipeRead, HANDLE_FLAG_INHERIT, 0);

    // Set up process
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = m_hPipeWrite;
    si.hStdError = m_hPipeWrite;
    si.hStdInput = NULL;
    si.wShowWindow = SW_HIDE;

    // Need mutable copy of cmdLine for CreateProcess
    char cmdBuf[2048];
    strncpy(cmdBuf, cmdLine, sizeof(cmdBuf) - 1);
    cmdBuf[sizeof(cmdBuf) - 1] = 0;

    // Set PYTHONPATH for ViTPose
    char envBlock[4096];
    char vitposePath[1024];
    sprintf(vitposePath, "%s\\third-party\\ViTPose", workDir);

    // Get existing PYTHONPATH
    char existingPath[2048] = "";
    GetEnvironmentVariableA("PYTHONPATH", existingPath, sizeof(existingPath));

    if (existingPath[0])
        sprintf(envBlock, "%s;%s", vitposePath, existingPath);
    else
        strcpy(envBlock, vitposePath);
    SetEnvironmentVariableA("PYTHONPATH", envBlock);

    BOOL ok = CreateProcessA(
        NULL, cmdBuf, NULL, NULL,
        TRUE,  // inherit handles
        CREATE_NO_WINDOW,
        NULL,  // inherit environment (with modified PYTHONPATH)
        workDir,
        &si, &pi);

    // Close the write end in our process (child has it)
    CloseHandle(m_hPipeWrite);
    m_hPipeWrite = NULL;

    if (!ok)
    {
        CloseHandle(m_hPipeRead);
        m_hPipeRead = NULL;
        return false;
    }

    m_hProcess = pi.hProcess;
    m_hThread = pi.hThread;
    return true;
}

// ============================================================
// Pipe Reading (non-blocking)
// ============================================================

void MocapBridge::ReadPipe()
{
    if (!m_hPipeRead)
        return;

    while (true)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(m_hPipeRead, NULL, 0, NULL, &avail, NULL))
            break;
        if (avail == 0)
            break;

        // Read a chunk
        char buf[4096];
        DWORD toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
        DWORD bytesRead = 0;
        if (!ReadFile(m_hPipeRead, buf, toRead, &bytesRead, NULL))
            break;
        if (bytesRead == 0)
            break;

        // Append to line buffer, process complete lines
        for (DWORD i = 0; i < bytesRead; ++i)
        {
            if (buf[i] == '\n' || buf[i] == '\r')
            {
                if (m_linePos > 0)
                {
                    m_lineBuf[m_linePos] = 0;
                    ParseLine(m_lineBuf);
                    m_linePos = 0;
                }
            }
            else if (m_linePos < (int)sizeof(m_lineBuf) - 1)
            {
                m_lineBuf[m_linePos++] = buf[i];
            }
        }
    }
}

// ============================================================
// JSON Line Parsing
// ============================================================

void MocapBridge::ParseLine(const char* line)
{
    // Determine message type
    char typeStr[32] = "";
    if (!JsonGetString(line, "type", typeStr, sizeof(typeStr)))
        return;

    if (strcmp(typeStr, "status") == 0)
    {
        JsonGetString(line, "msg", m_statusMsg, sizeof(m_statusMsg));
        JsonGetFloat(line, "progress", &m_progress);
        if (m_state == MOCAP_IDLE || m_state == MOCAP_LOADING)
            m_state = MOCAP_PROCESSING;
    }
    else if (strcmp(typeStr, "frame_count") == 0)
    {
        JsonGetInt(line, "total", &m_totalFrames);
        JsonGetFloat(line, "fps", &m_fps);
        JsonGetInt(line, "subjects", &m_subjectCount);
        JsonGetString(line, "pkl_path", m_pklPath, sizeof(m_pklPath));
        m_frames.reserve(m_totalFrames);
        m_state = MOCAP_STREAMING;
    }
    else if (strcmp(typeStr, "smpl_frame") == 0)
    {
        MocapFrame frame;
        memset(&frame, 0, sizeof(frame));
        JsonGetInt(line, "f", &frame.frameIndex);
        JsonGetFloatArray(line, "pose", frame.pose, 72);
        JsonGetFloatArray(line, "trans", frame.trans, 3);
        frame.hasContact = JsonGetFloatArray(line, "contact", frame.contact, 4);
        m_frames.push_back(frame);

        // Update progress
        if (m_totalFrames > 0)
        {
            m_progress = 0.92f + 0.08f * ((float)m_frames.size() / (float)m_totalFrames);
            sprintf(m_statusMsg, "Receiving frame %d/%d", (int)m_frames.size(), m_totalFrames);
        }
    }
    else if (strcmp(typeStr, "done") == 0)
    {
        m_state = MOCAP_DONE;
        m_progress = 1.0f;
        JsonGetString(line, "pkl_path", m_pklPath, sizeof(m_pklPath));
        sprintf(m_statusMsg, "Done! %d frames captured", (int)m_frames.size());
    }
    else if (strcmp(typeStr, "error") == 0)
    {
        m_state = MOCAP_ERROR;
        JsonGetString(line, "msg", m_errorMsg, sizeof(m_errorMsg));
    }
}

// ============================================================
// Cleanup
// ============================================================

void MocapBridge::CleanupProcess()
{
    if (m_hPipeRead)  { CloseHandle(m_hPipeRead);  m_hPipeRead = NULL; }
    if (m_hPipeWrite) { CloseHandle(m_hPipeWrite); m_hPipeWrite = NULL; }
    if (m_hThread)    { CloseHandle(m_hThread);    m_hThread = NULL; }
    if (m_hProcess)   { CloseHandle(m_hProcess);   m_hProcess = NULL; }
}

// ============================================================
// Minimal JSON Helpers (no external deps, VS2005 compatible)
// ============================================================

bool MocapBridge::JsonGetString(const char* json, const char* key, char* out, int outSize)
{
    // Find "key":"value"
    char pattern[128];
    sprintf(pattern, "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    int i = 0;
    while (*p && *p != '"' && i < outSize - 1)
    {
        if (*p == '\\' && *(p+1)) { p++; } // skip escape
        out[i++] = *p++;
    }
    out[i] = 0;
    return true;
}

bool MocapBridge::JsonGetFloat(const char* json, const char* key, float* out)
{
    char pattern[128];
    sprintf(pattern, "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    *out = (float)atof(p);
    return true;
}

bool MocapBridge::JsonGetInt(const char* json, const char* key, int* out)
{
    char pattern[128];
    sprintf(pattern, "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    *out = atoi(p);
    return true;
}

bool MocapBridge::JsonGetFloatArray(const char* json, const char* key, float* out, int count)
{
    char pattern[128];
    sprintf(pattern, "\"%s\":[", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    for (int i = 0; i < count; ++i)
    {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == 0) return false;
        out[i] = (float)atof(p);
        // Skip past this number
        while (*p && *p != ',' && *p != ']') p++;
    }
    return true;
}
