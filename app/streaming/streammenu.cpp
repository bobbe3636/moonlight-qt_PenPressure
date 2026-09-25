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
constexpr int k_ButtonSizeDip = 52;  // includes room around the round button for its glow
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
        : m_Session(session), m_Window(window), m_Parent(parent),
          m_Companion(session->isCompanion()), m_Main((HWND)session->companionParentWindow())
    {
        QSettings settings;
        // Default: bottom-left corner, like Parsec
        m_FracX = qBound(0.0, settings.value("streammenu/x", 0.02).toDouble(), 1.0);
        m_FracY = qBound(0.0, settings.value("streammenu/y", 0.97).toDouble(), 1.0);
        m_Visible = StreamingPreferences::get()->showStreamMenuButton;
        if (!m_Companion) {
            // Shared with the extra screens' menus so their Sound check mark is right
            settings.setValue("streammenu/muted", m_Session->isAudioMuted());
        }

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

        // Wherever it was dropped, as a proportion of the window (so it keeps its place
        // across resizes and fullscreen toggles)
        int s = buttonSize();
        RECT rc = clientRectOnScreen();
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        int x = rc.left + (int)(m_FracX * std::max(0, w - s));
        int y = rc.top + (int)(m_FracY * std::max(0, h - s));
        if (s != m_RenderedSize) {
            render();
        }
        SetWindowPos(m_Button, nullptr, x, y, s, s, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    // Remember where the button was dropped (free placement, no docking)
    void savePosition()
    {
        RECT br, rc = clientRectOnScreen();
        GetWindowRect(m_Button, &br);
        int s = buttonSize();
        m_FracX = qBound(0.0, (double)(br.left - rc.left) / std::max(1, (int)(rc.right - rc.left) - s), 1.0);
        m_FracY = qBound(0.0, (double)(br.top - rc.top) / std::max(1, (int)(rc.bottom - rc.top) - s), 1.0);

        QSettings settings;
        settings.setValue("streammenu/x", m_FracX);
        settings.setValue("streammenu/y", m_FracY);
        reposition();
    }

    // ---- drawing: round button, Moonlight logo, circular highlight ring (per-pixel alpha) ----

    void render()
    {
        int s = buttonSize();
        m_RenderedSize = s;

        QImage image(s, s, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);

            const QPointF c(s / 2.0, s / 2.0);
            const double r = s * 0.40;              // the button; the rest is room for glow/shadow
            const QColor accent(110, 160, 255);     // highlight colour

            // Soft drop shadow
            for (int i = 3; i >= 1; i--) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 22));
                painter.drawEllipse(c + QPointF(0, s * 0.02), r + i * s * 0.018, r + i * s * 0.018);
            }

            // Outer glow while hovered
            if (m_Hover) {
                for (int i = 4; i >= 1; i--) {
                    QColor glow = accent;
                    glow.setAlpha(18 * (5 - i));
                    painter.setPen(QPen(glow, s * 0.03));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(c, r + i * s * 0.02, r + i * s * 0.02);
                }
            }

            // Disc: darker when pressed, a touch lighter when hovered
            QRadialGradient fill(c - QPointF(0, r * 0.4), r * 1.4);
            int base = m_Pressed ? 14 : (m_Hover ? 40 : 24);
            fill.setColorAt(0, QColor(base + 18, base + 18, base + 24, 235));
            fill.setColorAt(1, QColor(base, base, base + 4, 225));
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawEllipse(c, r, r);

            // The circular highlight ring
            QColor ring = m_Hover ? accent : QColor(255, 255, 255, 70);
            if (m_Pressed) {
                ring.setAlpha(150);
            }
            painter.setPen(QPen(ring, m_Hover ? s * 0.045 : s * 0.03));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(c, r - s * 0.015, r - s * 0.015);

            // Logo, slightly smaller while pressed
            QSvgRenderer logo(QString(":/res/moonlight.svg"));
            double half = r * (m_Pressed ? 0.56 : 0.62);
            logo.render(&painter, QRectF(c.x() - half, c.y() - half, 2 * half, 2 * half));
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

        // Check marks show what's on; stream-wide state lives in the main window's session
        QSettings shared;
        bool soundOn = m_Companion ? !shared.value("streammenu/muted", false).toBool() : !m_Session->isAudioMuted();
        int screenCount = m_Companion ? shared.value("extrascreens", 0).toInt() + 1 : m_Session->extraScreenCount() + 1;

        add(menu, CmdFullScreen, "Fullscreen" + key + "X", m_Session->isFullScreen());
        add(menu, CmdMinimize, "Minimize" + key + "D");
        add(menu, CmdMetrics, "Metrics" + key + "S", m_Session->isStatsOverlayVisible());
        add(menu, CmdSound, "Sound", soundOn);
        separator(menu);

        HMENU screens = CreatePopupMenu();
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
        add(menu, CmdCursor, "Show local cursor" + key + "C", m_Session->isLocalCursorVisible());
        add(menu, CmdLockCursor, "Lock cursor to window" + key + "L", m_Session->isCursorLocked());
        add(menu, CmdSystemKeys, "Capture system keys" + key + "K", m_Session->isSystemKeysCaptured());
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

    // Commands about the whole stream: the main window runs them; an extra screen's window
    // forwards them there. Resolutions travel as width/height in lParam.
    static bool isStreamWide(UINT cmd)
    {
        return cmd == CmdDisconnect || cmd == CmdQuitAppAndExit || cmd == CmdSound ||
               (cmd >= CmdScreens1 && cmd < CmdScreens1 + 3) || cmd == CmdResolution;
    }

    static UINT commandMessage()
    {
        static UINT message = RegisterWindowMessageW(L"MoonlightStreamMenuCommand");
        return message;
    }

    void runStreamWide(UINT cmd, LPARAM lParam)
    {
        if (m_Companion) {
            if (m_Main != nullptr && IsWindow(m_Main)) {
                PostMessageW(m_Main, commandMessage(), cmd, lParam);
            }
            return;
        }

        switch (cmd) {
        case CmdDisconnect:     m_Session->runShortcutCommand('Q'); break;
        case CmdQuitAppAndExit: m_Session->runShortcutCommand('E'); break;
        case CmdSound:
            m_Session->setAudioMuted(!m_Session->isAudioMuted());
            QSettings().setValue("streammenu/muted", m_Session->isAudioMuted());
            break;
        case CmdResolution:
            if (LOWORD(lParam) != m_Session->streamWidth() || HIWORD(lParam) != m_Session->streamHeight()) {
                m_Session->reconnectWithResolution(LOWORD(lParam), HIWORD(lParam));
            }
            break;
        default:
            if (cmd >= CmdScreens1 && cmd < CmdScreens1 + 3) {
                m_Session->setExtraScreenCount((int)(cmd - CmdScreens1));
            }
            break;
        }
    }

    void run(UINT cmd, const std::vector<Resolution>& list)
    {
        if (cmd >= CmdResolution && cmd < CmdResolution + list.size()) {
            const auto& r = list[cmd - CmdResolution];
            runStreamWide(CmdResolution, MAKELPARAM(r.w, r.h));
            return;
        }
        if (isStreamWide(cmd)) {
            runStreamWide(cmd, 0);
            return;
        }

        switch (cmd) {
        case 0: break; // dismissed
        case CmdFullScreen:     m_Session->runShortcutCommand('X'); break;
        case CmdMinimize:       m_Session->runShortcutCommand('D'); break;
        case CmdMetrics:        m_Session->runShortcutCommand('S'); break;
        case CmdImmersive:      m_Session->toggleImmersive(); break;
        case CmdReleaseInput:   m_Session->runShortcutCommand('Z'); break;
        case CmdCursor:         m_Session->runShortcutCommand('C'); break;
        case CmdLockCursor:     m_Session->runShortcutCommand('L'); break;
        case CmdSystemKeys:     m_Session->runShortcutCommand('K'); break;
        case CmdPaste:          m_Session->runShortcutCommand('V'); break;
        case CmdCtrlAltDel:     m_Session->sendCtrlAltDel(); break;
        case CmdHideButton:     toggle(); break;
        default: break;
        }
    }

    void openMenuFromButton()
    {
        // Open towards the middle of the window from wherever the button sits
        RECT br;
        GetWindowRect(m_Button, &br);
        bool right = m_FracX > 0.5, below = m_FracY > 0.5;
        int x = right ? br.left : br.right;
        int y = below ? br.bottom : br.top;
        showMenu(x, y, (right ? TPM_RIGHTALIGN : TPM_LEFTALIGN) | (below ? TPM_BOTTOMALIGN : TPM_TOPALIGN));
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
            self->render();
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
                self->render();
                if (dragged) {
                    self->savePosition();
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
            if (self->m_Pressed) {
                self->m_Pressed = false;
                self->render();
            }
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
        if (msg == commandMessage() && !self->m_Companion) {
            // A stream-wide command from an extra screen's menu
            self->runStreamWide((UINT)wParam, lParam);
            return 0;
        }
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
    double m_FracX = 0.02;  // button position as a proportion of the window
    double m_FracY = 0.97;
    bool m_Visible = true;
    bool m_Hover = false;
    bool m_Pressed = false;
    bool m_Dragging = false;
    POINT m_PressCursor = {};
    RECT m_PressRect = {};
    int m_RenderedSize = 0;
    bool m_Companion = false;   // an extra screen's window: stream-wide commands go to m_Main
    HWND m_Main = nullptr;
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
