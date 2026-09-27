#include "streammenu.h"

#include <QtGlobal>

#ifdef Q_OS_WIN32

#include "session.h"
#include "settings/streamingpreferences.h"
#include "settings/shortcuts.h"
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
#include <memory>
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
    CmdHalfBitrate,
    CmdPrintScreen,
    CmdScreens1 = 100,      // 100..102 = 1..3 screens
    CmdResolution = 200,    // 200 + index into the resolution list
    CmdShortcutsReset = 299,
    CmdShortcut = 300,      // 300 + index into Shortcuts::actions()
};

constexpr UINT_PTR k_ParentSubclassId = 0x4D4C534D; // 'MLSM'
constexpr int k_ButtonSizeDip = 52;  // includes room around the round button for its glow
const wchar_t* k_ButtonClass = L"MoonlightStreamMenuButton";

// The menu's look (Parsec-like): dark, an icon beside each command, red for leaving the stream
constexpr COLORREF k_MenuBack = RGB(28, 28, 30);
constexpr COLORREF k_MenuHover = RGB(54, 54, 58);
constexpr COLORREF k_MenuText = RGB(235, 235, 235);
constexpr COLORREF k_MenuIcon = RGB(205, 205, 210);
constexpr COLORREF k_MenuDim = RGB(140, 140, 148);
constexpr COLORREF k_MenuDanger = RGB(255, 96, 96);
constexpr COLORREF k_MenuLine = RGB(62, 62, 66);

// Glyphs of Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10)
namespace Glyph {
constexpr const wchar_t* FullScreen = L"";
constexpr const wchar_t* BackToWindow = L"";
constexpr const wchar_t* Minimize = L"";
constexpr const wchar_t* Metrics = L"";
constexpr const wchar_t* Volume = L"";
constexpr const wchar_t* Mute = L"";
constexpr const wchar_t* Screens = L"";
constexpr const wchar_t* Resolution = L"";
constexpr const wchar_t* Mouse = L"";
constexpr const wchar_t* Keyboard = L"";
constexpr const wchar_t* Shortcuts = L"";
constexpr const wchar_t* Camera = L"";
constexpr const wchar_t* KeyboardMouse = L"";
constexpr const wchar_t* Cursor = L"";
constexpr const wchar_t* Lock = L"";
constexpr const wchar_t* Paste = L"";
constexpr const wchar_t* Shield = L"";
constexpr const wchar_t* Hide = L"";
constexpr const wchar_t* Show = L"";
constexpr const wchar_t* Disconnect = L"";
constexpr const wchar_t* Power = L"";
constexpr const wchar_t* Check = L"";
constexpr const wchar_t* Chevron = L"";
}

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
        m_MenuBrush = CreateSolidBrush(k_MenuBack);

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
        destroyMenuFonts();
        DeleteObject(m_MenuBrush);
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

    int dpi() const
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
        return (int)dpi;
    }

    int buttonSize() const
    {
        return MulDiv(k_ButtonSizeDip, dpi(), 96);
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

    // Owner-drawn items in the native menu, so keyboard use and submenus work as usual
    struct Item {
        std::wstring icon;      // a glyph of the icon font, or none
        std::wstring text;
        std::wstring shortcut;  // right-aligned hint
        bool checked = false;
        bool danger = false;    // red: leaves the stream
        bool info = false;      // a grey note, not a command
        bool submenu = false;
        bool separator = false;
    };

    int scale(int dip) const
    {
        return MulDiv(dip, m_MenuDpi, 96);
    }

    static bool hasFont(const wchar_t* face)
    {
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
        bool found = false;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM result) -> int {
            *(bool*)result = true;
            return 0;
        }, (LPARAM)&found, 0);
        ReleaseDC(nullptr, dc);
        return found;
    }

    void createMenuFonts()
    {
        destroyMenuFonts();
        m_MenuDpi = dpi();
        auto font = [this](int px, const wchar_t* face) {
            return CreateFontW(-scale(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        };
        // Windows 11's icon font, or Windows 10's (same glyphs)
        static const wchar_t* iconFace = hasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        m_TextFont = font(14, L"Segoe UI");
        m_IconFont = font(16, iconFace);
        m_SmallIconFont = font(11, iconFace);
    }

    void destroyMenuFonts()
    {
        for (HFONT* f : {&m_TextFont, &m_IconFont, &m_SmallIconFont}) {
            if (*f != nullptr) {
                DeleteObject(*f);
                *f = nullptr;
            }
        }
    }

    Item* newItem(const wchar_t* icon, const QString& text, const QString& shortcut = QString())
    {
        m_Items.push_back(std::make_unique<Item>());
        Item* item = m_Items.back().get();
        item->icon = icon != nullptr ? icon : L"";
        item->text = text.toStdWString();
        item->shortcut = shortcut.toStdWString();
        return item;
    }

    static void insert(HMENU menu, Item* item, UINT id, HMENU sub = nullptr)
    {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STATE | MIIM_ID | (sub != nullptr ? MIIM_SUBMENU : 0);
        mii.fType = MFT_OWNERDRAW | (item->separator ? MFT_SEPARATOR : 0);
        mii.fState = (item->checked ? MFS_CHECKED : 0) | (item->info ? MFS_DISABLED : 0);
        mii.wID = id;
        mii.hSubMenu = sub;
        mii.dwItemData = (ULONG_PTR)item;
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);
    }

    void add(HMENU menu, UINT id, const wchar_t* icon, const QString& text, const QString& shortcut = QString(),
             bool checked = false, bool danger = false)
    {
        Item* item = newItem(icon, text, shortcut);
        item->checked = checked;
        item->danger = danger;
        insert(menu, item, id);
    }

    void info(HMENU menu, const QString& text)
    {
        Item* item = newItem(nullptr, text);
        item->info = true;
        insert(menu, item, 0);
    }

    void separator(HMENU menu)
    {
        Item* item = newItem(nullptr, QString());
        item->separator = true;
        insert(menu, item, 0);
    }

    void submenu(HMENU menu, HMENU sub, const wchar_t* icon, const QString& text)
    {
        Item* item = newItem(icon, text);
        item->submenu = true;
        insert(menu, item, 0, sub);
    }

    Item* findItem(ULONG_PTR data) const
    {
        for (const auto& item : m_Items) {
            if ((ULONG_PTR)item.get() == data) {
                return item.get();
            }
        }
        return nullptr;
    }

    static void fill(HDC dc, const RECT& rc, COLORREF color)
    {
        SetDCBrushColor(dc, color);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    }

    // Columns (DIPs): icon 10..38, text from 46; on the right a check mark and a submenu arrow
    bool measureItem(MEASUREITEMSTRUCT* mis)
    {
        Item* item = findItem(mis->itemData);
        if (item == nullptr) {
            return false;
        }
        if (item->separator) {
            mis->itemWidth = scale(40);
            mis->itemHeight = scale(9);
            return true;
        }

        HDC dc = GetDC(m_Parent);
        HGDIOBJ old = SelectObject(dc, m_TextFont);
        SIZE text = {}, shortcut = {};
        GetTextExtentPoint32W(dc, item->text.c_str(), (int)item->text.size(), &text);
        if (!item->shortcut.empty()) {
            GetTextExtentPoint32W(dc, item->shortcut.c_str(), (int)item->shortcut.size(), &shortcut);
        }
        SelectObject(dc, old);
        ReleaseDC(m_Parent, dc);

        mis->itemWidth = scale(46) + text.cx + (shortcut.cx > 0 ? scale(32) + shortcut.cx : 0) + scale(48);
        mis->itemHeight = scale(item->info ? 26 : 32);
        return true;
    }

    bool drawItem(DRAWITEMSTRUCT* dis)
    {
        Item* item = findItem(dis->itemData);
        if (item == nullptr) {
            return false;
        }
        HDC dc = dis->hDC;
        RECT rc = dis->rcItem;
        bool hot = (dis->itemState & ODS_SELECTED) && !item->info && !item->separator;
        fill(dc, rc, hot ? k_MenuHover : k_MenuBack);

        if (item->separator) {
            int y = (rc.top + rc.bottom) / 2;
            fill(dc, { rc.left + scale(10), y, rc.right - scale(10), y + std::max(1, scale(1)) }, k_MenuLine);
            return true;
        }

        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, m_IconFont);
        const UINT centered = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        if (!item->icon.empty()) {
            RECT r = { rc.left + scale(10), rc.top, rc.left + scale(38), rc.bottom };
            SetTextColor(dc, item->danger ? k_MenuDanger : k_MenuIcon);
            DrawTextW(dc, item->icon.c_str(), -1, &r, centered);
        }

        SelectObject(dc, m_SmallIconFont);
        if (item->checked) {
            RECT r = { rc.right - scale(44), rc.top, rc.right - scale(20), rc.bottom };
            SetTextColor(dc, k_MenuText);
            DrawTextW(dc, Glyph::Check, -1, &r, centered);
        }
        if (item->submenu) {
            RECT r = { rc.right - scale(24), rc.top, rc.right - scale(6), rc.bottom };
            SetTextColor(dc, k_MenuIcon);
            DrawTextW(dc, Glyph::Chevron, -1, &r, centered);
        }

        SelectObject(dc, m_TextFont);
        RECT textRect = { rc.left + scale(46), rc.top, rc.right - scale(48), rc.bottom };
        if (!item->shortcut.empty()) {
            SetTextColor(dc, k_MenuDim);
            DrawTextW(dc, item->shortcut.c_str(), -1, &textRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE s = {};
            GetTextExtentPoint32W(dc, item->shortcut.c_str(), (int)item->shortcut.size(), &s);
            textRect.right -= s.cx + scale(24);
        }
        SetTextColor(dc, item->info ? k_MenuDim : item->danger ? k_MenuDanger : k_MenuText);
        DrawTextW(dc, item->text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);

        if (item->submenu) {
            // Our chevron only: Windows draws its arrow afterwards, clipped away here
            ExcludeClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
        }
        return true;
    }

    void styleMenu(HMENU menu)
    {
        MENUINFO mi = {};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
        mi.hbrBack = m_MenuBrush;
        SetMenuInfo(menu, &mi);
    }

    void showMenu(int x, int y, UINT align)
    {
        // With the mouse captured (immersive), release it first so the menu is usable
        if (m_Session->isImmersive()) {
            m_Session->runShortcutCommand('Z');
        }

        // Each command shows its current (user-rebindable) shortcut
        auto keyOf = [](const char* id) { return Shortcuts::binding(id); };
        createMenuFonts();
        m_Items.clear();
        HMENU menu = CreatePopupMenu();

        // Check marks show what's on; stream-wide state lives in the main window's session
        QSettings shared;
        bool soundOn = m_Companion ? !shared.value("streammenu/muted", false).toBool() : !m_Session->isAudioMuted();
        int screenCount = m_Companion ? shared.value("extrascreens", 0).toInt() + 1 : m_Session->extraScreenCount() + 1;
        bool fullScreen = m_Session->isFullScreen();

        add(menu, CmdHideButton, canShowButton() ? Glyph::Hide : Glyph::Show,
            canShowButton() ? "Hide button" : "Show the menu button", keyOf("menu_button"));
        add(menu, CmdFullScreen, fullScreen ? Glyph::BackToWindow : Glyph::FullScreen, "Fullscreen",
            keyOf("fullscreen_toggle"), fullScreen);
        add(menu, CmdMinimize, Glyph::Minimize, "Minimize", keyOf("minimize"));
        add(menu, CmdSound, soundOn ? Glyph::Volume : Glyph::Mute, "Sound", QString(), soundOn);
        add(menu, CmdMetrics, Glyph::Metrics, "Metrics", keyOf("metrics"), m_Session->isStatsOverlayVisible());
        separator(menu);

        HMENU screens = CreatePopupMenu();
        for (int n = 1; n <= 3; n++) {
            add(screens, CmdScreens1 + n - 1, nullptr, n == 1 ? QString("1 screen") : QString("%1 screens").arg(n),
                QString(), n == screenCount);
        }
        separator(screens);
        add(screens, CmdHalfBitrate, nullptr, "Extra screens at half bitrate (next launch)", QString(),
            shared.value("extrascreenshalfbitrate", false).toBool());
        submenu(menu, screens, Glyph::Screens, "Screens");

        HMENU resolutionMenu = CreatePopupMenu();
        auto list = resolutions();
        for (size_t i = 0; i < list.size(); i++) {
            bool current = list[i].w == m_Session->streamWidth() && list[i].h == m_Session->streamHeight();
            add(resolutionMenu, CmdResolution + (UINT)i, nullptr, list[i].label, QString(), current);
        }
        separator(resolutionMenu);
        info(resolutionMenu, "Changing it reconnects (a few seconds)");
        submenu(menu, resolutionMenu, Glyph::Resolution, "Resolution");
        separator(menu);

        add(menu, CmdImmersive, Glyph::Mouse, "Immersive mode (capture mouse)", keyOf("immersive"), m_Session->isImmersive());
        add(menu, CmdSystemKeys, Glyph::Keyboard, "Keyboard immersive (Alt+Tab, Win key)", keyOf("keyboard_immersive"),
            m_Session->isKeyboardImmersive());
        add(menu, CmdPrintScreen, Glyph::Camera, "Print Screen goes to the host", QString(), Session::isPrintScreenToHost());
        add(menu, CmdReleaseInput, Glyph::KeyboardMouse, "Release mouse and keyboard", keyOf("release"));
        add(menu, CmdCursor, Glyph::Cursor, "Show local cursor", keyOf("cursor"), m_Session->isLocalCursorVisible());
        add(menu, CmdLockCursor, Glyph::Lock, "Lock cursor to window", keyOf("lock_cursor"), m_Session->isCursorLocked());
        add(menu, CmdPaste, Glyph::Paste, "Paste clipboard as text", keyOf("paste"));
        add(menu, CmdCtrlAltDel, Glyph::Shield, "Send Ctrl+Alt+Del", keyOf("ctrl_alt_del"));

        HMENU shortcutsMenu = CreatePopupMenu();
        const auto& actions = Shortcuts::actions();
        for (int i = 0; i < actions.size(); i++) {
            QString binding = Shortcuts::binding(actions[i].id);
            add(shortcutsMenu, CmdShortcut + (UINT)i, nullptr, actions[i].label, binding.isEmpty() ? QString("None") : binding);
        }
        separator(shortcutsMenu);
        add(shortcutsMenu, CmdShortcutsReset, nullptr, "Reset all to defaults");
        separator(shortcutsMenu);
        info(shortcutsMenu, "Pick one, then press the new keys");
        submenu(menu, shortcutsMenu, Glyph::Shortcuts, "Keyboard shortcuts");
        separator(menu);

        add(menu, CmdDisconnect, Glyph::Disconnect, "Disconnect", keyOf("disconnect"), false, true);
        add(menu, CmdQuitAppAndExit, Glyph::Power, "Quit app and exit Moonlight", keyOf("quit_exit"), false, true);
        styleMenu(menu);

        // The stream window owns the menu, so it keeps keyboard focus
        SetForegroundWindow(m_Parent);
        UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | align,
                                          x, y, m_Parent, nullptr);
        DestroyMenu(menu); // also destroys the submenus
        m_Items.clear();
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
        const auto& actions = Shortcuts::actions();
        if (cmd >= CmdShortcut && cmd < CmdShortcut + (UINT)actions.size()) {
            const auto& action = actions[cmd - CmdShortcut];
            m_Session->startShortcutCapture(action.id, action.label);
            return;
        }
        if (cmd == CmdShortcutsReset) {
            Shortcuts::resetAll();
            m_Session->reloadShortcuts();
            m_Session->showStatusMessage("Keyboard shortcuts reset to the defaults", true);
            return;
        }

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
        case CmdSystemKeys:     m_Session->toggleKeyboardImmersive(); break;
        case CmdPaste:          m_Session->runShortcutCommand('V'); break;
        case CmdCtrlAltDel:     m_Session->sendCtrlAltDel(); break;
        case CmdHideButton:     toggle(); break;
        case CmdPrintScreen:    m_Session->togglePrintScreenToHost(); break;
        case CmdHalfBitrate:
            // Any screen's window can flip it, so start from the stored value
            StreamingPreferences::get()->extraScreensHalfBitrate =
                    !QSettings().value("extrascreenshalfbitrate", false).toBool();
            StreamingPreferences::get()->save();
            break;
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
        case WM_MEASUREITEM:
            // The menu's owner-drawn items
            if (((MEASUREITEMSTRUCT*)lParam)->CtlType == ODT_MENU && self->measureItem((MEASUREITEMSTRUCT*)lParam)) {
                return TRUE;
            }
            break;
        case WM_DRAWITEM:
            if (((DRAWITEMSTRUCT*)lParam)->CtlType == ODT_MENU && self->drawItem((DRAWITEMSTRUCT*)lParam)) {
                return TRUE;
            }
            break;
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

    // The menu while it's open: its items, and fonts for the monitor's DPI
    std::vector<std::unique_ptr<Item>> m_Items;
    HFONT m_TextFont = nullptr;
    HFONT m_IconFont = nullptr;
    HFONT m_SmallIconFont = nullptr;
    HBRUSH m_MenuBrush = nullptr;
    int m_MenuDpi = 96;
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
