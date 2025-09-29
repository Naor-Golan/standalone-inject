// TargetApp.c
// Full integrated program:
// - Force console window to ~1000x800 pixels (best-effort).
// - Print art.txt (if present).
// - Embedded secret literal discoverable with strings.
// - Accept hex input (spaces allowed) and send STOP to monitor pipe on success.
// Build: gcc TargetApp.c -o TargetApp.exe   (MinGW)
//        cl /TC TargetApp.c /Fe:TargetApp.exe  (MSVC)

#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Embedded secret literal (kept but not printed)
volatile const char secret_hidden[] = "secret unlock code: 4E 41 4F 52";

// --- Utility: log to C:\Temp\target_debug.log (best-effort)
static void log_debug(const char *s) {
    FILE *f = fopen("C:\\Temp\\target_debug.log", "a");
    if (!f) return;
    fputs(s, f);
    fputs("\n", f);
    fclose(f);
}

// --- Force console outer window size in pixels (best-effort).
//    Uses font cell size to compute columns/rows then attempts to set buffer/window.
static void ForceConsoleWindowSizePixels(int targetWidthPx, int targetHeightPx)
{
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) hOut = NULL;

    SHORT fontX = 0, fontY = 0;
    // Try GetCurrentConsoleFontEx
    {
        typedef BOOL (WINAPI *GCFEX)(HANDLE, BOOL, PCONSOLE_FONT_INFOEX);
        HMODULE hKernel = GetModuleHandleA("kernel32.dll");
        if (hKernel && hOut) {
            GCFEX pGet = (GCFEX) GetProcAddress(hKernel, "GetCurrentConsoleFontEx");
            if (pGet) {
                CONSOLE_FONT_INFOEX fi;
                ZeroMemory(&fi, sizeof(fi));
                fi.cbSize = sizeof(fi);
                if (pGet(hOut, FALSE, &fi)) {
                    fontX = fi.dwFontSize.X;
                    fontY = fi.dwFontSize.Y;
                }
            }
        }
    }

    // Fallback to GetCurrentConsoleFont + GetConsoleFontSize
    if ((fontX == 0 || fontY == 0) && hOut) {
        CONSOLE_FONT_INFO cfi;
        if (GetCurrentConsoleFont(hOut, FALSE, &cfi)) {
            COORD csz = GetConsoleFontSize(hOut, cfi.nFont);
            fontX = csz.X;
            fontY = csz.Y;
        }
    }

    // If we couldn't determine font size, just set window pixels and return
    if (fontX <= 0 || fontY <= 0) {
        SetWindowPos(hwnd, NULL, 0, 0, targetWidthPx, targetHeightPx,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
        return;
    }

    // compute desired columns/rows from pixel request
    int cols = targetWidthPx / fontX;
    int rows = targetHeightPx / fontY;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    // Get current buffer info
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (hOut && GetConsoleScreenBufferInfo(hOut, &csbi)) {
        // Ensure buffer at least desired
        COORD newBuf = csbi.dwSize;
        if (newBuf.X < cols) newBuf.X = (SHORT)cols;
        if (newBuf.Y < rows) newBuf.Y = (SHORT)rows;

        if ((int)csbi.dwSize.X < cols || (int)csbi.dwSize.Y < rows) {
            if (!SetConsoleScreenBufferSize(hOut, newBuf)) {
                log_debug("SetConsoleScreenBufferSize (expand) failed in ForceConsoleWindowSizePixels.");
            } else {
                // refresh csbi
                GetConsoleScreenBufferInfo(hOut, &csbi);
            }
        }

        SMALL_RECT winRect;
        winRect.Left = 0;
        winRect.Top = 0;
        winRect.Right = (SHORT)(cols - 1);
        winRect.Bottom = (SHORT)(rows - 1);

        if (!SetConsoleWindowInfo(hOut, TRUE, &winRect)) {
            // Try retry strategy: expand buffer then retry
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "SetConsoleWindowInfo failed; buffer %d x %d", csbi.dwSize.X, csbi.dwSize.Y);
            log_debug(tmp);

            COORD ensureBuf = csbi.dwSize;
            if (ensureBuf.X < newBuf.X) ensureBuf.X = newBuf.X;
            if (ensureBuf.Y < newBuf.Y) ensureBuf.Y = newBuf.Y;
            if (!SetConsoleScreenBufferSize(hOut, ensureBuf)) {
                log_debug("SetConsoleScreenBufferSize retry failed in ForceConsoleWindowSizePixels.");
            } else {
                if (!SetConsoleWindowInfo(hOut, TRUE, &winRect)) {
                    log_debug("SetConsoleWindowInfo still failed after buffer resize in ForceConsoleWindowSizePixels.");
                }
            }
        }
    }

    // Finally set outer window pixel size (may be clamped by OS)
    SetWindowPos(hwnd, NULL, 0, 0, targetWidthPx, targetHeightPx,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

// --- Print art.txt trying to resize the console to fit (best-effort)
static void print_art_from_file_resize(const char *fname) {
    FILE *f = fopen(fname, "rb");
    if (!f) return;

    size_t maxWidth = 0;
    size_t lines = 0;
    char linebuf[4096];
    while (fgets(linebuf, sizeof(linebuf), f)) {
        size_t len = strlen(linebuf);
        while (len && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r')) { linebuf[--len] = '\0'; }
        if (len > maxWidth) maxWidth = len;
        lines++;
    }

    // Choose desired pixel dims — but we already will force 1000x800 earlier.
    // Here compute desired cols/rows for console window adjustments instead.
    int desiredCols = (int)(maxWidth ? maxWidth : 80);
    int desiredRows = (int)(lines ? lines + 4 : 24);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) { fclose(f); return; }

    SetConsoleOutputCP(CP_UTF8);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) {
        // fallback to printing art
        fseek(f, 0, SEEK_SET);
        while (fgets(linebuf, sizeof(linebuf), f)) fputs(linebuf, stdout);
        fclose(f);
        return;
    }

    // If buffer smaller than needed, expand
    COORD newBuf = csbi.dwSize;
    if (newBuf.X < desiredCols) newBuf.X = (SHORT)desiredCols;
    if (newBuf.Y < desiredRows) newBuf.Y = (SHORT)desiredRows;
    if ((int)csbi.dwSize.X < desiredCols || (int)csbi.dwSize.Y < desiredRows) {
        if (!SetConsoleScreenBufferSize(hOut, newBuf)) {
            log_debug("SetConsoleScreenBufferSize failed in print_art_from_file_resize.");
            // continue anyway
        } else {
            GetConsoleScreenBufferInfo(hOut, &csbi);
        }
    }

    // Try set window rect
    SMALL_RECT winRect;
    winRect.Left = 0;
    winRect.Top = 0;
    winRect.Right = (SHORT)(desiredCols - 1);
    winRect.Bottom = (SHORT)(desiredRows - 1);
    if (!SetConsoleWindowInfo(hOut, TRUE, &winRect)) {
        log_debug("SetConsoleWindowInfo failed in print_art_from_file_resize.");
    }

    // Print art from start
    fseek(f, 0, SEEK_SET);
    while (fgets(linebuf, sizeof(linebuf), f)) fputs(linebuf, stdout);

    fclose(f);
    puts(""); // spacing after art
}

// trim newline(s)
static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

// remove spaces and uppercase into dst
static void remove_spaces_and_upper(char *dst, const char *src, size_t dstsz) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dstsz; ++i) {
        if (!isspace((unsigned char)src[i])) {
            dst[j++] = (char)toupper((unsigned char)src[i]);
        }
    }
    dst[j] = '\0';
}

int main(void) {
    char buf[1024];

    (void)secret_hidden; // keep the literal in the binary

    // Force console window pixel size first (best-effort)
    ForceConsoleWindowSizePixels(1450, 800);

    // Print ascii art if present (attempt resize to accommodate)
    print_art_from_file_resize("art.txt");

    printf("Enter code (type the hex bytes, spaces allowed):\n> ");
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    trim_newline(buf);

    char compact[1024];
    remove_spaces_and_upper(compact, buf, sizeof(compact));

    if (strcmp(compact, "4E414F52") == 0) {
        puts("Cortex control regained, hack stopped.");
        const char *pipename = "\\\\.\\pipe\\LabMonitorPipe_v1";
        if (WaitNamedPipeA(pipename, 1500)) {
            HANDLE h = CreateFileA(pipename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                const char msg[] = "STOP";
                DWORD written = 0;
                WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
                CloseHandle(h);
                puts("Sent STOP to monitor.");
            } else {
                puts("Couldn't open monitor pipe. Monitor might not be running.");
            }
        } else {
            puts("Monitor pipe not available (timed out).");
        }
    } else {
        puts("Access denied. Try again.");
    }

    puts("Press ENTER to exit.");
    getchar();
    return 0;
}
