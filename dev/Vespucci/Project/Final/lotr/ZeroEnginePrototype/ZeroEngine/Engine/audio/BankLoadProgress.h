#pragma once
#include <Windows.h>
#include <string>

// Simple progress dialog for bank loading
class BankLoadProgressDialog
{
public:
    BankLoadProgressDialog();
    ~BankLoadProgressDialog();

    // Show the dialog (non-blocking)
    void Show(const char* title, int totalBanks);
    
    // Update progress
    void UpdateProgress(int currentBank, const char* bankName, bool success);
    
    // Close the dialog
    void Close();
    
    // Check if user cancelled
    bool IsCancelled() const { return m_cancelled; }

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ProcessMessages();

    HWND m_hwnd;
    HWND m_progressBar;
    HWND m_statusText;
    HWND m_detailsText;
    int m_totalBanks;
    int m_currentBank;
    int m_successCount;
    int m_failCount;
    bool m_cancelled;
    std::string m_details;
};

