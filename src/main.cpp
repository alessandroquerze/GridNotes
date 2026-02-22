#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#define BACKGROUND 0
#define TILE_COLOR 26

//#include "startup.h"

HFONT g_bigFont = nullptr;
void CreateGlobalFont()
{
    if (g_bigFont)
        return;

    LOGFONT lf{};
    GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);

    lf.lfHeight = (LONG)(lf.lfHeight * 1.6);

    g_bigFont = CreateFontIndirect(&lf);
}
static BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
    auto* v = reinterpret_cast<std::vector<HMONITOR>*>(lParam);
    v->push_back(hMon);
    return TRUE;
}

void CenterWindowOnSecondMonitor(HWND hwnd,int wid,int hei)
{
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, (LPARAM)&mons);

    // Se c'è un secondo monitor, usa quello.
    // Altrimenti usa il monitor primario (o quello più vicino alla finestra).
    HMONITOR target = nullptr;

    if (mons.size() >= 2) {
        target = mons[1];
    } else {
        target = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(target, &mi);

    // Usa rcWork per non finire sotto la taskbar
    const RECT& r = mi.rcWork;

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int w =wid;
    int h = hei;

    int x = r.left + ((r.right - r.left) - w) / 2;
    int y = r.top  + ((r.bottom - r.top) - h) / 2;

    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}
struct Tile {
    int x{0};
    int y{0};
    int w{4};
    int h{4};
    std::wstring text;
    HWND edit{};
};

struct AppState {
    int cellSize{48};
    bool editLayout{false};
    bool startWithWindows{false};
    int windowWidth{1008};
    int windowHeight{660};
    std::vector<Tile> tiles;
};

static constexpr UINT_PTR kTimerSaveDebounce = 1;
static constexpr UINT kSaveDebounceMs = 800;
static bool g_savePending = false;

constexpr wchar_t kAppName[] = L"GridNotes";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kToolbarHeight = 36;
constexpr int kEditPadding = 1;
constexpr int kResizeHandlePx = 8; // thickness in px for resize handles AND hit-test range
constexpr int kCmdEditLayout = 101;
constexpr int kCmdStartup = 102;

AppState g_state;
HWND g_board{};
HWND g_editToggle{};
HWND g_startupToggle{};
HBRUSH g_editBgBrush{};
HBRUSH g_toolbarBgBrush{};
WNDPROC g_defaultEditProc{};
bool g_internalTextSet{};
bool g_dragging{};
int g_dragTile{-1};
enum class DragEdge { None, Left, Right, Top, Bottom };
DragEdge g_dragEdge = DragEdge::None;
POINT g_dragStart{};
Tile g_originalTile{};

void SaveState();
void LayoutTiles();
void Split2(int idx, bool vertical);
void Split4(int idx);
bool TextFitsInEdit(HWND edit, const std::wstring& text);

static inline bool Overlap1D(int a0, int a1, int b0, int b1) {
    // intervalli half-open: [a0,a1) e [b0,b1)
    return a0 < b1 && b0 < a1;
}
/* helpers per il clamping!*/
int LimitRightEdgeCells(int idx, const Tile& orig, int desiredRight, int boardCellsX) {
    int limit = std::min(desiredRight, boardCellsX);
    const int origRight = orig.x + orig.w;

    for (int j = 0; j < (int)g_state.tiles.size(); ++j) {
        if (j == idx) continue;
        const Tile& o = g_state.tiles[j];

        // devono sovrapporsi verticalmente
        if (!Overlap1D(orig.y, orig.y + orig.h, o.y, o.y + o.h)) continue;

        // consideriamo solo ostacoli alla DESTRA del bordo originale
        if (o.x >= origRight) {
            limit = std::min(limit, o.x); // non puoi oltrepassare il left dell'ostacolo
        }
    }
    return limit;
}

int LimitBottomEdgeCells(int idx, const Tile& orig, int desiredBottom, int boardCellsY) {
    int limit = std::min(desiredBottom, boardCellsY);
    const int origBottom = orig.y + orig.h;

    for (int j = 0; j < (int)g_state.tiles.size(); ++j) {
        if (j == idx) continue;
        const Tile& o = g_state.tiles[j];

        // devono sovrapporsi orizzontalmente
        if (!Overlap1D(orig.x, orig.x + orig.w, o.x, o.x + o.w)) continue;

        // ostacoli sotto il bordo originale
        if (o.y >= origBottom) {
            limit = std::min(limit, o.y);
        }
    }
    return limit;
}

int LimitLeftEdgeCells(int idx, const Tile& orig, int desiredLeft) {
    int limit = std::max(desiredLeft, 0);
    const int origLeft = orig.x;

    for (int j = 0; j < (int)g_state.tiles.size(); ++j) {
        if (j == idx) continue;
        const Tile& o = g_state.tiles[j];

        if (!Overlap1D(orig.y, orig.y + orig.h, o.y, o.y + o.h)) continue;

        const int oRight = o.x + o.w;
        // ostacoli a sinistra del bordo originale
        if (oRight <= origLeft) {
            limit = std::max(limit, oRight); // non puoi entrare: left >= right dell'ostacolo
        }
    }
    return limit;
}

int LimitTopEdgeCells(int idx, const Tile& orig, int desiredTop) {
    int limit = std::max(desiredTop, 0);
    const int origTop = orig.y;

    for (int j = 0; j < (int)g_state.tiles.size(); ++j) {
        if (j == idx) continue;
        const Tile& o = g_state.tiles[j];

        if (!Overlap1D(orig.x, orig.x + orig.w, o.x, o.x + o.w)) continue;

        const int oBottom = o.y + o.h;
        if (oBottom <= origTop) {
            limit = std::max(limit, oBottom);
        }
    }
    return limit;
}

void SyncTileTextsFromWindows() {
    for (auto& t : g_state.tiles) {
        if (!t.edit) continue;

        int len = GetWindowTextLengthW(t.edit);
        std::wstring text(len + 1, L'\0');
        GetWindowTextW(t.edit, text.data(), len + 1);
        text.resize(len);
        t.text = text;
    }
}

POINT GetBoardCellBounds() {
    RECT rc{};
    if (!g_board || !GetClientRect(g_board, &rc)) return POINT{1, 1};

    const int boardW = std::max(1, static_cast<int>(rc.right - rc.left));
    const int boardH = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const int cell = std::max(16, g_state.cellSize);
    const int cellsX = std::max(1, boardW / cell);
    const int cellsY = std::max(1, boardH / cell);
    return POINT{cellsX, cellsY};
}

std::wstring GetStatePath() {
    wchar_t appData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData);
    std::wstring folder = std::wstring(appData) + L"\\GridNotes";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\state.json";
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'\\': out += L"\\\\"; break;
            case L'"': out += L"\\\""; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::wstring JsonUnescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
                case L'\\': out += L'\\'; break;
                case L'"': out += L'"'; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                default: out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

bool IsStartupEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

    wchar_t value[1024]{};
    DWORD type = REG_SZ;
    DWORD size = sizeof(value);
    const LONG result = RegGetValueW(key, nullptr, kAppName, RRF_RT_REG_SZ, &type, value, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
/*commenta questa funzione se vuoi usare startup.h e il task scheduler (costo di circa 90kb sull'exe)*/
void SetStartup(bool enabled) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;

    if (enabled) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath), static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

int ExtractJsonInt(const std::wstring& src, const std::wstring& key, int fallback) {
    const std::wstring token = L"\"" + key + L"\":";
    const size_t pos = src.find(token);
    if (pos == std::wstring::npos) return fallback;

    size_t i = pos + token.size();
    while (i < src.size() && iswspace(src[i])) ++i;
    size_t end = i;
    if (end < src.size() && (src[end] == L'-' || src[end] == L'+')) ++end;
    while (end < src.size() && iswdigit(src[end])) ++end;
    if (end <= i) return fallback;

    return _wtoi(src.substr(i, end - i).c_str());
}

bool ExtractJsonBool(const std::wstring& src, const std::wstring& key, bool fallback) {
    const std::wstring token = L"\"" + key + L"\":";
    const size_t pos = src.find(token);
    if (pos == std::wstring::npos) return fallback;

    size_t i = pos + token.size();
    while (i < src.size() && iswspace(src[i])) ++i;
    if (src.compare(i, 4, L"true") == 0) return true;
    if (src.compare(i, 5, L"false") == 0) return false;
    return fallback;
}

std::vector<Tile> ExtractTiles(const std::wstring& src) {
    std::vector<Tile> tiles;
    const std::wstring key = L"\"tiles\":";
    const size_t keyPos = src.find(key);
    if (keyPos == std::wstring::npos) return tiles;

    const size_t arrayStart = src.find(L'[', keyPos + key.size());
    const size_t arrayEnd = src.find(L']', arrayStart + 1);
    if (arrayStart == std::wstring::npos || arrayEnd == std::wstring::npos) return tiles;

    size_t objPos = arrayStart;
    while (true) {
        const size_t open = src.find(L'{', objPos);
        if (open == std::wstring::npos || open > arrayEnd) break;
        const size_t close = src.find(L'}', open + 1);
        if (close == std::wstring::npos || close > arrayEnd) break;

        std::wstring obj = src.substr(open, close - open + 1);
        Tile t;
        t.x = ExtractJsonInt(obj, L"x", 0);
        t.y = ExtractJsonInt(obj, L"y", 0);
        t.w = std::max(1, ExtractJsonInt(obj, L"w", 1));
        t.h = std::max(1, ExtractJsonInt(obj, L"h", 1));
            //printf("ken");

        std::wstring token = L"\"text\":";
size_t pos = obj.find(token);

if (pos != std::wstring::npos) {
    size_t i = pos + token.size();

    // salta eventuali spazi
    while (i < obj.size() && obj[i] == L' ')
        ++i;

    // deve iniziare con "
    if (i < obj.size() && obj[i] == L'"') {
        ++i;

        std::wstring raw;
        while (i < obj.size() && obj[i] != L'"') {
            raw += obj[i++];
        }
        t.text = JsonUnescape(raw); //fondamentale per evitare doppi \
        t.text = raw;  // testo estratto
    }
}

        tiles.push_back(t);
        objPos = close + 1;
    }

    return tiles;
}

void SaveState() {
    SyncTileTextsFromWindows();

    const std::wstring statePath = GetStatePath();
    std::wofstream out(statePath.c_str());
    if (!out) return;

    out << L"{\n";
    out << L"  \"cellSize\": " << g_state.cellSize << L",\n";
    out << L"  \"startWithWindows\": " << (g_state.startWithWindows ? L"true" : L"false") << L",\n";
    out << L"  \"windowWidth\": " << g_state.windowWidth << L",\n";
    out << L"  \"windowHeight\": " << g_state.windowHeight << L",\n";
    out << L"  \"tiles\": [\n";
    for (size_t i = 0; i < g_state.tiles.size(); ++i) {
        const Tile& t = g_state.tiles[i];
        out << L"    {\"x\": " << t.x << L", \"y\": " << t.y << L", \"w\": " << t.w << L", \"h\": " << t.h
            << L", \"text\": \"" << JsonEscape(t.text) << L"\"}";
        if (i + 1 < g_state.tiles.size()) out << L",";
        out << L"\n";
    }
    out << L"  ]\n";
    out << L"}\n";
}

void LoadState() {
    g_state = AppState{};

    const std::wstring statePath = GetStatePath();
    std::wifstream in(statePath.c_str());
    if (!in) return;

    std::wstringstream buffer;
    buffer << in.rdbuf();
    std::wstring json = buffer.str();

    g_state.cellSize = std::max(16, ExtractJsonInt(json, L"cellSize", g_state.cellSize));
    g_state.startWithWindows = ExtractJsonBool(json, L"startWithWindows", false);
    g_state.windowWidth = std::max(600, ExtractJsonInt(json, L"windowWidth", g_state.windowWidth));
    g_state.windowHeight = std::max(400, ExtractJsonInt(json, L"windowHeight", g_state.windowHeight));
    g_state.tiles = ExtractTiles(json);
}

void CreateDefault2x2(RECT rcBoard) {
    const int boardW = static_cast<int>(rcBoard.right - rcBoard.left);
    const int boardH = static_cast<int>(rcBoard.bottom - rcBoard.top);
    const int cell = std::max(16, g_state.cellSize);
    const int cellsX = std::max(2, boardW / cell);
    const int cellsY = std::max(2, boardH / cell);
    const int halfX = cellsX / 2;
    const int halfY = cellsY / 2;

    g_state.tiles.clear();
    g_state.tiles.push_back(Tile{0, 0, halfX, halfY, L"", nullptr});
    g_state.tiles.push_back(Tile{halfX, 0, cellsX - halfX, halfY, L"", nullptr});
    g_state.tiles.push_back(Tile{0, halfY, halfX, cellsY - halfY, L"", nullptr});
    g_state.tiles.push_back(Tile{halfX, halfY, cellsX - halfX, cellsY - halfY, L"", nullptr});
}

int Cell() { return std::max(16, g_state.cellSize); }
int ToPx(int c) { return c * Cell(); }
int SnapToCells(int px) { return static_cast<int>(std::round(static_cast<double>(px) / Cell())); }

int FindTileIndexByEdit(HWND editHwnd) {
    for (int i = 0; i < static_cast<int>(g_state.tiles.size()); ++i) {
        if (g_state.tiles[i].edit == editHwnd) return i;
    }
    return -1;
}

void ShowTileContextMenu(HWND owner, int idx, POINT screenPt) {
    if (idx < 0 || idx >= static_cast<int>(g_state.tiles.size())) return;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Split in 2 ─");
    AppendMenuW(menu, MF_STRING, 2, L"Split in 2 |");
    AppendMenuW(menu, MF_STRING, 3, L"Split in 4 +");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Elimina tile");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, owner, nullptr);
    DestroyMenu(menu);

    if (cmd == 1) Split2(idx, false);
    if (cmd == 2) Split2(idx, true);
    if (cmd == 3) Split4(idx);
    if (cmd == 4 && g_state.tiles.size() > 1) {
        if (g_state.tiles[idx].edit) DestroyWindow(g_state.tiles[idx].edit);
        g_state.tiles.erase(g_state.tiles.begin() + idx);
        LayoutTiles();
        SaveState();
    }
}
/*
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CONTEXTMENU) {
        int idx = FindTileIndexByEdit(hwnd);
        POINT screenPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (screenPt.x == -1 && screenPt.y == -1) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            screenPt.x = rc.left + 12;
            screenPt.y = rc.top + 12;
        }
        ShowTileContextMenu(g_board ? g_board : hwnd, idx, screenPt);
        return 0;
    }

    return CallWindowProcW(g_defaultEditProc, hwnd, msg, wParam, lParam);
}*/
static bool IsWordChar(wchar_t c) {
    return std::iswalnum((wint_t)c) || c == L'_';
}
static void DoCtrlBackspace(HWND hEdit)
{
    DWORD sel = (DWORD)SendMessageW(hEdit, EM_GETSEL, 0, 0);
    int start = LOWORD(sel);
    int end   = HIWORD(sel);

    // Se c'è selezione, Backspace cancella la selezione
    if (start != end) {
        SendMessageW(hEdit, EM_REPLACESEL, TRUE, (LPARAM)L"");
        return;
    }

    if (start <= 0) return;

    const int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return;

   // std::wstring text;
    //text.resize(len);
    //old?
    std::wstring text(len, L'\0');
    GetWindowTextW(hEdit, text.data(), len + 1);

    int i = start;

    // 1) Salta gli spazi a SINISTRA del cursore
    while (i > 0 && std::iswspace((wint_t)text[i - 1])) i--;

    // 2) Cancella la "parola" (alfanumerico/underscore) oppure blocco di simboli
    if (i > 0) {
        if (IsWordChar(text[i - 1])) {
            while (i > 0 && IsWordChar(text[i - 1])) i--;
        } else {
            // punteggiatura/simboli: elimina fino allo spazio precedente
            while (i > 0 && !std::iswspace((wint_t)text[i - 1])) i--;
        }
    }

    // 3) (facoltativo ma tipico) cancella anche gli spazi subito PRIMA della parola
    // (questa riga rende l'effetto più "aggressivo"; se non lo vuoi, commentala)
    while (i > 0 && std::iswspace((wint_t)text[i - 1])) i--;

    // Cancella [i, start)
        SendMessageW(hEdit, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hEdit, EM_SETSEL, i, start);
    SendMessageW(hEdit, EM_REPLACESEL, TRUE, (LPARAM)L"");
        SendMessageW(hEdit, WM_SETREDRAW, TRUE, 0);
        //InvalidateRect(hEdit, nullptr, TRUE); //riga suggerita ma disattivata perchè non ritengo attualmente necessaria

}
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CHAR:{
 // Sopprimi il carattere generato da Ctrl+Backspace / Ctrl+Delete
    if (GetKeyState(VK_CONTROL) & 0x8000)
    {
        // 0x7F = DEL (il tuo ""), 0x08 = BS (backspace)
        if (wParam == 0x7F || wParam == 0x08)
            return 0;
    }
    break;
        }
   
    case WM_KEYDOWN:
        if (wParam == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            DoCtrlBackspace(hwnd);
            return 0; // consumato
        }
        break;

    case WM_CONTEXTMENU:
    {
        int idx = FindTileIndexByEdit(hwnd);

        POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // Distinzione corretta: menu da tastiera
        if (lParam == (LPARAM)-1)
        {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            screenPt.x = rc.left + 12;
            screenPt.y = rc.top + 12;
        }

        ShowTileContextMenu(g_board ? g_board : hwnd, idx, screenPt);
        return 0; // consumato
    }

    }
    

    // default: lascia gestire alla vecchia proc
    return CallWindowProcW(g_defaultEditProc, hwnd, msg, wParam, lParam);
}

bool TextFitsInEdit(HWND edit, const std::wstring& text) {
    RECT client{};
    GetClientRect(edit, &client);
    const int rawW = static_cast<int>(client.right - client.left);
    const int rawH = static_cast<int>(client.bottom - client.top);
    const int clientW = std::max(1, rawW);
    const int clientH = std::max(1, rawH);

    HDC hdc = GetDC(edit);
    if (!hdc) return true;

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;

    RECT calc{0, 0, clientW, 0};
    if (text.empty()) {
        calc.bottom = 1;
    } else {
        DrawTextW(hdc, text.c_str(), -1, &calc, DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
    }

    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(edit, hdc);

    return calc.bottom <= clientH;
}

void DestroyTileWindows() {
    for (auto& t : g_state.tiles) {
        if (t.edit) DestroyWindow(t.edit);
        t.edit = nullptr;
    }
}

/*void LayoutTiles() {
    for (auto& t : g_state.tiles) {
        if (!t.edit) {
            t.edit = CreateWindowExW(
                0,
                L"EDIT",
                t.text.c_str(),
                WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_WANTRETURN,
                0,
                0,
                10,
                10,
                g_board,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            if (!g_defaultEditProc) {
                g_defaultEditProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(t.edit, GWLP_WNDPROC));
            }
           SetWindowLongPtrW(t.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc));

LOGFONT lf;
GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
lf.lfHeight *= 1.6; // font doppio

HFONT bigFont = CreateFontIndirect(&lf);

SendMessageW(t.edit, WM_SETFONT, (WPARAM)bigFont, TRUE);
        }

        const int inset = kEditPadding + (g_state.editLayout ? kResizeHandlePx : 0);
        MoveWindow(
            t.edit,
            ToPx(t.x) + inset,
            ToPx(t.y) + inset,
            std::max(24, ToPx(t.w) - 2 * inset),
            std::max(24, ToPx(t.h) - 2 * inset),
            TRUE);
}

    InvalidateRect(g_board, nullptr, TRUE);
}*/
//old, without batching ^^^^


void LayoutTiles() {
    // 1) Crea le edit mancanti (come fai ora)
    for (auto& t : g_state.tiles) {
        if (!t.edit) {
            t.edit = CreateWindowExW(
                0, L"EDIT", t.text.c_str(),
                WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_WANTRETURN,
                0, 0, 10, 10,
                g_board, nullptr, GetModuleHandleW(nullptr), nullptr);

            if (!g_defaultEditProc) {
                g_defaultEditProc = reinterpret_cast<WNDPROC>(
                    GetWindowLongPtrW(t.edit, GWLP_WNDPROC));
            }
            SetWindowLongPtrW(t.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc));

            SendMessageW(t.edit, WM_SETFONT, (WPARAM)g_bigFont, TRUE);
        }
    }

    // 2) Batch delle posizioni
    HDWP hdwp = BeginDeferWindowPos((int)g_state.tiles.size());
    if (!hdwp) return; // fallimento raro, ma possibile

    for (auto& t : g_state.tiles) {
        const int inset = kEditPadding + (g_state.editLayout ? kResizeHandlePx : 0);

        int x = ToPx(t.x) + inset;
        int y = ToPx(t.y) + inset;
        int w = std::max(24, ToPx(t.w) - 2 * inset);
        int h = std::max(24, ToPx(t.h) - 2 * inset);

        hdwp = DeferWindowPos(
            hdwp,
            t.edit,
            nullptr,
            x, y, w, h,
            SWP_NOZORDER | SWP_NOACTIVATE
        );

        if (!hdwp) return; // se DeferWindowPos fallisce, hdwp diventa NULL
    }

    EndDeferWindowPos(hdwp);

    // 3) Invalida senza erase (importante per flicker)
    InvalidateRect(g_board, nullptr, FALSE);
}

int HitTestTile(POINT ptBoard, DragEdge* edge = nullptr) {
    constexpr int margin = kResizeHandlePx;
    for (int i = static_cast<int>(g_state.tiles.size()) - 1; i >= 0; --i) {
        RECT r{ToPx(g_state.tiles[i].x), ToPx(g_state.tiles[i].y), ToPx(g_state.tiles[i].x + g_state.tiles[i].w), ToPx(g_state.tiles[i].y + g_state.tiles[i].h)};
        if (!PtInRect(&r, ptBoard)) continue;

        if (edge) {
            *edge = DragEdge::None;
            if (ptBoard.x - r.left <= margin) *edge = DragEdge::Left;
            else if (r.right - ptBoard.x <= margin) *edge = DragEdge::Right;
            else if (ptBoard.y - r.top <= margin) *edge = DragEdge::Top;
            else if (r.bottom - ptBoard.y <= margin) *edge = DragEdge::Bottom;
        }

        return i;
    }
    return -1;
}

void Split2(int idx, bool vertical) {
    if (idx < 0 || idx >= static_cast<int>(g_state.tiles.size())) return;

    Tile t = g_state.tiles[idx];
    HWND removedEdit = g_state.tiles[idx].edit;
    if (vertical && t.w < 2) return;
    if (!vertical && t.h < 2) return;

    if (removedEdit) DestroyWindow(removedEdit);
    g_state.tiles.erase(g_state.tiles.begin() + idx);

    if (vertical) {
        int w1 = t.w / 2;
        g_state.tiles.push_back(Tile{t.x, t.y, w1, t.h, t.text, nullptr});
        g_state.tiles.push_back(Tile{t.x + w1, t.y, t.w - w1, t.h, L"", nullptr});
    } else {
        int h1 = t.h / 2;
        g_state.tiles.push_back(Tile{t.x, t.y, t.w, h1, t.text, nullptr});
        g_state.tiles.push_back(Tile{t.x, t.y + h1, t.w, t.h - h1, L"", nullptr});
    }

    DestroyTileWindows();
    LayoutTiles();
    SaveState();
}

void Split4(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_state.tiles.size())) return;

    Tile t = g_state.tiles[idx];
    HWND removedEdit = g_state.tiles[idx].edit;
    if (t.w < 2 || t.h < 2) return;

    int w1 = t.w / 2;
    int h1 = t.h / 2;

    if (removedEdit) DestroyWindow(removedEdit);
    g_state.tiles.erase(g_state.tiles.begin() + idx);
    g_state.tiles.push_back(Tile{t.x, t.y, w1, h1, t.text, nullptr});
    g_state.tiles.push_back(Tile{t.x + w1, t.y, t.w - w1, h1, L"", nullptr});
    g_state.tiles.push_back(Tile{t.x, t.y + h1, w1, t.h - h1, L"", nullptr});
    g_state.tiles.push_back(Tile{t.x + w1, t.y + h1, t.w - w1, t.h - h1, L"", nullptr});

    DestroyTileWindows();
    LayoutTiles();
    SaveState();
}

LRESULT CALLBACK BoardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (!g_state.editLayout) break;

            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            DragEdge edge;
            int idx = HitTestTile(pt, &edge);

            if (idx >= 0 && edge != DragEdge::None) {
                g_dragging = true;
                g_dragTile = idx;
                g_dragEdge = edge;
                g_dragStart = pt;
                g_originalTile = g_state.tiles[idx];
                SetCapture(hwnd);
            }
            break;
        }
        case WM_MOUSEMOVE: {
    if (!g_dragging || g_dragTile < 0) break;

    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    int dx = SnapToCells(pt.x - g_dragStart.x);
    int dy = SnapToCells(pt.y - g_dragStart.y);
    const POINT bounds = GetBoardCellBounds(); // bounds.x / bounds.y in CELLE

    Tile t = g_originalTile;

    const int origLeft   = g_originalTile.x;
    const int origTop    = g_originalTile.y;
    const int origRight  = g_originalTile.x + g_originalTile.w;
    const int origBottom = g_originalTile.y + g_originalTile.h;

    if (g_dragEdge == DragEdge::Right) {
        int desiredRight = std::clamp(origRight + dx, origLeft + 1, (int)bounds.x);
        desiredRight = LimitRightEdgeCells(g_dragTile, g_originalTile, desiredRight, bounds.x);
        t.w = desiredRight - origLeft;
    }

    if (g_dragEdge == DragEdge::Bottom) {
        int desiredBottom = std::clamp(origBottom + dy, origTop + 1, (int)bounds.y);
        desiredBottom = LimitBottomEdgeCells(g_dragTile, g_originalTile, desiredBottom, bounds.y);
        t.h = desiredBottom - origTop;
    }

    if (g_dragEdge == DragEdge::Left) {
        int desiredLeft = std::clamp(origLeft + dx, 0, origRight - 1);
        desiredLeft = LimitLeftEdgeCells(g_dragTile, g_originalTile, desiredLeft);
        t.x = desiredLeft;
        t.w = origRight - t.x;
    }

    if (g_dragEdge == DragEdge::Top) {
        int desiredTop = std::clamp(origTop + dy, 0, origBottom - 1);
        desiredTop = LimitTopEdgeCells(g_dragTile, g_originalTile, desiredTop);
        t.y = desiredTop;
        t.h = origBottom - t.y;
    }

    g_state.tiles[g_dragTile] = t;
    //g_state.tiles[g_dragTile].x = t.x;
//g_state.tiles[g_dragTile].y = t.y;
//g_state.tiles[g_dragTile].w = t.w;
//g_state.tiles[g_dragTile].h = t.h;
//old?
    LayoutTiles();
    break;
}
        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = false;
                g_dragTile = -1;
                g_dragEdge = DragEdge::None;
                ReleaseCapture();
                SaveState();
            }
            break;
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT local = pt;
            ScreenToClient(hwnd, &local);
            int idx = HitTestTile(local);
            if (idx < 0) break;
            ShowTileContextMenu(hwnd, idx, pt);
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, RGB(TILE_COLOR, TILE_COLOR, TILE_COLOR));
            SetTextColor(hdc, RGB(235, 235, 235));
            return reinterpret_cast<LRESULT>(g_editBgBrush);
        }
        /*case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);

            HBRUSH bg = CreateSolidBrush(RGB(BACKGROUND, BACKGROUND, BACKGROUND));

            FillRect(hdc, &ps.rcPaint, bg);
            DeleteObject(bg);

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
            HGDIOBJ oldBrush = SelectObject(hdc, hollow);

            for (const auto& t : g_state.tiles) {
                Rectangle(hdc, ToPx(t.x), ToPx(t.y), ToPx(t.x + t.w), ToPx(t.y + t.h));
            }

                       // Draw resize handles in Edit Layout mode (always visible)\n            if (g_state.editLayout) {\n                const int tpx = kResizeHandlePx;\n                HBRUSH handleBrush = CreateSolidBrush(RGB(140, 140, 140));\n\n                for (const auto& tile : g_state.tiles) {\n                    RECT r{ToPx(tile.x), ToPx(tile.y), ToPx(tile.x + tile.w), ToPx(tile.y + tile.h)};\n\n                    RECT left{r.left, r.top, r.left + tpx, r.bottom};\n                    RECT right{r.right - tpx, r.top, r.right, r.bottom};\n                    RECT top{r.left, r.top, r.right, r.top + tpx};\n                    RECT bottom{r.left, r.bottom - tpx, r.right, r.bottom};\n\n                    FillRect(hdc, &left, handleBrush);\n                    FillRect(hdc, &right, handleBrush);\n                    FillRect(hdc, &top, handleBrush);\n                    FillRect(hdc, &bottom, handleBrush);\n                }\n\n                DeleteObject(handleBrush);\n            }\nSelectObject(hdc, oldBrush);

                       SelectObject(hdc, oldPen);
            DeleteObject(pen);
            EndPaint(hwnd, &ps);
            return 0;
        }*/
       //old, la versione sopra causava flickering dei bordi
       case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // --- disegna tutto su "mem" invece che su hdc ---
    HBRUSH bg = CreateSolidBrush(RGB(BACKGROUND, BACKGROUND, BACKGROUND));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    HGDIOBJ oldBrush = SelectObject(mem, hollow);

    for (const auto& t : g_state.tiles) {
        Rectangle(mem, ToPx(t.x), ToPx(t.y), ToPx(t.x + t.w), ToPx(t.y + t.h));
    }

    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    // copia su schermo in un colpo solo
    BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(hwnd, &ps);
    return 0;
}
        case WM_COMMAND:
            return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
        case WM_ERASEBKGND:
            return 1;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DRAWITEM:
{
    auto* dis = (DRAWITEMSTRUCT*)lParam;
    HWND hCheck = dis->hwndItem;
    if (hCheck!= g_editToggle&&hCheck!= g_startupToggle) break;

    // scegli brush in base allo stato
    HBRUSH bg = g_state.editLayout
        ? CreateSolidBrush(RGB(0, 0, 0))   // ON
        : CreateSolidBrush(RGB(60, 60, 60)); // OFF

    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);

    // bordo semplice
    FrameRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(BLACK_BRUSH));

    // testo
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, RGB(235, 235, 235));
     const wchar_t* txt;
if(hCheck== g_editToggle){
    //printf("edit");
    txt = g_state.editLayout ? L"Edit layout: ON" : L"Edit layout: OFF";

}
else if(hCheck== g_startupToggle){
     txt = g_state.startWithWindows ? L"Start with Windows: ON" : L"Start with Windows: OFF";

}
    RECT r = dis->rcItem;
    DrawTextW(dis->hDC, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // focus (opzionale)
    if (dis->itemState & ODS_FOCUS) {
        RECT fr = dis->rcItem;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(dis->hDC, &fr);
    }

    return TRUE;
}
        case WM_CREATE: {
            g_editBgBrush = CreateSolidBrush(RGB(TILE_COLOR, TILE_COLOR, TILE_COLOR));
            g_toolbarBgBrush = CreateSolidBrush(RGB(TILE_COLOR, TILE_COLOR, TILE_COLOR));
            
            /*g_editToggle = CreateWindowW(
                L"BUTTON",
                L"Edit layout",
                WS_CHILD | WS_VISIBLE | BS_PUSHLIKE | BS_AUTOCHECKBOX,
                12,
                8,
                120,
                22,
                hwnd,
                reinterpret_cast<HMENU>(kCmdEditLayout),
                GetModuleHandleW(nullptr),
                nullptr);*/

                g_editToggle = CreateWindowW(
                    L"BUTTON",
                    L"", // lo disegni tu
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    12, 8, 140, 22,
                    hwnd,
                    (HMENU)kCmdEditLayout,
                    GetModuleHandleW(nullptr),
                    nullptr);
            // Make the "Edit layout" button behave like a toggle (pressed when active)
           // SendMessageW(g_editToggle, BM_SETCHECK, g_state.editLayout ? BST_CHECKED : BST_UNCHECKED, 0);
            //SetWindowTextW(g_editToggle, g_state.editLayout ? L"Edit layout: ON" : L"Edit layout: OFF");

            /*g_startupToggle = CreateWindowW(
                L"BUTTON",
                L"Start with Windows",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                160,
                8,
                170,
                22,
                hwnd,
                reinterpret_cast<HMENU>(kCmdStartup),
                GetModuleHandleW(nullptr),
                nullptr);*/
 //errore di copia di chatgpt lol           SetWindowTextW(g_editToggle, g_state.editLayout ? L"Edit layout ON" : L"Edit layout OFF");
           // SendMessageW(g_startupToggle, BM_SETCHECK, g_state.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
  g_startupToggle = CreateWindowW(
                    L"BUTTON",
                    L"", // lo disegni tu
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    160,
                8,
            240,
                22,
                    hwnd,
                    (HMENU)kCmdStartup,
                    GetModuleHandleW(nullptr),
                    nullptr);
            WNDCLASSW wc{};
            wc.lpfnWndProc = BoardProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"GridNotesBoard";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);

            g_board = CreateWindowW(L"GridNotesBoard", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, kToolbarHeight, 100, 100, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            MoveWindow(g_board, 0, kToolbarHeight, w, std::max(1, h - kToolbarHeight), TRUE);
            LayoutTiles();
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == kCmdEditLayout /* && HIWORD(wParam) == BN_CLICKED */) {
    g_state.editLayout = !g_state.editLayout;

    LayoutTiles();
    SaveState();

    InvalidateRect(g_editToggle, nullptr, TRUE); // ridisegna il bottone owner-draw
    InvalidateRect(g_board, nullptr, TRUE);
    UpdateWindow(g_board);
    return 0;
}
            /* if (LOWORD(wParam) == kCmdEditLayout && HIWORD(wParam) == BN_CLICKED) {
                // Button is BS_AUTOCHECKBOX|BS_PUSHLIKE: Windows toggles the check state for us.

                const LRESULT checked = SendMessageW(g_editToggle, BM_GETCHECK, 0, 0);
                g_state.editLayout = (checked == BST_CHECKED);
                SetWindowTextW(g_editToggle, g_state.editLayout ? L"Edit layout: ON" : L"Edit layout: OFF");

                LayoutTiles();
                SaveState();
                InvalidateRect(g_board, nullptr, TRUE);
                UpdateWindow(g_board);
                return 0;
            }*/
           if (LOWORD(wParam) == kCmdStartup /*&& HIWORD(wParam) == BN_CLICKED */) {
    g_state.startWithWindows = !g_state.startWithWindows;

    // (opzionale ma consigliato) allinea lo stato "checked" del controllo //non ho capito a che serve
    //SendMessageW(g_startupToggle, BM_SETCHECK,g_state.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);

    SetStartup(g_state.startWithWindows);
    SaveState();

    InvalidateRect(g_startupToggle, nullptr, TRUE); // ridisegna owner-draw
    return 0;
}
        /*    if (LOWORD(wParam) == kCmdStartup && HIWORD(wParam) == BN_CLICKED) {
                g_state.startWithWindows = (SendMessageW(g_startupToggle, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetStartup(g_state.startWithWindows);
                SaveState();
                return 0;
            }
*/
            if (HIWORD(wParam) == EN_CHANGE) {
                if (g_internalTextSet) return 0;

                HWND changed = reinterpret_cast<HWND>(lParam);
                for (auto& t : g_state.tiles) {
                    if (t.edit != changed) continue;

                    int len = GetWindowTextLengthW(t.edit);
                    std::wstring text(len + 1, L'\0');
                    GetWindowTextW(t.edit, text.data(), len + 1);
                    text.resize(len);

                    if (!TextFitsInEdit(t.edit, text)) {
                        g_internalTextSet = true;
                        SetWindowTextW(t.edit, t.text.c_str());
                        SendMessageW(t.edit, EM_SETSEL, static_cast<WPARAM>(t.text.size()), static_cast<LPARAM>(t.text.size()));
                        g_internalTextSet = false;
                        MessageBeep(MB_ICONWARNING);
                        break;
                    }

                    t.text = text;
                    // SaveState(); //esoso in termini di risorse:ogni lettera è un i/o

g_savePending = true;
KillTimer(hwnd, kTimerSaveDebounce);
SetTimer(hwnd, kTimerSaveDebounce, kSaveDebounceMs, nullptr);


                    break;
                }
            }
            return 0;
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(235, 235, 235));
            
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(g_toolbarBgBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_toolbarBgBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            g_state.windowWidth = rc.right - rc.left;
            g_state.windowHeight = rc.bottom - rc.top;
            SaveState();
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            if (g_editBgBrush) {
                DeleteObject(g_editBgBrush);
                g_editBgBrush = nullptr;
            }
            if (g_toolbarBgBrush) {
                DeleteObject(g_toolbarBgBrush);
                g_toolbarBgBrush = nullptr;
            }
            if(g_bigFont){
                  DeleteObject(g_toolbarBgBrush);
                g_bigFont = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        case WM_TIMER: {
    if (wParam == kTimerSaveDebounce) {
        KillTimer(hwnd, kTimerSaveDebounce);

        if (g_savePending) {
            g_savePending = false;
            SaveState();
        }
        return 0;
    }
    break;
}
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}





int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CreateGlobalFont();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    LoadState();

    // Registro vince sul valore salvato, così l'interruttore riflette lo stato reale.
    g_state.startWithWindows = IsStartupEnabled();

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"GridNotesMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

 
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"GridNotesMain",
        L"testFileGrid",
       // WS_OVERLAPPEDWINDOW,
       WS_POPUP, 
       CW_USEDEFAULT,
        CW_USEDEFAULT,
        g_state.windowWidth,
        g_state.windowHeight,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
    if (!hwnd) return 0;

CenterWindowOnSecondMonitor(hwnd,g_state.windowWidth,g_state.windowHeight);


    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    RECT boardRc{};
    GetClientRect(g_board, &boardRc);
    if (g_state.tiles.empty()) CreateDefault2x2(boardRc);
    LayoutTiles();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
