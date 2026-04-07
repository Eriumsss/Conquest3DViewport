#pragma once
// ============================================================================
// Console Window - Real-time output with color-coded logs
// ============================================================================

#ifndef CONSOLE_H
#define CONSOLE_H

#include "debugger.h"
#include <cstdio>

namespace ConquestDebugger {

// Console text colors
enum class ConsoleColor : WORD {
    Black       = 0,
    DarkBlue    = 1,
    DarkGreen   = 2,
    DarkCyan    = 3,
    DarkRed     = 4,
    DarkMagenta = 5,
    DarkYellow  = 6,
    Gray        = 7,
    DarkGray    = 8,
    Blue        = 9,
    Green       = 10,
    Cyan        = 11,
    Red         = 12,
    Magenta     = 13,
    Yellow      = 14,
    White       = 15
};

class Console {
public:
    static Console& Instance();

    bool Create(const char* title = "Conquest Debugger");
    void Destroy();
    bool IsCreated() const { return m_created; }

    // Output
    void Write(const char* text);
    void WriteLine(const char* text);
    void WriteFormatted(const char* format, ...);

    // Colored output
    void SetColor(ConsoleColor foreground, ConsoleColor background = ConsoleColor::Black);
    void ResetColor();
    void WriteColored(ConsoleColor color, const char* text);

    // Log level colors
    void WriteLog(LogLevel level, const char* text);

    // Utility
    void Clear();
    void SetTitle(const char* title);
    void SetSize(int width, int height);

    // Input (for future command interface)
    bool ReadLine(char* buffer, size_t bufferSize);

private:
    Console();
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    bool m_created;
    HANDLE m_stdout;
    HANDLE m_stdin;
    WORD m_defaultAttributes;
    FILE* m_logFile;

    // Color mapping for log levels
    static ConsoleColor GetColorForLevel(LogLevel level);
};

} // namespace ConquestDebugger

#endif // CONSOLE_H

