// ============================================================================
// Console - Real-time output window
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include "../include/console.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cstdlib>

namespace ConquestDebugger {

Console& Console::Instance() {
    static Console instance;
    return instance;
}

Console::Console()
    : m_created(false)
    , m_stdout(INVALID_HANDLE_VALUE)
    , m_stdin(INVALID_HANDLE_VALUE)
    , m_defaultAttributes(0)
    , m_logFile(nullptr)
{
}

Console::~Console() {
    Destroy();
}

bool Console::Create(const char* title) {
    if (m_created) return true;

    if (!AllocConsole()) {
        return false;
    }

    // Redirect stdout
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);

    m_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    m_stdin = GetStdHandle(STD_INPUT_HANDLE);

    // Save default attributes
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(m_stdout, &csbi)) {
        m_defaultAttributes = csbi.wAttributes;
    } else {
        m_defaultAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    SetTitle(title);

    // Open log file
    m_logFile = fopen("conquest_debugger.log", "w");
    if (m_logFile) {
        time_t now = time(nullptr);
        char timeBuf[64];
        ctime_s(timeBuf, sizeof(timeBuf), &now);
        fprintf(m_logFile, "=== ConquestDebugger Log Started: %s===\n\n", timeBuf);
    }

    m_created = true;
    return true;
}

void Console::Destroy() {
    if (!m_created) return;

    if (m_logFile) {
        fprintf(m_logFile, "\n=== Log Ended ===\n");
        fclose(m_logFile);
        m_logFile = nullptr;
    }

    FreeConsole();
    m_created = false;
}

void Console::Write(const char* text) {
    if (!m_created) return;
    printf("%s", text);
    if (m_logFile) {
        fprintf(m_logFile, "%s", text);
        fflush(m_logFile);
    }
}

void Console::WriteLine(const char* text) {
    if (!m_created) return;
    printf("%s\n", text);
    if (m_logFile) {
        fprintf(m_logFile, "%s\n", text);
        fflush(m_logFile);
    }
}

void Console::WriteFormatted(const char* format, ...) {
    if (!m_created) return;
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Write(buffer);
}

void Console::SetColor(ConsoleColor foreground, ConsoleColor background) {
    if (!m_created) return;
    SetConsoleTextAttribute(m_stdout, (WORD)foreground | ((WORD)background << 4));
}

void Console::ResetColor() {
    if (!m_created) return;
    SetConsoleTextAttribute(m_stdout, m_defaultAttributes);
}

void Console::WriteColored(ConsoleColor color, const char* text) {
    SetColor(color);
    Write(text);
    ResetColor();
}

ConsoleColor Console::GetColorForLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return ConsoleColor::DarkGray;
        case LogLevel::Debug:    return ConsoleColor::Gray;
        case LogLevel::Info:     return ConsoleColor::White;
        case LogLevel::Warning:  return ConsoleColor::Yellow;
        case LogLevel::Error:    return ConsoleColor::Red;
        case LogLevel::Critical: return ConsoleColor::Magenta;
        default:                 return ConsoleColor::White;
    }
}

void Console::WriteLog(LogLevel level, const char* text) {
    SetColor(GetColorForLevel(level));
    Write(text);
    ResetColor();
    Write("\n");
}

void Console::Clear() {
    if (!m_created) return;
    system("cls");
}

void Console::SetTitle(const char* title) {
    if (!m_created) return;
    SetConsoleTitleA(title);
}

void Console::SetSize(int width, int height) {
    if (!m_created) return;
    SMALL_RECT rect = { 0, 0, (SHORT)(width - 1), (SHORT)(height - 1) };
    COORD size = { (SHORT)width, (SHORT)height };
    SetConsoleScreenBufferSize(m_stdout, size);
    SetConsoleWindowInfo(m_stdout, TRUE, &rect);
}

} // namespace ConquestDebugger

