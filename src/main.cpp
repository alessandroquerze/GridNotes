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
    int windowWidth{900};
    int windowHeight{700};
    std::vector<Tile> tiles;
};

constexpr wchar_t kAppName[] = L"GridNotes";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kToolbarHeight = 36;
constexpr int kEditPadding = 4;
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
    const int cellsX = std::max(1, boardW / Cell());
    const int cellsY = std::max(1, boardH / Cell());
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

        const std::wstring textToken = L"\"text\":\"";
        const size_t textPos = obj.find(textToken);
        if (textPos != std::wstring::npos) {
            size_t i = textPos + textToken.size();
            std::wstring raw;
            while (i < obj.size()) {
                if (obj[i] == L'"' && !(i > 0 && obj[i - 1] == L'\\')) break;
                raw += obj[i++];
            }
            t.text = JsonUnescape(raw);
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

void LayoutTiles() {
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
            SendMessageW(t.edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }

        MoveWindow(
            t.edit,
            ToPx(t.x) + kEditPadding,
            ToPx(t.y) + kEditPadding,
            std::max(24, ToPx(t.w) - 2 * kEditPadding),
            std::max(24, ToPx(t.h) - 2 * kEditPadding),
            TRUE);
    }

    InvalidateRect(g_board, nullptr, TRUE);
}

int HitTestTile(POINT ptBoard, DragEdge* edge = nullptr) {
    constexpr int margin = 8;
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
            const POINT bounds = GetBoardCellBounds();

            Tile t = g_originalTile;
            if (g_dragEdge == DragEdge::Right) {
                const int newRight = std::clamp(g_originalTile.x + g_originalTile.w + dx, g_originalTile.x + 1, bounds.x);
                t.w = newRight - g_originalTile.x;
            }
            if (g_dragEdge == DragEdge::Bottom) {
                const int newBottom = std::clamp(g_originalTile.y + g_originalTile.h + dy, g_originalTile.y + 1, bounds.y);
                t.h = newBottom - g_originalTile.y;
            }
            if (g_dragEdge == DragEdge::Left) {
                const int right = g_originalTile.x + g_originalTile.w;
                t.x = std::clamp(g_originalTile.x + dx, 0, right - 1);
                t.w = right - t.x;
            }
            if (g_dragEdge == DragEdge::Top) {
                const int bottom = g_originalTile.y + g_originalTile.h;
                t.y = std::clamp(g_originalTile.y + dy, 0, bottom - 1);
                t.h = bottom - t.y;
            }

            g_state.tiles[g_dragTile].x = t.x;
            g_state.tiles[g_dragTile].y = t.y;
            g_state.tiles[g_dragTile].w = t.w;
            g_state.tiles[g_dragTile].h = t.h;
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
            SetBkColor(hdc, RGB(45, 45, 45));
            SetTextColor(hdc, RGB(235, 235, 235));
            return reinterpret_cast<LRESULT>(g_editBgBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);

            HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
            FillRect(hdc, &ps.rcPaint, bg);
            DeleteObject(bg);

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
            HGDIOBJ oldBrush = SelectObject(hdc, hollow);

            for (const auto& t : g_state.tiles) {
                Rectangle(hdc, ToPx(t.x), ToPx(t.y), ToPx(t.x + t.w), ToPx(t.y + t.h));
            }

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_editBgBrush = CreateSolidBrush(RGB(45, 45, 45));
            g_toolbarBgBrush = CreateSolidBrush(RGB(0, 0, 0));

            g_editToggle = CreateWindowW(
                L"BUTTON",
                L"Edit layout",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                12,
                8,
                120,
                22,
                hwnd,
                reinterpret_cast<HMENU>(kCmdEditLayout),
                GetModuleHandleW(nullptr),
                nullptr);

            g_startupToggle = CreateWindowW(
                L"BUTTON",
                L"Start with Windows",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                150,
                8,
                170,
                22,
                hwnd,
                reinterpret_cast<HMENU>(kCmdStartup),
                GetModuleHandleW(nullptr),
                nullptr);

            SendMessageW(g_editToggle, BM_SETCHECK, g_state.editLayout ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_startupToggle, BM_SETCHECK, g_state.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);

            WNDCLASSW wc{};
            wc.lpfnWndProc = BoardProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"GridNotesBoard";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);

            g_board = CreateWindowW(L"GridNotesBoard", nullptr, WS_CHILD | WS_VISIBLE, 0, kToolbarHeight, 100, 100, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
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
            if (LOWORD(wParam) == kCmdEditLayout && HIWORD(wParam) == BN_CLICKED) {
                g_state.editLayout = (SendMessageW(g_editToggle, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveState();
                return 0;
            }
            if (LOWORD(wParam) == kCmdStartup && HIWORD(wParam) == BN_CLICKED) {
                g_state.startWithWindows = (SendMessageW(g_startupToggle, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetStartup(g_state.startWithWindows);
                SaveState();
                return 0;
            }

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
                    SaveState();
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
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
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
        0,
        L"GridNotesMain",
        L"GridNotes (C++ / EDIT)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        g_state.windowWidth,
        g_state.windowHeight,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
    if (!hwnd) return 0;

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
