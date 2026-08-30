// llama-tray.c — Native Asynchronous System Tray with Real Graphical Floating Progress HUD & Active Model Indicator
// Build: gcc -O2 -DUNICODE -D_UNICODE "-Wl,-subsystem,windows" -o llama-tray.exe tray.c resource.res -lwinhttp -lshell32 -lgdi32 -luser32 -lcomctl32 -lkernel32 -lole32 -lcomdlg32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <objbase.h>

#define WM_TRAY            (WM_APP + 1)
#define WM_HUD_UPDATE      (WM_APP + 2)
#define WM_SCAN_COMPLETE   (WM_APP + 3)

#define IDM_OPEN           1001
#define IDM_UNLOAD         1003
#define IDM_RESTART        1004
#define IDM_EXIT           1005
#define IDM_RESCAN         1007

static HINSTANCE g_hInst;
static HWND      g_hwnd = NULL;
static HWND      g_hwndHUD = NULL;
static NOTIFYICONDATAW g_nid;
static UINT      g_taskbarCreated;
static HANDLE    g_job = NULL;
static DWORD     g_swapPid = 0;
static wchar_t   g_base[MAX_PATH];
static wchar_t   g_swapExe[MAX_PATH];
static wchar_t   g_cfg[MAX_PATH];
static wchar_t   g_modelsDir[MAX_PATH] = L"F:\\llama.cpp\\models";

// HUD State
static wchar_t   g_hudTitle[128]    = L"Loading Model...";
static wchar_t   g_hudSubtitle[128] = L"Transferring tensor weights to VRAM...";
static volatile int g_hudProgress   = 0;
static volatile int g_hudVisible    = 0;

static wchar_t   g_status[128]      = L"Ready (Idle)";
static wchar_t   g_activeModel[64]  = {0};
static wchar_t   g_tip[128]         = L"llama.cpp tray";
static wchar_t   g_ram[32]          = L"RAM ?";
static volatile LONG g_isScanning   = 0;

#define MAX_MODELS 64
static wchar_t   g_models[MAX_MODELS][64];
static int       g_modelCount = 0;
static const int IDM_MODEL_BASE = 2000;

typedef struct ScanResult {
    wchar_t models[MAX_MODELS][64];
    int count;
} ScanResult;

typedef struct HUDMsg {
    wchar_t title[128];
    wchar_t subtitle[128];
    int     progress;
    int     auto_hide_ms;
} HUDMsg;

static void compute_paths(void) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(g_base, exe);
    wchar_t *p = wcsrchr(g_base, L'\\');
    if (p) *p = 0;
    wsprintfW(g_swapExe, L"%ls\\bin\\llama-swap.exe", g_base);
    wsprintfW(g_cfg, L"%ls\\config.yaml", g_base);
}

static void wstr_tolower(wchar_t *s) {
    for (; *s; s++) *s = (wchar_t)towlower(*s);
}

static void derive_clean_key(const wchar_t *fname, char *out, int outsz) {
    wchar_t tmp[MAX_PATH];
    wcscpy(tmp, fname);
    wstr_tolower(tmp);
    wchar_t *dot = wcsrchr(tmp, L'.');
    if (dot && wcscmp(dot, L".gguf") == 0) *dot = 0;

    int j = 0;
    for (int i = 0; tmp[i] && j < outsz - 1; i++) {
        wchar_t c = tmp[i];
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.') {
            out[j++] = (char)c;
        } else {
            if (j > 0 && out[j-1] != '-') out[j++] = '-';
        }
    }
    if (j == 0) { out[0] = 'm'; j = 1; }
    out[j] = 0;
}

// -------------------------------------------------------------
// Graphical Progress HUD Window (Native Double-Buffered GDI)
// -------------------------------------------------------------
static void PositionHUD(HWND hwnd) {
    RECT rcWork;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int w = 380, h = 96;
    int x = rcWork.right - w - 16;
    int y = rcWork.bottom - h - 16;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static LRESULT CALLBACK HUDWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

        HBRUSH bgBrush = CreateSolidBrush(RGB(24, 26, 32));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(55, 60, 75));
        HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
        HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        SelectObject(memDC, nullBrush);
        Rectangle(memDC, 0, 0, rc.right, rc.bottom);
        SelectObject(memDC, oldPen);
        DeleteObject(borderPen);

        SetBkMode(memDC, TRANSPARENT);

        HFONT hTitleFont = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(memDC, hTitleFont);
        SetTextColor(memDC, RGB(245, 245, 245));
        RECT rTitle = {16, 12, rc.right - 70, 32};
        DrawTextW(memDC, g_hudTitle, -1, &rTitle, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        wchar_t pctText[16];
        wsprintfW(pctText, L"%d%%", g_hudProgress);
        RECT rPct = {rc.right - 64, 12, rc.right - 16, 32};
        SetTextColor(memDC, RGB(90, 165, 255));
        DrawTextW(memDC, pctText, -1, &rPct, DT_RIGHT | DT_SINGLELINE);

        RECT rBarTrack = {16, 38, rc.right - 16, 48};
        HBRUSH trackBrush = CreateSolidBrush(RGB(40, 44, 56));
        FillRect(memDC, &rBarTrack, trackBrush);
        DeleteObject(trackBrush);

        int barWidth = rc.right - 32;
        int fillWidth = (barWidth * g_hudProgress) / 100;
        if (fillWidth > 0) {
            RECT rBarFill = {16, 38, 16 + fillWidth, 48};
            HBRUSH fillBrush = CreateSolidBrush(g_hudProgress >= 100 ? RGB(46, 204, 113) : RGB(58, 140, 255));
            FillRect(memDC, &rBarFill, fillBrush);
            DeleteObject(fillBrush);
        }

        HFONT hSubFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SelectObject(memDC, hSubFont);
        SetTextColor(memDC, RGB(160, 165, 180));
        RECT rSub = {16, 56, rc.right - 16, 80};
        DrawTextW(memDC, g_hudSubtitle, -1, &rSub, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(memDC, oldFont);
        DeleteObject(hTitleFont);
        DeleteObject(hSubFont);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 999) {
            KillTimer(hwnd, 999);
            ShowWindow(hwnd, SW_HIDE);
            g_hudVisible = 0;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowHUD(const wchar_t *title, const wchar_t *subtitle, int progress, int auto_hide_ms) {
    if (!g_hwndHUD) return;
    if (title) wcscpy(g_hudTitle, title);
    if (subtitle) wcscpy(g_hudSubtitle, subtitle);
    g_hudProgress = (progress < 0) ? 0 : (progress > 100 ? 100 : progress);

    PositionHUD(g_hwndHUD);
    ShowWindow(g_hwndHUD, SW_SHOWNOACTIVATE);
    InvalidateRect(g_hwndHUD, NULL, FALSE);
    UpdateWindow(g_hwndHUD);
    g_hudVisible = 1;

    KillTimer(g_hwndHUD, 999);
    if (auto_hide_ms > 0) {
        SetTimer(g_hwndHUD, 999, auto_hide_ms, NULL);
    }
}

static HICON MakeIcon(COLORREF bg, COLORREF fg, const wchar_t *txt) {
    const int s = 32;
    HDC hdc = GetDC(NULL);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP hbm = CreateCompatibleBitmap(hdc, s, s);
    HBITMAP hmask = CreateBitmap(s, s, 1, 1, NULL);
    HBRUSH br = CreateSolidBrush(bg);
    HBRUSH obr = (HBRUSH)SelectObject(mem, hbm);
    RECT r = {0, 0, s, s};
    FillRect(mem, &r, br);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, fg);
    HFONT f = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT of = (HFONT)SelectObject(mem, f);
    DrawTextW(mem, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, obr); SelectObject(mem, of);
    HDC mm = CreateCompatibleDC(hdc);
    HBITMAP om = (HBITMAP)SelectObject(mm, hmask);
    PatBlt(mm, 0, 0, s, s, BLACKNESS);
    SelectObject(mm, om);
    ICONINFO ii = {0};
    ii.fIcon = TRUE; ii.hbmMask = hmask; ii.hbmColor = hbm;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(br); DeleteObject(f); DeleteObject(hbm); DeleteObject(hmask);
    DeleteDC(mem); DeleteDC(mm); ReleaseDC(NULL, hdc);
    return icon;
}

static void fast_kill_process(DWORD pid) {
    if (!pid) return;
    wchar_t cmd[128];
    wsprintfW(cmd, L"taskkill /F /T /PID %u", pid);
    STARTUPINFOW si = {0}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void start_swap(void) {
    if (g_swapPid) return;
    g_job = CreateJobObjectW(NULL, NULL);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    wchar_t cmd[MAX_PATH * 2];
    wsprintfW(cmd, L"\"%ls\" --config \"%ls\" --listen 127.0.0.1:8888", g_swapExe, g_cfg);
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_base, &si, &pi)) {
        g_swapPid = pi.dwProcessId;
        if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void stop_swap(void) {
    if (g_swapPid) {
        fast_kill_process(g_swapPid);
        g_swapPid = 0;
    }
    if (g_job) { CloseHandle(g_job); g_job = NULL; }
}

static int http_req(const char *verb, const char *path, const char *body) {
    HINTERNET hS = WinHttpOpen(L"llama-tray/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hS) return -1;
    WinHttpSetTimeouts(hS, 500, 800, 800, 800);
    HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", 8888, 0);
    if (!hC) { WinHttpCloseHandle(hS); return -1; }
    wchar_t wpath[256], wverb[16];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 256);
    MultiByteToWideChar(CP_UTF8, 0, verb, -1, wverb, 16);
    HINTERNET hR = WinHttpOpenRequest(hC, wverb, wpath, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return -1; }
    DWORD blen = body ? (DWORD)strlen(body) : 0;
    if (!WinHttpSendRequest(hR, L"Content-Type: application/json\r\n", (DWORD)-1,
                            (LPVOID)(body ? body : ""), blen, blen, 0)) {
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return -1;
    }
    if (!WinHttpReceiveResponse(hR, NULL)) {
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return -1;
    }
    DWORD status = 0, siz = sizeof(status);
    WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &siz, NULL);
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return (int)status;
}

static void update_running_model_from_api(void) {
    HINTERNET hS = WinHttpOpen(L"llama-tray-running/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hS) return;
    WinHttpSetTimeouts(hS, 300, 300, 300, 300);
    HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", 8888, 0);
    if (!hC) { WinHttpCloseHandle(hS); return; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", L"/running", NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return; }
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, NULL)) {
        char buf[512] = {0}; DWORD read = 0;
        if (WinHttpReadData(hR, buf, sizeof(buf) - 1, &read) && read > 0) {
            buf[read] = 0;
            char *p = strstr(buf, "[\"");
            if (!p) p = strstr(buf, "\"model\":\"");
            if (p) {
                p = strchr(p, ':') ? (strchr(p, ':') + 2) : (p + 2);
                char *end = strchr(p, '"');
                if (end) {
                    *end = 0;
                    MultiByteToWideChar(CP_UTF8, 0, p, -1, g_activeModel, 64);
                } else {
                    g_activeModel[0] = 0;
                }
            } else {
                g_activeModel[0] = 0;
            }
        }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
}

static void get_sys_ram(void) {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) wsprintfW(g_ram, L"RAM %u%%", ms.dwMemoryLoad);
    else wcscpy(g_ram, L"RAM ?");
}

static void update_tray_tip(void) {
    if (!g_hwnd) return;
    if (InterlockedCompareExchange(&g_isScanning, 0, 0) == 1) {
        wsprintfW(g_tip, L"llama.cpp · [%d%% Scanning] · %ls", g_hudProgress, g_hudSubtitle);
    } else {
        if (g_activeModel[0]) {
            wsprintfW(g_tip, L"llama.cpp · [Active: %ls] · %ls", g_activeModel, g_ram);
        } else {
            wsprintfW(g_tip, L"llama.cpp · [Idle] · %ls", g_ram);
        }
    }
    g_tip[127] = 0;
    g_nid.uFlags = NIF_TIP | NIF_ICON | NIF_MESSAGE;
    wcscpy(g_nid.szTip, g_tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// -------------------------------------------------------------
// Real-Time Log & Loading Progress Stream Monitor Thread
// -------------------------------------------------------------
static DWORD WINAPI LogStreamMonitorThread(LPVOID lpParam) {
    HWND hwnd = (HWND)lpParam;
    char buffer[4096];
    DWORD bytesRead = 0;

    while (1) {
        HINTERNET hS = WinHttpOpen(L"llama-tray-logmon/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
        if (!hS) { Sleep(2000); continue; }
        WinHttpSetTimeouts(hS, 2000, 3000, 0, 0);

        HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", 8888, 0);
        if (!hC) { WinHttpCloseHandle(hS); Sleep(2000); continue; }

        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", L"/logs/stream", NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); Sleep(2000); continue; }

        if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hR, NULL)) {
            WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
            Sleep(2000);
            continue;
        }

        while (WinHttpReadData(hR, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = 0;
            
            if (strstr(buffer, "loading model") || strstr(buffer, "load_tensors") || strstr(buffer, "load_model:")) {
                char *pct_pos = strstr(buffer, "%");
                int pct = 50;
                if (pct_pos && pct_pos > buffer + 3) {
                    char pct_str[8] = {0};
                    int k = 0;
                    char *p = pct_pos - 1;
                    while (p >= buffer && (isdigit((unsigned char)*p) || *p == '.') && k < 6) {
                        pct_str[k++] = *p;
                        p--;
                    }
                    if (k > 0) {
                        for (int l = 0; l < k / 2; l++) {
                            char t = pct_str[l]; pct_str[l] = pct_str[k-1-l]; pct_str[k-1-l] = t;
                        }
                        pct = atoi(pct_str);
                    }
                }
                
                HUDMsg *hm = (HUDMsg*)malloc(sizeof(HUDMsg));
                if (hm) {
                    wsprintfW(hm->title, L"Loading Model into VRAM...");
                    wsprintfW(hm->subtitle, L"Transferring tensor weights: %d%%", pct);
                    hm->progress = pct;
                    hm->auto_hide_ms = 0;
                    PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)hm, 0);
                }
            }
            else if (strstr(buffer, "server is listening") || strstr(buffer, "HTTP server listening") || strstr(buffer, "model loaded") || strstr(buffer, "all slots are idle")) {
                update_running_model_from_api();
                HUDMsg *hm = (HUDMsg*)malloc(sizeof(HUDMsg));
                if (hm) {
                    wsprintfW(hm->title, L"🚀 %ls Ready", g_activeModel[0] ? g_activeModel : L"Model");
                    wsprintfW(hm->subtitle, L"Model is 100%% active in GPU VRAM.");
                    hm->progress = 100;
                    hm->auto_hide_ms = 1800;
                    PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)hm, 0);
                }
                wcscpy(g_status, L"🟢 Active (Ready)");
                update_tray_tip();
            }
            else if (strstr(buffer, "unloading model") || strstr(buffer, "model unloaded")) {
                g_activeModel[0] = 0;
                HUDMsg *hm = (HUDMsg*)malloc(sizeof(HUDMsg));
                if (hm) {
                    wsprintfW(hm->title, L"💤 VRAM Released (Idle)");
                    wsprintfW(hm->subtitle, L"Idle timeout reached. GPU memory cleared.");
                    hm->progress = 0;
                    hm->auto_hide_ms = 1800;
                    PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)hm, 0);
                }
                wcscpy(g_status, L"Ready (Idle)");
                update_tray_tip();
            }
        }

        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        Sleep(1000);
    }
    return 0;
}

// -------------------------------------------------------------
// Asynchronous Background Model Scanner
// -------------------------------------------------------------
typedef struct ModelFileInfo {
    wchar_t fname[MAX_PATH];
    wchar_t fullpath[MAX_PATH];
    UINT64  size;
    int     is_assistant;
    int     is_mmproj;
} ModelFileInfo;

static DWORD WINAPI WorkerScanThread(LPVOID lpParam) {
    HWND hwnd = (HWND)lpParam;
    
    HUDMsg *h1 = (HUDMsg*)malloc(sizeof(HUDMsg));
    if (h1) {
        wsprintfW(h1->title, L"Scanning Model Library...");
        wsprintfW(h1->subtitle, L"Enumerating F:\\llama.cpp\\models...");
        h1->progress = 20;
        h1->auto_hide_ms = 0;
        PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)h1, 0);
    }

    wchar_t search_pattern[MAX_PATH];
    wsprintfW(search_pattern, L"%ls\\*.gguf", g_modelsDir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_isScanning, 0);
        return 0;
    }

    ModelFileInfo files[128];
    int file_count = 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (file_count >= 128) break;

        wcscpy(files[file_count].fname, fd.cFileName);
        wsprintfW(files[file_count].fullpath, L"%ls\\%ls", g_modelsDir, fd.cFileName);
        files[file_count].size = ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;

        wchar_t lower[MAX_PATH];
        wcscpy(lower, fd.cFileName);
        wstr_tolower(lower);

        files[file_count].is_assistant = (wcsstr(lower, L"assistant") != NULL || wcsstr(lower, L"drafter") != NULL);
        files[file_count].is_mmproj = (wcsstr(lower, L"mmproj") != NULL);

        file_count++;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    HUDMsg *h2 = (HUDMsg*)malloc(sizeof(HUDMsg));
    if (h2) {
        wsprintfW(h2->title, L"Matching Architectures & Drafters...");
        wsprintfW(h2->subtitle, L"Detected %d models. Applying optimal flags...", file_count);
        h2->progress = 60;
        h2->auto_hide_ms = 0;
        PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)h2, 0);
    }

    wchar_t general_mmproj[MAX_PATH] = {0};
    for (int i = 0; i < file_count; i++) {
        if (files[i].is_mmproj) {
            wcscpy(general_mmproj, files[i].fullpath);
            break;
        }
    }

    FILE *f = _wfopen(g_cfg, L"wb");
    if (f) {
        fprintf(f, "# llama-swap config — Auto-Generated with Dynamic Rules\n");
        fprintf(f, "listen: 127.0.0.1:8888\n");
        fprintf(f, "healthCheckTimeout: 180\n");
        fprintf(f, "startPort: 15800\n");
        fprintf(f, "logLevel: info\n\n");
        fprintf(f, "models:\n");

        for (int i = 0; i < file_count; i++) {
            if (files[i].is_assistant || files[i].is_mmproj) continue;

            char key[64];
            derive_clean_key(files[i].fname, key, sizeof(key));

            wchar_t lower[MAX_PATH];
            wcscpy(lower, files[i].fname);
            wstr_tolower(lower);

            char model_path_u[MAX_PATH * 2];
            WideCharToMultiByte(CP_UTF8, 0, files[i].fullpath, -1, model_path_u, sizeof(model_path_u), NULL, NULL);
            for (char *p = model_path_u; *p; p++) if (*p == '\\') *p = '/';

            wchar_t matched_assistant[MAX_PATH] = {0};
            for (int a = 0; a < file_count; a++) {
                if (!files[a].is_assistant) continue;
                wchar_t a_lower[MAX_PATH];
                wcscpy(a_lower, files[a].fname);
                wstr_tolower(a_lower);
                if (wcsstr(lower, L"e4b") && wcsstr(a_lower, L"e4b")) {
                    wcscpy(matched_assistant, files[a].fullpath);
                    break;
                }
            }

            fprintf(f, "  %s:\n", key);
            fprintf(f, "    ttl: 300\n");
            fprintf(f, "    proxy: http://127.0.0.1:${PORT}\n");
            fprintf(f, "    cmd: >-\n");
            fprintf(f, "      F:/llama.cpp/llama-server.exe -m \"%s\"", model_path_u);

            if (matched_assistant[0]) {
                char assist_path_u[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, matched_assistant, -1, assist_path_u, sizeof(assist_path_u), NULL, NULL);
                for (char *p = assist_path_u; *p; p++) if (*p == '\\') *p = '/';
                fprintf(f, "\n      --model-draft \"%s\" --spec-type draft-mtp", assist_path_u);
            } else if ((wcsstr(lower, L"qwen3.8-9b") || wcsstr(lower, L"qwen3.8_9b") || wcsstr(lower, L"mtp")) && !wcsstr(lower, L"turbo") && !wcsstr(lower, L"14b")) {
                fprintf(f, "\n      --spec-type draft-mtp");
            }

            if (general_mmproj[0] && (wcsstr(lower, L"gemma-4-12b") || wcsstr(lower, L"vision") || wcsstr(lower, L"vl"))) {
                char mm_path_u[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, general_mmproj, -1, mm_path_u, sizeof(mm_path_u), NULL, NULL);
                for (char *p = mm_path_u; *p; p++) if (*p == '\\') *p = '/';
                fprintf(f, "\n      --mmproj \"%s\"", mm_path_u);
            }

            if (files[i].size > (7ULL * 1024 * 1024 * 1024) || wcsstr(lower, L"14b") || wcsstr(lower, L"32b") || wcsstr(lower, L"70b")) {
                fprintf(f, "\n      --cache-type-k q8_0 --cache-type-v q8_0");
            }

            int ctx = 131072;
            int is_gemma = (wcsstr(lower, L"gemma") != NULL || wcsstr(lower, L"e4b") != NULL);
            const char *fa_str = is_gemma ? "--flash-attn off" : "--flash-attn on";

            fprintf(f, "\n      --host 127.0.0.1 --port ${PORT} --ctx-size %d --batch-size 4096 --ubatch-size 2048 --n-gpu-layers 99 %s --no-webui\n\n", ctx, fa_str);
        }
        fclose(f);
    }

    HUDMsg *h3 = (HUDMsg*)malloc(sizeof(HUDMsg));
    if (h3) {
        wsprintfW(h3->title, L"Starting llama-swap Router...");
        wsprintfW(h3->subtitle, L"Binding to 127.0.0.1:8888...");
        h3->progress = 85;
        h3->auto_hide_ms = 0;
        PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)h3, 0);
    }

    stop_swap();
    start_swap();

    ScanResult *res = (ScanResult*)malloc(sizeof(ScanResult));
    if (res) {
        res->count = 0;
        FILE *rf = _wfopen(g_cfg, L"rb");
        if (rf) {
            char line[512];
            int in_models = 0;
            while (fgets(line, sizeof(line), rf)) {
                size_t L = strlen(line);
                while (L && (line[L-1]=='\n' || line[L-1]=='\r')) line[--L]=0;
                const char *p = line;
                while (*p==' '||*p=='\t') p++;
                if (!in_models) {
                    if (strncmp(p, "models:", 7)==0 && (p[7]==' '||p[7]=='\t'||p[7]==0)) in_models = 1;
                    continue;
                }
                if (p == line && *p && strchr(p, ':')) break;
                int sp = 0; const char *q = line;
                while (*q==' ') { sp++; q++; }
                if (sp == 2 && res->count < MAX_MODELS) {
                    char *colon = strchr(q, ':');
                    if (colon && colon > q) {
                        size_t nlen = colon - q;
                        if (nlen > 0 && nlen < 64) {
                            char name[64];
                            memcpy(name, q, nlen); name[nlen] = 0;
                            if (!(isalnum((unsigned char)name[0]) || name[0]=='_' || name[0]=='-' || name[0]=='.')) continue;
                            MultiByteToWideChar(CP_UTF8, 0, name, -1, res->models[res->count], 64);
                            res->count++;
                        }
                    }
                }
            }
            fclose(rf);
        }
        PostMessageW(hwnd, WM_SCAN_COMPLETE, (WPARAM)res, 0);
    }

    HUDMsg *h4 = (HUDMsg*)malloc(sizeof(HUDMsg));
    if (h4) {
        wsprintfW(h4->title, L"✓ Ready");
        wsprintfW(h4->subtitle, L"All models configured for on-demand inference.");
        h4->progress = 100;
        h4->auto_hide_ms = 1500;
        PostMessageW(hwnd, WM_HUD_UPDATE, (WPARAM)h4, 0);
    }

    InterlockedExchange(&g_isScanning, 0);
    return 0;
}

static void trigger_async_scan(HWND hwnd) {
    if (InterlockedCompareExchange(&g_isScanning, 1, 0) == 0) {
        CreateThread(NULL, 0, WorkerScanThread, (LPVOID)hwnd, 0, NULL);
    }
}

typedef struct TriggerContext { char model[64]; } TriggerContext;
static DWORD WINAPI TriggerLoadThread(LPVOID lpParam) {
    TriggerContext *tc = (TriggerContext*)lpParam;
    if (tc) {
        wchar_t wmodel[64];
        MultiByteToWideChar(CP_UTF8, 0, tc->model, -1, wmodel, 64);
        wcscpy(g_activeModel, wmodel);

        HUDMsg *hm = (HUDMsg*)malloc(sizeof(HUDMsg));
        if (hm) {
            wsprintfW(hm->title, L"Loading: %ls", wmodel);
            wsprintfW(hm->subtitle, L"Launching llama-server & streaming weights to VRAM...");
            hm->progress = 25;
            hm->auto_hide_ms = 0;
            PostMessageW(g_hwnd, WM_HUD_UPDATE, (WPARAM)hm, 0);
        }

        // Dedicated HTTP client with generous 45-second timeout for model loading
        HINTERNET hS = WinHttpOpen(L"llama-tray-loader/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
        if (hS) {
            WinHttpSetTimeouts(hS, 5000, 5000, 45000, 45000);
            HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", 8888, 0);
            if (hC) {
                HINTERNET hR = WinHttpOpenRequest(hC, L"POST", L"/v1/chat/completions", NULL, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                if (hR) {
                    char req[320];
                    wsprintfA(req, "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"max_tokens\":1}", tc->model);
                    DWORD blen = (DWORD)strlen(req);
                    if (WinHttpSendRequest(hR, L"Content-Type: application/json\r\n", (DWORD)-1, (LPVOID)req, blen, blen, 0) &&
                        WinHttpReceiveResponse(hR, NULL)) {
                        DWORD status = 0, sz = sizeof(status);
                        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
                        if (status == 200) {
                            HUDMsg *hm_done = (HUDMsg*)malloc(sizeof(HUDMsg));
                            if (hm_done) {
                                wsprintfW(hm_done->title, L"🚀 %ls Ready", wmodel);
                                wsprintfW(hm_done->subtitle, L"Model is 100%% active in GPU VRAM.");
                                hm_done->progress = 100;
                                hm_done->auto_hide_ms = 1800;
                                PostMessageW(g_hwnd, WM_HUD_UPDATE, (WPARAM)hm_done, 0);
                            }
                            wcscpy(g_status, L"🟢 Active (Ready)");
                            update_tray_tip();
                        }
                    }
                    WinHttpCloseHandle(hR);
                }
                WinHttpCloseHandle(hC);
            }
            WinHttpCloseHandle(hS);
        }
        free(tc);
    }
    return 0;
}

static void trigger_load_async(const char *model_id) {
    TriggerContext *tc = (TriggerContext*)malloc(sizeof(TriggerContext));
    if (tc) {
        strncpy(tc->model, model_id, 63);
        tc->model[63] = 0;
        CreateThread(NULL, 0, TriggerLoadThread, (LPVOID)tc, 0, NULL);
    }
}

static DWORD WINAPI UnloadAllThread(LPVOID lpParam) {
    g_activeModel[0] = 0;
    HUDMsg *hm = (HUDMsg*)malloc(sizeof(HUDMsg));
    if (hm) {
        wsprintfW(hm->title, L"Unloading Models...");
        wsprintfW(hm->subtitle, L"Releasing GPU VRAM back to system...");
        hm->progress = 50;
        hm->auto_hide_ms = 0;
        PostMessageW(g_hwnd, WM_HUD_UPDATE, (WPARAM)hm, 0);
    }

    http_req("POST", "/api/models/unload", "");
    http_req("POST", "/unload", "");

    HUDMsg *hm2 = (HUDMsg*)malloc(sizeof(HUDMsg));
    if (hm2) {
        wsprintfW(hm2->title, L"✓ VRAM Cleared");
        wsprintfW(hm2->subtitle, L"All models unloaded. VRAM returned to 0 MB.");
        hm2->progress = 100;
        hm2->auto_hide_ms = 1500;
        PostMessageW(g_hwnd, WM_HUD_UPDATE, (WPARAM)hm2, 0);
    }
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &g_nid); return 0; }
    switch (msg) {
    case WM_HUD_UPDATE: {
        HUDMsg *hm = (HUDMsg*)wp;
        if (hm) {
            ShowHUD(hm->title, hm->subtitle, hm->progress, hm->auto_hide_ms);
            free(hm);
        }
        get_sys_ram();
        update_tray_tip();
        return 0;
    }
    case WM_SCAN_COMPLETE: {
        ScanResult *res = (ScanResult*)wp;
        if (res) {
            g_modelCount = res->count;
            for (int i = 0; i < g_modelCount; i++) {
                wcscpy(g_models[i], res->models[i]);
            }
            free(res);
        }
        wsprintfW(g_status, L"Ready (%d models)", g_modelCount);
        get_sys_ram();
        update_tray_tip();
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            update_running_model_from_api();
            
            HMENU m = CreatePopupMenu();
            
            wchar_t info[128];
            if (g_activeModel[0]) {
                wsprintfW(info, L"🟢 Active: %ls · %ls", g_activeModel, g_ram);
            } else {
                wsprintfW(info, L"💤 Idle (No model in VRAM) · %ls", g_ram);
            }
            AppendMenuW(m, MF_STRING | MF_DISABLED | MF_GRAYED, 0, info);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Web UI");
            
            if (g_modelCount > 0) {
                HMENU lm = CreatePopupMenu();
                for (int i = 0; i < g_modelCount; i++) {
                    wchar_t buf[128];
                    int is_active = (g_activeModel[0] && _wcsicmp(g_models[i], g_activeModel) == 0);
                    
                    if (is_active) {
                        wsprintfW(buf, L"✓ %ls  [Active]", g_models[i]);
                        AppendMenuW(lm, MF_STRING | MF_CHECKED, IDM_MODEL_BASE + i, buf);
                    } else {
                        wsprintfW(buf, L"   %ls", g_models[i]);
                        AppendMenuW(lm, MF_STRING | MF_UNCHECKED, IDM_MODEL_BASE + i, buf);
                    }
                }
                AppendMenuW(m, MF_POPUP, (UINT_PTR)lm, L"Select / Switch Model");
            }
            
            AppendMenuW(m, MF_STRING, IDM_RESCAN,  L"Rescan & Auto-Configure Models");
            AppendMenuW(m, MF_STRING, IDM_UNLOAD,  L"Unload All Models (Release VRAM)");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_RESTART, L"Restart Service");
            AppendMenuW(m, MF_STRING, IDM_EXIT,    L"Exit");
            
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
        } else if (lp == WM_LBUTTONDOWN || lp == WM_LBUTTONDBLCLK) {
            ShellExecuteW(NULL, L"open", L"http://127.0.0.1:8888/ui", NULL, NULL, SW_SHOW);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:    ShellExecuteW(NULL, L"open", L"http://127.0.0.1:8888/ui", NULL, NULL, SW_SHOW); break;
        case IDM_RESCAN:  trigger_async_scan(hwnd); break;
        case IDM_UNLOAD:  CreateThread(NULL, 0, UnloadAllThread, NULL, 0, NULL); break;
        case IDM_RESTART: trigger_async_scan(hwnd); break;
        case IDM_EXIT:    DestroyWindow(hwnd); break;
        default:
            if (LOWORD(wp) >= IDM_MODEL_BASE && LOWORD(wp) < IDM_MODEL_BASE + g_modelCount) {
                int idx = LOWORD(wp) - IDM_MODEL_BASE;
                char mb[64];
                WideCharToMultiByte(CP_UTF8, 0, g_models[idx], -1, mb, sizeof(mb), NULL, NULL);
                trigger_load_async(mb);
            }
            break;
        }
        return 0;
    case WM_TIMER:
        if (InterlockedCompareExchange(&g_isScanning, 0, 0) == 0) {
            get_sys_ram();
            update_tray_tip();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_hwndHUD) DestroyWindow(g_hwndHUD);
        stop_swap();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL InitInstance(HINSTANCE hInst) {
    g_hInst = hInst;
    
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.lpszClassName = L"LlamaTrayWClass";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"LlamaTrayWClass", L"llama.cpp tray",
                             WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return FALSE;

    WNDCLASSW wcHUD = {0};
    wcHUD.lpfnWndProc = HUDWndProc;
    wcHUD.hInstance   = hInst;
    wcHUD.hCursor     = LoadCursor(NULL, IDC_ARROW);
    wcHUD.lpszClassName = L"LlamaProgressHUD";
    RegisterClassW(&wcHUD);

    g_hwndHUD = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"LlamaProgressHUD", L"llama.cpp Progress",
                                WS_POPUP | WS_BORDER, 0, 0, 380, 96,
                                NULL, NULL, hInst, NULL);

    HICON icon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    if (!icon) icon = MakeIcon(RGB(24, 26, 34), RGB(90, 160, 255), L"LL");
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = icon;
    wcscpy(g_nid.szTip, L"llama.cpp tray");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    CreateThread(NULL, 0, LogStreamMonitorThread, (LPVOID)g_hwnd, 0, NULL);
    trigger_async_scan(g_hwnd);
    SetTimer(g_hwnd, 1, 5000, NULL);
    return TRUE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow) {
    (void)hPrev; (void)cmd; (void)nShow;

    HWND existing = FindWindowW(L"LlamaTrayWClass", NULL);
    if (existing) {
        PostMessageW(existing, WM_COMMAND, IDM_RESCAN, 0);
        return 0;
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\LlamaTraySingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    compute_paths();
    if (!InitInstance(hInst)) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
