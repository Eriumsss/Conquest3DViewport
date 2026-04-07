#include "BankLoadProgress.h"
#include <CommCtrl.h>
#include <sstream>

#pragma comment(lib, "comctl32.lib")

BankLoadProgressDialog::BankLoadProgressDialog()
    : m_hwnd(NULL)
    , m_progressBar(NULL)
    , m_statusText(NULL)
    , m_detailsText(NULL)
    , m_totalBanks(0)
    , m_currentBank(0)
    , m_successCount(0)
    , m_failCount(0)
    , m_cancelled(false)
{
}

BankLoadProgressDialog::~BankLoadProgressDialog()
{
    Close();
}

void BankLoadProgressDialog::Show(const char* title, int totalBanks)
{
    m_totalBanks = totalBanks;
    m_currentBank = 0;
    m_successCount = 0;
    m_failCount = 0;
    m_cancelled = false;
    m_details.clear();

    // Create dialog window
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "BankLoadProgressClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassEx(&wc);

    m_hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "BankLoadProgressClass",
        "Loading Audio Banks",
        WS_POPUP | WS_BORDER | WS_CAPTION,
        (GetSystemMetrics(SM_CXSCREEN) - 500) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 200) / 2,
        500, 200,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    // Create progress bar
    InitCommonControls();
    m_progressBar = CreateWindowEx(
        0, PROGRESS_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        20, 20, 460, 25,
        m_hwnd, NULL, GetModuleHandle(NULL), NULL
    );
    SendMessage(m_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, totalBanks));
    SendMessage(m_progressBar, PBM_SETSTEP, 1, 0);

    // Create status text
    m_statusText = CreateWindowEx(
        0, "STATIC", "Initializing...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 55, 460, 20,
        m_hwnd, NULL, GetModuleHandle(NULL), NULL
    );

    // Create details text (scrollable)
    m_detailsText = CreateWindowEx(
        WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        20, 85, 460, 80,
        m_hwnd, NULL, GetModuleHandle(NULL), NULL
    );

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    ProcessMessages();
}

void BankLoadProgressDialog::UpdateProgress(int currentBank, const char* bankName, bool success)
{
    if (!m_hwnd) return;

    m_currentBank = currentBank;
    if (success) m_successCount++;
    else m_failCount++;

    // Update progress bar
    SendMessage(m_progressBar, PBM_SETPOS, currentBank, 0);

    // Update status text
    char statusBuf[256];
    sprintf_s(statusBuf, 256, "Loading bank %d / %d (OK:%d FAIL:%d)",
              currentBank, m_totalBanks, m_successCount, m_failCount);
    SetWindowText(m_statusText, statusBuf);

    // Add to details
    std::string detail = success ? "[OK] " : "[FAIL] ";
    detail += bankName;
    detail += "\r\n";
    m_details += detail;

    // Keep only last 20 lines
    size_t lineCount = 0;
    size_t pos = m_details.length();
    while (pos > 0 && lineCount < 20) {
        pos = m_details.rfind("\r\n", pos - 1);
        if (pos == std::string::npos) break;
        lineCount++;
    }
    if (lineCount >= 20 && pos != std::string::npos) {
        m_details = m_details.substr(pos + 2);
    }

    // Update details text
    SetWindowText(m_detailsText, m_details.c_str());

    // Scroll to bottom
    SendMessage(m_detailsText, EM_SETSEL, m_details.length(), m_details.length());
    SendMessage(m_detailsText, EM_SCROLLCARET, 0, 0);

    ProcessMessages();
}

void BankLoadProgressDialog::Close()
{
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = NULL;
    }
}

void BankLoadProgressDialog::ProcessMessages()
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

