/*
 * WinKill v3 - Emulazione xkill per Windows 10/11
 * Usa IUIAutomation per identificare le app nella taskbar (Win10 e Win11).
 * Mantiene il metodo toolbar (TB_GETBUTTON) come secondo fallback.
 *
 * Compilazione MinGW (mingw-w64, 64-bit):
 *   g++ -o winkill.exe winkill.cpp -lgdi32 -luser32 -lole32 -luiautomationcore -mwindows -std=c++17
 *
 * Compilazione MSVC (Developer Command Prompt):
 *   cl winkill.cpp /std:c++17 /link user32.lib gdi32.lib ole32.lib uiautomationcore.lib /SUBSYSTEM:WINDOWS
 *
 * NOTA: per terminare processi protetti esegui come Amministratore.
 */

#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <uiautomation.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <ole2.h>
#include <string>
#include <vector>
#include <algorithm>

 // ─────────────────────────────────────────────────────────────────────────────
 // TBBUTTON 64-bit per accesso cross-process alla toolbar Win10
 // ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push,1)
struct TBBUTTON64 {
    INT       iBitmap;
    INT       idCommand;
    BYTE      fsState;
    BYTE      fsStyle;
    BYTE      bReserved[6];
    DWORD_PTR dwData;
    INT_PTR   iString;
};
#pragma pack(pop)

// ─── Globals ─────────────────────────────────────────────────────────────────
static HCURSOR        g_hCross = NULL;
static HWND           g_hOverlay = NULL;
static IUIAutomation* g_pUIA = NULL;

// ─── Utility: nome processo da PID ───────────────────────────────────────────
static std::wstring ProcName(DWORD pid)
{
    std::wstring name = L"(sconosciuto)";
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return name;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe))
        do { if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; } } while (Process32NextW(hSnap, &pe));
    CloseHandle(hSnap);
    return name;
}

static bool IsChildOf(HWND child, HWND parent)
{
    for (HWND w = child; w; w = GetParent(w))
        if (w == parent) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper UIA: rilascia automaticamente un IUIAutomationElement
// ─────────────────────────────────────────────────────────────────────────────
struct UIAElem {
    IUIAutomationElement* p = nullptr;
    ~UIAElem() { if (p) p->Release(); }
    IUIAutomationElement** operator&() { return &p; }
    IUIAutomationElement* operator->() { return p; }
    operator bool() const { return p != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// METODO 1 — IUIAutomation
//
// Partendo dall'elemento UIA sotto il cursore, risaliamo l'albero cercando
// un elemento di tipo Button che abbia un nome (= titolo della finestra).
// Poi cerchiamo la finestra top-level corrispondente tramite EnumWindows.
// ─────────────────────────────────────────────────────────────────────────────

// Raccoglie tutte le top-level windows visibili (esclude explorer/shell)
struct WinEntry { HWND hw; std::wstring title; std::wstring proc; };
static std::vector<WinEntry> g_topWindows;

static BOOL CALLBACK CollectWindows(HWND hw, LPARAM)
{
    if (!IsWindowVisible(hw)) return TRUE;
    if (GetWindowLong(hw, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    std::wstring pname = ProcName(pid);
    if (_wcsicmp(pname.c_str(), L"explorer.exe") == 0) return TRUE;

    WCHAR buf[512] = {};
    GetWindowText(hw, buf, 512);
    if (!wcslen(buf)) return TRUE;

    g_topWindows.push_back({ hw, buf, pname });
    return TRUE;
}

// Cerca la finestra top-level il cui titolo corrisponde (anche parzialmente)
// al nome del bottone UIA
static HWND MatchWindowByName(const std::wstring& btnName)
{
    if (btnName.empty()) return NULL;

    g_topWindows.clear();
    EnumWindows(CollectWindows, 0);

    // 1. Match esatto
    for (auto& e : g_topWindows)
        if (e.title == btnName) return e.hw;

    // 2. Il titolo della finestra contiene il nome del bottone
    for (auto& e : g_topWindows)
        if (e.title.find(btnName) != std::wstring::npos) return e.hw;

    // 3. Il nome del bottone contiene il titolo della finestra
    for (auto& e : g_topWindows)
        if (!e.title.empty() && btnName.find(e.title) != std::wstring::npos) return e.hw;

    // 4. Match parziale sui primi 20 caratteri
    size_t nchar = 20;
    size_t btnsize = btnName.size();
	size_t substrsize = min(nchar, btnsize);
    std::wstring prefix = btnName.substr(0, substrsize);
    for (auto& e : g_topWindows)
        if (e.title.find(prefix) != std::wstring::npos) return e.hw;

    return NULL;
}

static HWND FindTaskbarAppViaUIA(POINT pt)
{
    if (!g_pUIA) return NULL;

    UIAElem pEl;
    if (FAILED(g_pUIA->ElementFromPoint({ pt.x, pt.y }, &pEl.p)) || !pEl) return NULL;

    IUIAutomationTreeWalker* pWalker = NULL;
    g_pUIA->get_RawViewWalker(&pWalker);
    if (!pWalker) return NULL;

    HWND result = NULL;
    IUIAutomationElement* pCurrent = pEl.p; pCurrent->AddRef();

    for (int depth = 0; depth < 20 && pCurrent && !result; ++depth)
    {
        CONTROLTYPEID ct = 0;
        pCurrent->get_CurrentControlType(&ct);

        if (ct == UIA_ButtonControlTypeId)
        {
            BSTR bname = NULL;
            pCurrent->get_CurrentName(&bname);
            std::wstring name = bname ? bname : L"";
            SysFreeString(bname);

            if (!name.empty()) {
                result = MatchWindowByName(name);
            }
        }

        // Risali al parent
        IUIAutomationElement* pParent = NULL;
        HRESULT hr = pWalker->GetParentElement(pCurrent, &pParent);
        pCurrent->Release(); pCurrent = NULL;
        if (FAILED(hr) || !pParent) break;
        pCurrent = pParent;
    }

    if (pCurrent) pCurrent->Release();
    pWalker->Release();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// METODO 2 — Toolbar cross-process (fallback Win10)
// ─────────────────────────────────────────────────────────────────────────────
static HWND FindTaskbarAppViaToolbar(POINT pt)
{
    HWND hTray = FindWindow(L"Shell_TrayWnd", NULL);
    if (!hTray) return NULL;
    HWND hRebar = FindWindowEx(hTray, NULL, L"ReBarWindow32", NULL);
    HWND hSwWnd = FindWindowEx(hRebar, NULL, L"MSTaskSwWClass", NULL);
    HWND hList = FindWindowEx(hSwWnd, NULL, L"MSTaskListWClass", NULL);
    if (!hList) return NULL;

    DWORD expPid = 0; GetWindowThreadProcessId(hList, &expPid);
    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE, expPid);
    if (!hProc) return NULL;

    SIZE_T bufSize = max(sizeof(TBBUTTON64), sizeof(RECT));
    LPVOID remBuf = VirtualAllocEx(hProc, NULL, bufSize, MEM_COMMIT, PAGE_READWRITE);
    if (!remBuf) { CloseHandle(hProc); return NULL; }

    int   count = (int)SendMessage(hList, TB_BUTTONCOUNT, 0, 0);
    HWND  result = NULL;

    for (int i = 0; i < count && !result; ++i)
    {
        SendMessage(hList, TB_GETBUTTON, (WPARAM)i, (LPARAM)remBuf);
        TBBUTTON64 btn = {}; SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, remBuf, &btn, sizeof(btn), &rd) || rd != sizeof(btn))
            continue;

        SendMessage(hList, TB_GETITEMRECT, (WPARAM)i, (LPARAM)remBuf);
        RECT rc = {};
        if (!ReadProcessMemory(hProc, remBuf, &rc, sizeof(rc), &rd) || rd != sizeof(rc))
            continue;

        POINT org = { 0,0 }; ClientToScreen(hList, &org);
        rc.left += org.x; rc.right += org.x;
        rc.top += org.y; rc.bottom += org.y;
        if (!PtInRect(&rc, pt)) continue;

        HWND hw = NULL;
        if (btn.dwData &&
            ReadProcessMemory(hProc, (LPCVOID)btn.dwData, &hw, sizeof(hw), &rd) &&
            rd == sizeof(hw) && IsWindow(hw))
            result = hw;
    }

    VirtualFreeEx(hProc, remBuf, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return result;
}

// ─── Risolve il target finale ─────────────────────────────────────────────────
static HWND ResolveTarget(POINT pt)
{
    HWND hWin = WindowFromPoint(pt);
    if (!hWin) return NULL;

    HWND hTray = FindWindow(L"Shell_TrayWnd", NULL);
    HWND hTray2 = FindWindow(L"Shell_SecondaryTrayWnd", NULL);

    bool onTaskbar = (hWin == hTray || hWin == hTray2 ||
        IsChildOf(hWin, hTray) || IsChildOf(hWin, hTray2));

    if (onTaskbar) {
        // Metodo 1: IUIAutomation (Win10 + Win11)
        HWND app = FindTaskbarAppViaUIA(pt);
        if (app) return GetAncestor(app, GA_ROOT);

        // Metodo 2: toolbar cross-process (Win10 classico)
        app = FindTaskbarAppViaToolbar(pt);
        if (app) return GetAncestor(app, GA_ROOT);

        return NULL;
    }

    HWND top = GetAncestor(hWin, GA_ROOT);
    return (top == g_hOverlay) ? NULL : top;
}

// ─── Termina processo ─────────────────────────────────────────────────────────
static bool KillTarget(HWND hTarget, std::wstring& err)
{
    DWORD pid = 0; GetWindowThreadProcessId(hTarget, &pid);
    if (!pid) { err = L"Impossibile ottenere il PID."; return false; }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) {
        err = L"Impossibile aprire il processo (PID " + std::to_wstring(pid) +
            L").\nProva ad eseguire WinKill come Amministratore.";
        return false;
    }
    BOOL ok = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    if (!ok) { err = L"TerminateProcess fallita."; return false; }
    return true;
}

// ─── Disegna overlay ──────────────────────────────────────────────────────────
static void DrawOverlay(HDC hdc, RECT rc)
{
    SetBkMode(hdc, TRANSPARENT);
    const int bw = 580, bh = 96;
    int bx = (rc.right - bw) / 2, by = 18;

    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
    RECT box = { bx,by,bx + bw,by + bh }; FillRect(hdc, &box, bg); DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 2, RGB(200, 40, 40));
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, bx, by, bx + bw, by + bh);
    SelectObject(hdc, op); SelectObject(hdc, ob); DeleteObject(pen);

    HFONT fBig = CreateFont(23, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT fSm = CreateFont(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, fBig);

    SetTextColor(hdc, RGB(215, 50, 50));
    RECT r1 = { bx,by + 5,bx + bw,by + 34 }; DrawText(hdc, L"WinKill  v3  (UIA)", -1, &r1, DT_CENTER | DT_SINGLELINE);

    SelectObject(hdc, fSm);
    SetTextColor(hdc, RGB(200, 200, 200));
    RECT r2 = { bx,by + 36,bx + bw,by + 60 }; DrawText(hdc, L"Clicca su una finestra o sull'icona in taskbar da chiudere", -1, &r2, DT_CENTER | DT_SINGLELINE);
    RECT r3 = { bx,by + 62,bx + bw,by + 88 }; DrawText(hdc, L"Tasto destro o ESC per annullare", -1, &r3, DT_CENTER | DT_SINGLELINE);

    SelectObject(hdc, old); DeleteObject(fBig); DeleteObject(fSm);
}

// ─── Window procedure ─────────────────────────────────────────────────────────
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        SetCapture(hwnd); SetCursor(g_hCross); return 0;

    case WM_SETCURSOR:
        SetCursor(g_hCross); return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(42, 0, 0));
        FillRect(hdc, &rc, bg); DeleteObject(bg);
        DrawOverlay(hdc, rc);
        EndPaint(hwnd, &ps); return 0;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt; GetCursorPos(&pt);
        ShowWindow(hwnd, SW_HIDE);   // nascondi prima di hit-test

        HWND hTarget = ResolveTarget(pt);

        if (!hTarget) {
            HWND hTray = FindWindow(L"Shell_TrayWnd", NULL);
            RECT rcT = {}; GetWindowRect(hTray, &rcT);
            if (PtInRect(&rcT, pt))
                MessageBox(NULL,
                    L"Nessuna applicazione identificata in quella posizione.\n\n"
                    L"Suggerimenti:\n"
                    L"• Clicca direttamente sull'icona di un'app aperta.\n"
                    L"• Clicca sulla finestra dell'app anziché sull'icona taskbar.\n"
                    L"• Esegui WinKill come Amministratore per app elevate.",
                    L"WinKill — Nessun target", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            ShowWindow(hwnd, SW_SHOW);
            return 0;
        }

        DWORD pid = 0; GetWindowThreadProcessId(hTarget, &pid);
        std::wstring proc = ProcName(pid);
        WCHAR title[512] = L"(senza titolo)"; GetWindowText(hTarget, title, 512);

        WCHAR confirm[900];
        wsprintf(confirm,
            L"Vuoi terminare questa applicazione?\n\n"
            L"Finestra : %s\n"
            L"Processo : %s  (PID %lu)\n\n"
            L"L'applicazione verrà chiusa forzatamente.",
            title, proc.c_str(), pid);

        int res = MessageBox(NULL, confirm, L"WinKill — Conferma",
            MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        if (res == IDYES) {
            std::wstring err;
            if (!KillTarget(hTarget, err))
                MessageBox(NULL, err.c_str(), L"WinKill — Errore",
                    MB_OK | MB_ICONERROR | MB_TOPMOST);
        }

        ReleaseCapture(); DestroyWindow(hwnd); return 0;
    }

    case WM_RBUTTONDOWN:
        ReleaseCapture(); DestroyWindow(hwnd); return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { ReleaseCapture(); DestroyWindow(hwnd); }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─── WinMain ──────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    // COM richiesto da IUIAutomation
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Inizializza IUIAutomation
    CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), (void**)&g_pUIA);

    g_hCross = LoadCursor(NULL, IDC_CROSS);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.hCursor = g_hCross;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"WinKillOverlay";
    RegisterClassEx(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_hOverlay = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"WinKillOverlay", L"WinKill",
        WS_POPUP,
        vx, vy, vw, vh,
        NULL, NULL, hInst, NULL);

    if (!g_hOverlay) { CoUninitialize(); return 1; }

    SetLayeredWindowAttributes(g_hOverlay, 0, 175, LWA_ALPHA);
    ShowWindow(g_hOverlay, SW_SHOW);
    UpdateWindow(g_hOverlay);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }

    if (g_pUIA) { g_pUIA->Release(); g_pUIA = NULL; }
    CoUninitialize();
    return 0;
}