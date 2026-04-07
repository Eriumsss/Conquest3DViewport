#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>
//Eriumsss
// ============================================================================
// DLL Injector for Debug Overlay
// Injects DebugOverlay.dll into ConquestLLC.exe
// ============================================================================

DWORD GetProcessIdByName(const char* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, processName) == 0) {
                CloseHandle(snapshot);
                return pe.th32ProcessID;
            }
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return 0;
}

bool InjectDLL(DWORD processId, const char* dllPath) {
    // Get full path
    char fullPath[MAX_PATH];
    GetFullPathNameA(dllPath, MAX_PATH, fullPath, NULL);

    // Open target process
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (!process) {
        std::cerr << "Failed to open process. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Allocate memory in target process for DLL path
    size_t pathLen = strlen(fullPath) + 1;
    LPVOID remotePath = VirtualAllocEx(process, NULL, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        std::cerr << "Failed to allocate memory. Error: " << GetLastError() << std::endl;
        CloseHandle(process);
        return false;
    }

    // Write DLL path to target process
    if (!WriteProcessMemory(process, remotePath, fullPath, pathLen, NULL)) {
        std::cerr << "Failed to write memory. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // Get LoadLibraryA address
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryA");

    // Create remote thread to load DLL
    HANDLE thread = CreateRemoteThread(process, NULL, 0, loadLibrary, remotePath, 0, NULL);
    if (!thread) {
        std::cerr << "Failed to create remote thread. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // Wait for thread to complete
    WaitForSingleObject(thread, INFINITE);

    // Cleanup
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(process);

    return true;
}

int main(int argc, char* argv[]) {
    std::cout << "=== LOTR: Conquest Debug Overlay Injector ===" << std::endl;
    std::cout << std::endl;

    const char* processName = "ConquestLLC.exe";
    const char* dllName = "DebugOverlay.dll";

    // Find process
    std::cout << "Looking for " << processName << "..." << std::endl;
    DWORD pid = GetProcessIdByName(processName);

    if (pid == 0) {
        std::cout << "Process not found. Please start the game first." << std::endl;
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "Found process with PID: " << pid << std::endl;

    // Inject DLL
    std::cout << "Injecting " << dllName << "..." << std::endl;
    
    if (InjectDLL(pid, dllName)) {
        std::cout << "Successfully injected!" << std::endl;
        std::cout << std::endl;
        std::cout << "Controls:" << std::endl;
        std::cout << "  F1 - Toggle overlay" << std::endl;
        std::cout << "  F2 - Toggle FPS counter" << std::endl;
        std::cout << "  F5 - Toggle debug menu" << std::endl;
    } else {
        std::cout << "Injection failed!" << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();
    return 0;
}

