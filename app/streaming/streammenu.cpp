#include "streammenu.h"

#include <QtGlobal>

#ifdef Q_OS_WIN32

#include "session.h"
#include "settings/streamingpreferences.h"
#include "SDL_compat.h"
#include <SDL_syswm.h>

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSvgRenderer>

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {

enum Edge { EdgeLeft, EdgeTop, EdgeRight, EdgeBottom };

enum Command {
    CmdDisconnect = 1,
    CmdQuitAppAndExit,
    CmdFullScreen,
    CmdMinimize,
    CmdMetrics,
    CmdSound,
    CmdImmersive,
    CmdReleaseInput,
    CmdCursor,
    CmdLockCursor,
    CmdSystemKeys,
    CmdPaste,
    CmdCtrlAltDel,
    CmdHideButton,
    CmdScreens1 = 100,      // 100..102 = 1..3 screens
    CmdResolution = 200,    // 200 + index into the resolution list
};

constexpr UINT_PTR k_ParentSubclassId = 0x4D4C534D; // 'MLSM'
constexpr int k_ButtonSizeDip = 40;
const wchar_t* k_ButtonClass = L"MoonlightStreamMenuButton";

// Dark menus on Windows 10 1903+ (undocumented uxtheme ordinals, the same ones Explorer uses)
void enableDarkMenus()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme == nullptr) {
        return;
    }
    using SetPreferredAppModeFn = int (WINAPI*)(int);
    using FlushMenuThemesFn = void (WINAPI*)();
    auto setPreferredAppMode = (SetPreferredAppModeFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    auto flushMenuThemes = (FlushMenuThemesFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    if (setPreferredAppMode != nullptr) {
        setPreferredAppMode(2); // ForceDark
    }
    if (flushMenuThemes != nullptr) {
        flushMenuThemes();
    }
}

}

class StreamMenu
{
public:
    StreamMenu(Session* session, SDL_Window* window, HWND parent)
        : m_Session(session), m_Window(window), m_Parent(parent)
    {
        QSettings settings;
        m_Edge = qBound(0, settings.value("streammenu/edge", EdgeBottom).toInt(), 3);
        m_Pos = qBound(0.0, settings.value("streammenu/pos", 0.0).toDouble(), 1.0);
        m_Visible = StreamingPreferences::get()->showStreamMenuButton;

        enableDarkMenus();
        registerClass();

        m_Button = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   k_ButtonClass, L"Moonlight menu", WS_POPUP,
                                   0, 0, 1, 1, m_Parent, nullptr, GetModuleHandleW(nullptr), this);
        SetWindowSubclass(m_Parent, parentProc, k_ParentSubclassId, (DWORD_PTR)this);
        render();
        reposition();
    }

    ~StreamMenu()
    {
        RemoveWindowSubclass(m_Parent, parentProc, k_ParentSubclassId);
        if (m_Button != nullptr) {
            DestroyWindow(m_Button);
        }
    }

    void toggle()
    {
        if (!canShowButton()) {
            // Exclusive fullscreen: no floating window over it, so open the menu at the cursor
            POINT pt;
            GetCursorPos(&pt);
            showMenu(pt.x, pt.y, TPM_LEFTALIGN | TPM_TOPALIGN);
            return;
        }
        m_Visible = !m_Visible;
        StreamingPreferences::get()->showStreamMenuButton = m_Visible;
        StreamingPreferences::get()->save();
        reposition();
    }

private:
    // ---- geometry ----

    int buttonSize() const
    {
        // GetDpiForWindow is Windows 10 1607+; look it up so older SDK targets still build
        using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
        static auto getDpiForWindow = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        UINT dpi = 96;
        if (getDpiForWindow != nullptr) {
            dpi = getDpiForWindow(m_Parent);
        }
        else {
            HDC dc = GetDC(m_Parent);
            dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(m_Parent, dc);
        }
        return MulDiv(k_ButtonSizeDip, dpi, 96);
    }

    RECT clientRectOnScreen() const
    {
        RECT rc;
        GetClientRect(m_Parent, &rc);
        POINT origin = { 0, 0 };
        ClientToScreen(m_Parent, &origin);
        OffsetRect(&rc, origin.x, origin.y);
        return rc;
    }

    bool canShowButton() const
    {
        // A window over an exclusive-fullscreen swapchain would knock it out of fullscreen
        Uint32 flags = SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        return flags != SDL_WINDOW_FULLSCREEN && !IsIconic(m_Parent);
    }

    void reposition()
    {
        if (m_Button == nullptr) {
            return;
        }
        if (!m_Visible || !canShowButton()) {
            ShowWindow(m_Button, SW_HIDE);
            return;
        }

        int s = buttonSize(), m = s / 5;
        RECT rc = clientRectOnScreen();
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        int x = rc.left + m, y = rc.top + m;
        int alongX = rc.left + m + (int)(m_Pos * std::max(0, w - s - 2 * m));
        int alongY = rc.top + m + (int)(m_Pos * std::max(0, h - s - 2 * m));
        switch (m_Edge) {
        case EdgeLeft:   x = rc.left + m;       y = alongY; break;
        case EdgeRight:  x = rc.right - m - s;  y = alongY; break;
        case EdgeTop:    x = alongX;            y = rc.top + m; break;
        default:         x = alongX;            y = rc.bottom - m - s; break;
        }
        if (s != m_RenderedSize) {
            render();
        }
        SetWindowPos(m_Button, nullptr, x, y, s, s, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    // Dock to the nearest window edge, remembering where along it
    void snap()
    {
        RECT br, rc = clientRectOnScreen();
        GetWindowRect(m_Button, &br);
        int s = buttonSize(), m = s / 5;
        int cx = (br.left + br.right) / 2, cy = (br.top + br.bottom) / 2;
        int d[4] = { cx - rc.left, cy - rc.top, rc.right - cx, rc.bottom - cy };
        m_Edge = (int)(std::min_element(d, d + 4) - d);

        double span = (m_Edge == EdgeLeft || m_Edge == EdgeRight)
                ? std::max(1, (int)(rc.bottom - rc.top) - s - 2 * m)
                : std::max(1, (int)(rc.right - rc.left) - s - 2 * m);
        double along = (m_Edge == EdgeLeft || m_Edge == EdgeRight)
                ? br.top - (rc.top + m)
                : br.left - (rc.left + m);
        m_Pos = qBound(0.0, along / span, 1.0);

        QSettings settings;
        settings.setValue("streammenu/edge", m_Edge);
        settings.setValue("streammenu/pos", m_Pos);
        reposition();
    }

    // ---- drawing: dark rounded tile with the Moonlight logo, per-pixel alpha ----

    void render()
    {
        int s = buttonSize();
        m_RenderedSize = s;

        QImage image(s, s, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(0.5, 0.5, s - 1, s - 1), s * 0.22, s * 0.22);
            painter.fillPath(path, m_Hover ? QColor(52, 52, 58, 245) : QColor(26, 26, 30, 215));
            painter.setPen(QPen(QColor(255, 255, 255, m_Hover ? 70 : 35), 1));
            painter.drawPath(path);

            QSvgRenderer logo(QString(":/res/moonlight.svg"));
            double pad = s * 0.18;
            logo.render(&painter, QRectF(pad, pad, s - 2 * pad, s - 2 * pad));
        }

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = s;
        bmi.bmiHeader.biHeight = -s; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap != nullptr && bits != nullptr) {
            // Premultiplied ARGB32 is BGRA in memory: exactly what UpdateLayeredWindow wants
            for (int row = 0; row < s; row++) {
                memcpy((uint8_t*)bits + row * s * 4, image.constScanLine(row), s * 4);
            }
            HGDIOBJ old = SelectObject(mem, bitmap);
            SIZE size = { s, s };
            POINT src = { 0, 0 };
            BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(m_Button, screen, nullptr, &size, mem, &src, 0, &blend, ULW_ALPHA);
            SelectObject(mem, old);
            DeleteObject(bitmap);
        }
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }

    // ---- the menu ----

    struct Resolution { int w, h; QString label; };

    std::vector<Resolution> resolutions()
    {
        std::vector<Resolution> list;
        SDL_DisplayMode mode;
        int display = SDL_GetWindowDisplayIndex(m_Window);
        if (display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0) {
            list.push_back({ mode.w, mode.h, QString("This monitor (%1 x %2)").arg(mode.w).arg(mode.h) });
        }
        const Resolution common[] = {
            { 1280, 720, "1280 x 720" }, { 1920, 1080, "1920 x 1080" },
            { 2560, 1440, "2560 x 1440" }, { 3840, 2160, "3840 x 2160" },
        };
        for (const auto& r : common) {
            if (list.empty() || r.w != list[0].w || r.h != list[0].h) {
                list.push_back(r);
            }
        }
        return list;
    }

    static void add(HMENU menu, UINT id, const QString& text, bool checked = false, bool radio = false)
    {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
        mii.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
        mii.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
        mii.wID = id;
        std::wstring label = text.toStdWString();
        mii.dwTypeData = label.data();
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);
    }

    static void separator(HMENU menu)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    static void submenu(HMENU menu, HMENU sub, const QString& text)
    {
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, text.toStdWString().c_str());
    }

    void showMenu(int x, int y, UINT align)
    {
        // With the mouse captured (immersive), release it first so the menu is usable
        if (m_Session->isImmersive()) {
            m_Session->runShortcutCommand('Z');
        }

        const QString key = QStringLiteral("\tCtrl+Alt+Shift+");
        HMENU menu = CreatePopupMenu();

        add(menu, CmdFullScreen, (m_Session->isFullScreen() ? "Windowed" : "Fullscreen") + key + "X");
        add(menu, CmdMinimize, "Minimize" + key + "D");
        add(menu, CmdMetrics, "Metrics" + key + "S", m_Session->isStatsOverlayVisible());
        add(menu, CmdSound, "Sound", !m_Session->isAudioMuted());
        separator(menu);

        HMENU screens = CreatePopupMenu();
        int screenCount = m_Session->extraScreenCount() + 1;
        for (int n = 1; n <= 3; n++) {
            add(screens, CmdScreens1 + n - 1, n == 1 ? QString("1 screen") : QString("%1 screens").arg(n), n == screenCount, true);
        }
        submenu(menu, screens, "Screens");

        HMENU resolutionMenu = CreatePopupMenu();
        auto list = resolutions();
        for (size_t i = 0; i < list.size(); i++) {
            bool current = list[i].w == m_Session->streamWidth() && list[i].h == m_Session->streamHeight();
            add(resolutionMenu, CmdResolution + (UINT)i, list[i].label, current, true);
        }
        separator(resolutionMenu);
        AppendMenuW(resolutionMenu, MF_STRING | MF_GRAYED, 0, L"Changing it reconnects (a few seconds)");
        submenu(menu, resolutionMenu, "Resolution");
        separator(menu);

        add(menu, CmdImmersive, "Immersive mode (capture mouse)" + key + "M", m_Session->isImmersive());
        add(menu, CmdReleaseInput, "Release mouse and keyboard" + key + "Z");
        add(menu, CmdCursor, "Show/hide local cursor" + key + "C");
        add(menu, CmdLockCursor, "Lock cursor to window" + key + "L");
        add(menu, CmdSystemKeys, "Capture system keys" + key + "K");
        add(menu, CmdPaste, "Paste clipboard as text" + key + "V");
        add(menu, CmdCtrlAltDel, "Send Ctrl+Alt+Del");
        separator(menu);

        add(menu, CmdHideButton, (canShowButton() ? "Hide this button" : "Show the menu button") + key + "B");
        add(menu, CmdDisconnect, "Disconnect" + key + "Q");
        add(menu, CmdQuitAppAndExit, "Quit app and exit Moonlight" + key + "E");

        // The stream window owns the menu, so it keeps keyboard focus
        SetForegroundWindow(m_Parent);
        UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | align,
                                          x, y, m_Parent, nullptr);
        DestroyMenu(menu); // also destroys the submenus
        run(cmd, list);
    }

    void run(UINT cmd, const std::vector<Resolution>& list)
    {
        switch (cmd) {
        case 0: break; // dismissed
        case CmdDisconnect:     m_Session->runShortcutCommand('Q'); break;
        case CmdQuitAppAndExit: m_Session->runShortcutCommand('E'); break;
        case CmdFullScreen:     m_Session->runShortcutCommand('X'); break;
        case CmdMinimize:       m_Session->runShortcutCommand('D'); break;
        case CmdMetrics:        m_Session->runShortcutCommand('S'); break;
        case CmdSound:          m_Session->setAudioMuted(!m_Session->isAudioMuted()); break;
        case CmdImmersive:      m_Session->toggleImmersive(); break;
        case CmdReleaseInput:   m_Session->runShortcutCommand('Z'); break;
        case CmdCursor:         m_Session->runShortcutCommand('C'); break;
        case CmdLockCursor:     m_Session->runShortcutCommand('L'); break;
        case CmdSystemKeys:     m_Session->runShortcutCommand('K'); break;
        case CmdPaste:          m_Session->runShortcutCommand('V'); break;
        case CmdCtrlAltDel:     m_Session->sendCtrlAltDel(); break;
        case CmdHideButton:     toggle(); break;
        default:
            if (cmd >= CmdScreens1 && cmd < CmdScreens1 + 3) {
                m_Session->setExtraScreenCount((int)(cmd - CmdScreens1));
            }
            else if (cmd >= CmdResolution && cmd < CmdResolution + list.size()) {
                const auto& r = list[cmd - CmdResolution];
                if (r.w != m_Session->streamWidth() || r.h != m_Session->streamHeight()) {
                    m_Session->reconnectWithResolution(r.w, r.h);
                }
            }
            break;
        }
    }

    void openMenuFromButton()
    {
        RECT br;
        GetWindowRect(m_Button, &br);
        switch (m_Edge) {
        case EdgeLeft:  showMenu(br.right + 4, br.top, TPM_LEFTALIGN | TPM_TOPALIGN); break;
        case EdgeRight: showMenu(br.left - 4, br.top, TPM_RIGHTALIGN | TPM_TOPALIGN); break;
        case EdgeTop:   showMenu(br.left, br.bottom + 4, TPM_LEFTALIGN | TPM_TOPALIGN); break;
        default:        showMenu(br.left, br.top - 4, TPM_LEFTALIGN | TPM_BOTTOMALIGN); break;
        }
    }

    // ---- window procedures ----

    static void registerClass()
    {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = buttonProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = k_ButtonClass;
        registered = RegisterClassExW(&wc) != 0;
    }

    static LRESULT CALLBACK buttonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
        }
        auto self = (StreamMenu*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // never take focus from the stream

        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            self->m_Pressed = true;
            self->m_Dragging = false;
            GetCursorPos(&self->m_PressCursor);
            GetWindowRect(hwnd, &self->m_PressRect);
            return 0;

        case WM_MOUSEMOVE:
            if (!self->m_Hover) {
                self->m_Hover = true;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                self->render();
            }
            if (self->m_Pressed) {
                POINT pt;
                GetCursorPos(&pt);
                int dx = pt.x - self->m_PressCursor.x, dy = pt.y - self->m_PressCursor.y;
                if (!self->m_Dragging && (abs(dx) > 4 || abs(dy) > 4)) {
                    self->m_Dragging = true;
                }
                if (self->m_Dragging) {
                    RECT rc = self->clientRectOnScreen();
                    int s = self->buttonSize();
                    int x = std::clamp((int)self->m_PressRect.left + dx, (int)rc.left, std::max((int)rc.left, (int)rc.right - s));
                    int y = std::clamp((int)self->m_PressRect.top + dy, (int)rc.top, std::max((int)rc.top, (int)rc.bottom - s));
                    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
                }
            }
            return 0;

        case WM_MOUSELEAVE:
            self->m_Hover = false;
            self->render();
            return 0;

        case WM_LBUTTONUP:
            if (self->m_Pressed) {
                bool dragged = self->m_Dragging;
                self->m_Pressed = self->m_Dragging = false;
                ReleaseCapture();
                if (dragged) {
                    self->snap();
                }
                else {
                    self->openMenuFromButton();
                }
            }
            return 0;

        case WM_RBUTTONUP:
            self->openMenuFromButton();
            return 0;

        case WM_CAPTURECHANGED:
            self->m_Pressed = false;
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_Button = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // The stream window: keep the button docked while it moves, resizes or changes mode
    static LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR subclassId, DWORD_PTR refData)
    {
        auto self = (StreamMenu*)refData;
        switch (msg) {
        case WM_WINDOWPOSCHANGED:
        case WM_SIZE:
        case WM_DPICHANGED:
            {
                LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
                self->reposition();
                return result;
            }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, parentProc, subclassId);
            break;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    Session* m_Session;
    SDL_Window* m_Window;
    HWND m_Parent;
    HWND m_Button = nullptr;
    int m_Edge = EdgeBottom;
    double m_Pos = 0.0;
    bool m_Visible = true;
    bool m_Hover = false;
    bool m_Pressed = false;
    bool m_Dragging = false;
    POINT m_PressCursor = {};
    RECT m_PressRect = {};
    int m_RenderedSize = 0;
};

StreamMenu* streamMenuCreate(Session* session, SDL_Window* window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return nullptr;
    }
    return new StreamMenu(session, window, info.info.win.window);
}

void streamMenuDestroy(StreamMenu* menu)
{
    delete menu;
}

void streamMenuToggle(StreamMenu* menu)
{
    if (menu != nullptr) {
        menu->toggle();
    }
}

#else

StreamMenu* streamMenuCreate(Session*, SDL_Window*) { return nullptr; }
void streamMenuDestroy(StreamMenu*) {}
void streamMenuToggle(StreamMenu*) {}

#endif
