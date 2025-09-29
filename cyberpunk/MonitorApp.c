// MonitorApp.c  (renamed runtime: netwatch.exe)
// Background watchdog (no console). launches synapse_burnout.exe, restarts if it exits,
// listens on \\.\pipe\LabMonitorPipe_v1 for "STOP".
// Graceful shutdown: when STOP is received, give child a short grace period to exit naturally,
// then stop restarting. No logging performed.

#include <windows.h>
#include <stdio.h>

volatile LONG g_stop = 0;

DWORD WINAPI PipeThreadProc(LPVOID lpv) {
    (void)lpv;
    const char *pipename = "\\\\.\\pipe\\LabMonitorPipe_v1";
    while (!g_stop) {
        HANDLE hPipe = CreateNamedPipeA(
            pipename,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            512,
            512,
            0,
            NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        char buf[128] = {0};
        DWORD readBytes = 0;
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf)-1, &readBytes, NULL);
        if (ok && readBytes > 0) {
            buf[readBytes] = '\0';
            for (DWORD i=0;i<readBytes;i++) if (buf[i]=='\r' || buf[i]=='\n') { buf[i]='\0'; break; }
            if (_stricmp(buf, "STOP") == 0) {
                InterlockedExchange(&g_stop, 1);
                FlushFileBuffers(hPipe);
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
                break;
            }
        }

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}

int start_target_and_wait(void) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    const char *target = "synapse_burnout.exe";

    DWORD attrs = GetFileAttributesA(target);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // target not present; sleep before retrying
        Sleep(5000);
        return -1;
    }

    char cmdline[512];
    snprintf(cmdline, sizeof(cmdline), "\"%s\"", target);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Sleep(2000);
        return -1;
    }

    // Wait for the child to exit OR if g_stop is set: wait a short grace period for child to exit naturally.
    while (1) {
        DWORD wr = WaitForSingleObject(pi.hProcess, 500);
        if (wr == WAIT_OBJECT_0) {
            break;
        }
        if (g_stop) {
            // STOP received: give the child a grace period to exit on its own (10 seconds)
            DWORD graceMs = 10000; // 10 seconds
            DWORD waited = 0;
            DWORD slice = 200;
            while (waited < graceMs) {
                DWORD wr2 = WaitForSingleObject(pi.hProcess, slice);
                if (wr2 == WAIT_OBJECT_0) {
                    break;
                }
                waited += slice;
            }
            // Do NOT forcibly terminate the child; close handles and return.
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 0;
        }
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nCmdShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nCmdShow;

    HANDLE hPipeThread = CreateThread(NULL, 0, PipeThreadProc, NULL, 0, NULL);
    if (!hPipeThread) { /* continue without pipe thread if creation failed */ }

    while (!g_stop) {
        int res = start_target_and_wait();
        if (g_stop) break;
        if (res == -1) {
            Sleep(5000);
        } else {
            Sleep(1000);
        }
    }

    if (hPipeThread) {
        // allow a short wait for pipe thread to exit
        WaitForSingleObject(hPipeThread, 2000);
        CloseHandle(hPipeThread);
    }

    return 0;
}
