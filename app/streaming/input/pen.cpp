#include "input.h"

#include <QtGlobal>
#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32

#include <SDL_syswm.h>
#include <commctrl.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

// Native Windows pen input.
//
// SDL2 doesn't handle WM_POINTER, so pen input normally reaches us as synthesized mouse
// events (no pressure, tilt, eraser or hover), or at best as a "pen" touch device with
// pressure only. Instead we subclass the stream window and read WM_POINTER pen data
// directly, including coalesced history samples, and send it with LiSendPenEvent().
// Pen pointer messages we consume never reach DefWindowProc, so Windows doesn't also
// promote them to mouse input.

static constexpr UINT_PTR k_PenSubclassId = 0x4D4C5045; // 'MLPE'
static constexpr double k_Pi = 3.14159265358979323846;

// Raw Wacom reports.
//
// Windows Ink only exposes one barrel button (and only while touching), 1024 pressure levels,
// and the tablet driver turns the upper button into a local middle click. Wacom's pen tablets
// also deliver their native report 0x10 on a vendor-defined HID collection (usage page 0xFF00,
// usage 0x0A; Wacom's router filter wraps it as report 0xDC), which other readers can open
// alongside the driver. From it we take both barrel switches and the full-resolution pressure;
// position, tilt and timing still come from WM_POINTER.
//
// Report 0x10: [0] id, [1] flags (tip, barrel1, barrel2, eraser, invert, in range, ...),
// [2..4] X, [5..7] Y, [8..9] pressure (0..8191), [10..11] tilt, [16] hover distance (0..63).

struct WacomRawReader
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::thread thread;
    std::atomic<bool> stop { false };
    std::atomic<uint8_t> flags { 0 };
    std::atomic<uint16_t> pressure { 0 };
    std::atomic<uint8_t> distance { 0 };
    std::atomic<uint64_t> lastReportMs { 0 };
    std::atomic<bool> seenReport { false };
    std::atomic<bool> exited { false };
    std::atomic<uint32_t> reportCount { 0 };  // for the pen statistics log line

    static constexpr uint8_t k_FlagBarrel1 = 0x02;
    static constexpr uint8_t k_FlagBarrel2 = 0x04;
    static constexpr float k_MaxPressure = 8191.0f;
    static constexpr float k_MaxDistance = 63.0f;

    // Only trust raw state that is current
    bool fresh() const
    {
        return seenReport && GetTickCount64() - lastReportMs < 100;
    }

    void run()
    {
        uint8_t buf[1024];
        while (!stop) {
            DWORD n = 0;
            if (!ReadFile(handle, buf, sizeof(buf), &n, nullptr)) {
                break;
            }

            const uint8_t* report = buf;
            if (n >= 28 && buf[0] == 0xDC && buf[1] == 0x10) {
                report = buf + 1; // unwrap Wacom's router filter
                n -= 1;
            }
            if (n < 10 || report[0] != 0x10) {
                continue;
            }

            flags = report[1];
            pressure = (uint16_t)(report[8] | (report[9] << 8));
            if (n >= 17) {
                distance = report[16];
            }
            lastReportMs = GetTickCount64();
            reportCount++;
            if (!seenReport) {
                seenReport = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using raw Wacom reports for pen buttons and pressure");
            }
        }
        exited = true;
    }

    static WacomRawReader* open()
    {
        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devs == INVALID_HANDLE_VALUE) {
            return nullptr;
        }

        WacomRawReader* reader = nullptr;
        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(ifData);
        for (DWORD i = 0; reader == nullptr && SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &ifData); i++) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, &size, nullptr);
            std::vector<uint8_t> detailBuf(size);
            auto detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detailBuf.data();
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifData, detail, size, nullptr, nullptr) ||
                    wcsstr(CharLowerW(detail->DevicePath), L"vid_056a") == nullptr) {
                continue;
            }

            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                continue;
            }

            bool match = false;
            PHIDP_PREPARSED_DATA ppd;
            if (HidD_GetPreparsedData(h, &ppd)) {
                HIDP_CAPS caps;
                match = HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS &&
                        caps.UsagePage == 0xFF00 && caps.Usage == 0x0A;
                HidD_FreePreparsedData(ppd);
            }

            if (match) {
                reader = new WacomRawReader();
                reader->handle = h;
                reader->thread = std::thread(&WacomRawReader::run, reader);
            }
            else {
                CloseHandle(h);
            }
        }

        SetupDiDestroyDeviceInfoList(devs);
        return reader;
    }

    ~WacomRawReader()
    {
        stop = true;
        if (thread.joinable()) {
            // The reader blocks in a synchronous ReadFile until the pen next reports; cancel
            // until it has actually left (a cancel can land just before its next read starts)
            while (!exited) {
                CancelIoEx(handle, nullptr);
                CancelSynchronousIo(thread.native_handle());
                Sleep(5);
            }
            thread.join();
        }
        CloseHandle(handle);
    }
};

// Wintab.
//
// The tablet driver's own pen API (Wacom's, and most other makers ship one too) reports the
// tablet's full pressure range for any model, where Windows Ink stops at 1024 levels. We read
// only pressure from it; position, tilt and buttons keep coming from WM_POINTER. Wintab32.dll
// is loaded at runtime, so nothing changes where there's no Wintab driver.
//
// Wintab delivers packets to one context at a time (the top of its overlap order): the stream
// window the pen enters brings its context to the top, so with several screens' windows the
// one under the pen gets the pressure.

namespace wintab {
    DECLARE_HANDLE(HCTX);
    typedef DWORD WTPKT;
    typedef DWORD FIX32;

    constexpr UINT WTI_DEFCONTEXT = 3;
    constexpr UINT WTI_DEVICES = 100;
    constexpr UINT DVC_NPRESSURE = 15;
    constexpr UINT CXO_MESSAGES = 0x0004;
    constexpr WTPKT PK_STATUS = 0x0002;
    constexpr WTPKT PK_TIME = 0x0004;
    constexpr WTPKT PK_NORMAL_PRESSURE = 0x0400;
    constexpr UINT WT_DEFBASE = 0x7FF0;
    constexpr UINT WT_PACKET = WT_DEFBASE;

    struct AXIS {
        LONG axMin;
        LONG axMax;
        UINT axUnits;
        FIX32 axResolution;
    };

    struct LOGCONTEXTW {
        WCHAR lcName[40];
        UINT lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
        WTPKT lcPktData, lcPktMode, lcMoveMask;
        DWORD lcBtnDnMask, lcBtnUpMask;
        LONG lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
        LONG lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
        FIX32 lcSensX, lcSensY, lcSensZ;
        BOOL lcSysMode;
        int lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
        FIX32 lcSysSensX, lcSysSensY;
    };

    // Fields in the order of their PK_ bits: PK_STATUS, PK_TIME, PK_NORMAL_PRESSURE
    struct PACKET {
        UINT status;
        DWORD time;
        UINT normalPressure;
    };
}

struct WintabReader
{
    HMODULE dll = nullptr;
    wintab::HCTX ctx = nullptr;
    float maxPressure = 1023.0f;
    std::atomic<uint32_t> pressure { 0 };
    std::atomic<uint64_t> lastPacketMs { 0 };
    std::atomic<uint32_t> packetCount { 0 };  // for the pen statistics log line
    bool seenPacket = false;

    UINT (WINAPI* fnInfo)(UINT, UINT, LPVOID) = nullptr;
    wintab::HCTX (WINAPI* fnOpen)(HWND, wintab::LOGCONTEXTW*, BOOL) = nullptr;
    BOOL (WINAPI* fnClose)(wintab::HCTX) = nullptr;
    BOOL (WINAPI* fnPacket)(wintab::HCTX, UINT, LPVOID) = nullptr;
    BOOL (WINAPI* fnOverlap)(wintab::HCTX, BOOL) = nullptr;

    // Only trust pressure that is current
    bool fresh() const
    {
        return seenPacket && GetTickCount64() - lastPacketMs < 100;
    }

    static WintabReader* open(HWND hwnd)
    {
        HMODULE dll = LoadLibraryW(L"Wintab32.dll");
        if (dll == nullptr) {
            return nullptr;
        }

        auto reader = new WintabReader();
        reader->dll = dll;
        reader->fnInfo = (decltype(fnInfo))GetProcAddress(dll, "WTInfoW");
        reader->fnOpen = (decltype(fnOpen))GetProcAddress(dll, "WTOpenW");
        reader->fnClose = (decltype(fnClose))GetProcAddress(dll, "WTClose");
        reader->fnPacket = (decltype(fnPacket))GetProcAddress(dll, "WTPacket");
        reader->fnOverlap = (decltype(fnOverlap))GetProcAddress(dll, "WTOverlap");
        if (!reader->fnInfo || !reader->fnOpen || !reader->fnClose || !reader->fnPacket || !reader->fnOverlap ||
                reader->fnInfo(0, 0, nullptr) == 0) {
            // No Wintab service running (e.g. the driver's Wintab is off)
            delete reader;
            return nullptr;
        }

        wintab::AXIS pressureAxis = {};
        if (reader->fnInfo(wintab::WTI_DEVICES, wintab::DVC_NPRESSURE, &pressureAxis) && pressureAxis.axMax > 0) {
            reader->maxPressure = (float)pressureAxis.axMax;
        }

        // A digitizing context (not a system one: the driver keeps moving the cursor) that
        // posts WT_PACKET to the stream window with just the pressure
        wintab::LOGCONTEXTW lc = {};
        if (!reader->fnInfo(wintab::WTI_DEFCONTEXT, 0, &lc)) {
            delete reader;
            return nullptr;
        }
        wcscpy_s(lc.lcName, L"Moonlight pen pressure");
        lc.lcOptions |= wintab::CXO_MESSAGES;
        lc.lcMsgBase = wintab::WT_DEFBASE;
        lc.lcPktData = wintab::PK_STATUS | wintab::PK_TIME | wintab::PK_NORMAL_PRESSURE;
        lc.lcPktMode = 0;  // absolute
        lc.lcMoveMask = wintab::PK_NORMAL_PRESSURE;
        reader->ctx = reader->fnOpen(hwnd, &lc, TRUE);
        if (reader->ctx == nullptr) {
            delete reader;
            return nullptr;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Wintab available: %.0f pressure levels", reader->maxPressure + 1);
        return reader;
    }

    void onPacket(WPARAM serial)
    {
        wintab::PACKET packet;
        if (fnPacket(ctx, (UINT)serial, &packet)) {
            pressure = packet.normalPressure;
            lastPacketMs = GetTickCount64();
            packetCount++;
            if (!seenPacket) {
                seenPacket = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using Wintab for pen pressure");
            }
        }
    }

    // The pen entered this window: its context gets the packets from now on
    void bringToTop()
    {
        fnOverlap(ctx, TRUE);
    }

    ~WintabReader()
    {
        if (ctx != nullptr) {
            fnClose(ctx);
        }
        FreeLibrary(dll);
    }
};

// Pen statistics, logged every 2 s while the pen is active ("Pen stats: ..."), to see how
// evenly and how often samples leave this PC. Only touched on the UI thread.
static struct {
    uint64_t windowStart = 0;
    uint64_t lastMessage = 0;
    uint32_t maxGapMs = 0;
    uint32_t messages = 0, sent = 0, contactSamples = 0, sharedPressure = 0;
    uint32_t rawAtStart = 0, wintabAtStart = 0;
} s_Stats;

static void logPenStats(WacomRawReader* raw, WintabReader* wt)
{
    uint64_t now = GetTickCount64();
    if (s_Stats.lastMessage != 0 && now - s_Stats.lastMessage < 500) {
        s_Stats.maxGapMs = qMax(s_Stats.maxGapMs, (uint32_t)(now - s_Stats.lastMessage));
    }
    s_Stats.lastMessage = now;
    if (s_Stats.windowStart == 0) {
        s_Stats.windowStart = now;
        s_Stats.rawAtStart = raw != nullptr ? raw->reportCount.load() : 0;
        s_Stats.wintabAtStart = wt != nullptr ? wt->packetCount.load() : 0;
        return;
    }

    uint64_t elapsed = now - s_Stats.windowStart;
    if (elapsed < 2000) {
        return;
    }
    uint32_t rawReports = raw != nullptr ? raw->reportCount.load() - s_Stats.rawAtStart : 0;
    uint32_t wintabPackets = wt != nullptr ? wt->packetCount.load() - s_Stats.wintabAtStart : 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Pen stats: %.0f messages/s, %.0f samples sent/s, raw tablet %.0f reports/s, Wintab %.0f packets/s, "
                "max gap %u ms, %u/%u contact samples used the latest raw/Wintab pressure",
                s_Stats.messages * 1000.0 / elapsed, s_Stats.sent * 1000.0 / elapsed,
                rawReports * 1000.0 / elapsed, wintabPackets * 1000.0 / elapsed, s_Stats.maxGapMs,
                s_Stats.sharedPressure, s_Stats.contactSamples);
    s_Stats = {};
}

static LRESULT CALLBACK penSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR refData)
{
    switch (msg) {
    case WM_POINTERENTER:
    case WM_POINTERLEAVE:
    case WM_POINTERDOWN:
    case WM_POINTERUP:
    case WM_POINTERUPDATE:
    case WM_POINTERCAPTURECHANGED:
    case wintab::WT_PACKET:
        if (((SdlInputHandler*)refData)->handleNativePenMessage(hwnd, msg, wParam)) {
            return 0;
        }
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (((SdlInputHandler*)refData)->handleNativePenMouseButton(msg, wParam)) {
            // WM_XBUTTON* must return TRUE when processed
            return (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP) ? TRUE : 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, penSubclassProc, subclassId);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void SdlInputHandler::installNativePenHook()
{
    SDL_SysWMinfo info;

    if (m_Window == nullptr || m_NativePenHwnd != nullptr) {
        return;
    }

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(m_Window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return;
    }

    if (SetWindowSubclass(info.info.win.window, penSubclassProc, k_PenSubclassId, (DWORD_PTR)this)) {
        m_NativePenHwnd = info.info.win.window;
        // MOONLIGHT_PEN_NO_RAW=1 skips the raw Wacom reader (to try Wintab on a supported model)
        if (qEnvironmentVariableIntValue("MOONLIGHT_PEN_NO_RAW") == 0) {
            m_WacomRaw = WacomRawReader::open();
        }
        // Off by default: while a Wintab context is open, Wacom's driver stops sending this
        // window Windows Ink pen messages (the pen becomes a mouse), which this path needs.
        // MOONLIGHT_PEN_WINTAB=1 turns it on for experiments.
        if (qEnvironmentVariableIntValue("MOONLIGHT_PEN_WINTAB") != 0) {
            m_Wintab = WintabReader::open(info.info.win.window);
        }
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SetWindowSubclass() failed for native pen input: %lu",
                    GetLastError());
    }
}

void SdlInputHandler::removeNativePenHook()
{
    if (m_NativePenHwnd != nullptr) {
        if (IsWindow((HWND)m_NativePenHwnd)) {
            RemoveWindowSubclass((HWND)m_NativePenHwnd, penSubclassProc, k_PenSubclassId);
        }
        m_NativePenHwnd = nullptr;
    }

    delete (WacomRawReader*)m_WacomRaw;
    m_WacomRaw = nullptr;
    delete (WintabReader*)m_Wintab;
    m_Wintab = nullptr;
}

bool SdlInputHandler::handleNativePenMessage(void* hwndPtr, unsigned int msg, uintptr_t wParam)
{
    HWND hwnd = (HWND)hwndPtr;
    auto wt = (WintabReader*)m_Wintab;
    if (msg == wintab::WT_PACKET) {
        if (wt != nullptr) {
            wt->onPacket(wParam);
        }
        return true;
    }

    UINT32 pointerId = GET_POINTERID_WPARAM(wParam);

    POINTER_INPUT_TYPE pointerType;
    if (!GetPointerType(pointerId, &pointerType) || pointerType != PT_PEN) {
        return false;
    }

    // Without host support, let Windows turn the pen into mouse input as before
    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
        return false;
    }

    if (!m_NativePenLogged) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using native Windows pen input");
        m_NativePenLogged = true;
    }
    if (!m_DisabledTouchFeedback) {
        // No press-and-hold rings or tap ripples on top of the stream
        disableTouchFeedback();
        m_DisabledTouchFeedback = true;
    }

    if (msg == WM_POINTERENTER && wt != nullptr) {
        wt->bringToTop();
    }

    if (msg == WM_POINTERCAPTURECHANGED) {
        LiSendPenEvent(LI_TOUCH_EVENT_CANCEL, LI_TOOL_TYPE_UNKNOWN, 0, 0.0f, 0.0f, 0.0f,
                       0.0f, 0.0f, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
        return true;
    }

    // Newest sample first; history holds the samples Windows coalesced since the last message
    POINTER_INFO pointerInfo;
    UINT32 count = 1;
    if (GetPointerInfo(pointerId, &pointerInfo) && pointerInfo.historyCount > 1) {
        count = pointerInfo.historyCount;
    }
    // Reused across messages (pen input arrives at hundreds of messages per second)
    static std::vector<POINTER_PEN_INFO> samples;
    samples.resize(count);
    if (!GetPointerPenInfoHistory(pointerId, &count, samples.data())) {
        count = 1;
        if (!GetPointerPenInfo(pointerId, &samples[0])) {
            return false;
        }
    }

    // Pen himetric coordinates mapped through the device rect are sub-pixel precise
    RECT deviceRect, displayRect;
    bool haveDeviceRects = GetPointerDeviceRects(samples[0].pointerInfo.sourceDevice, &deviceRect, &displayRect) &&
                           deviceRect.right > deviceRect.left && deviceRect.bottom > deviceRect.top;

    // Everything below is in physical pixels (Moonlight is per-monitor DPI aware)
    POINT clientOrigin = {};
    RECT clientRect;
    ClientToScreen(hwnd, &clientOrigin);
    GetClientRect(hwnd, &clientRect);
    if (clientRect.right <= 0 || clientRect.bottom <= 0) {
        return true;
    }

    SDL_Rect src, dst;
    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;
    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    for (int i = (int)count - 1; i >= 0; i--) {
        const POINTER_PEN_INFO& pen = samples[i];
        POINTER_FLAGS flags = pen.pointerInfo.pointerFlags;

        float screenX = (float)pen.pointerInfo.ptPixelLocation.x;
        float screenY = (float)pen.pointerInfo.ptPixelLocation.y;
        if (haveDeviceRects) {
            float hx = displayRect.left + (float)(pen.pointerInfo.ptHimetricLocation.x - deviceRect.left) *
                       (displayRect.right - displayRect.left) / (deviceRect.right - deviceRect.left);
            float hy = displayRect.top + (float)(pen.pointerInfo.ptHimetricLocation.y - deviceRect.top) *
                       (displayRect.bottom - displayRect.top) / (deviceRect.bottom - deviceRect.top);

            // Some drivers report himetric in another space; only trust it if it agrees
            if (std::fabs(hx - screenX) < 4.0f && std::fabs(hy - screenY) < 4.0f) {
                screenX = hx;
                screenY = hy;
            }
        }

        // Window-relative, then scaled and clamped to the video region like touch input
        float windowX = (screenX - clientOrigin.x) * windowWidth / clientRect.right;
        float windowY = (screenY - clientOrigin.y) * windowHeight / clientRect.bottom;
        float vidrelx = qMin(qMax(windowX, (float)dst.x), (float)(dst.x + dst.w)) - dst.x;
        float vidrely = qMin(qMax(windowY, (float)dst.y), (float)(dst.y + dst.h)) - dst.y;
        m_LastPenX = vidrelx / dst.w;
        m_LastPenY = vidrely / dst.h;

        uint8_t eventType;
        if (msg == WM_POINTERLEAVE && i == 0) {
            eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
        }
        else if (flags & POINTER_FLAG_DOWN) {
            eventType = LI_TOUCH_EVENT_DOWN;
        }
        else if (flags & POINTER_FLAG_UP) {
            eventType = LI_TOUCH_EVENT_UP;
        }
        else if (flags & POINTER_FLAG_INCONTACT) {
            eventType = LI_TOUCH_EVENT_MOVE;
        }
        else if (flags & POINTER_FLAG_INRANGE) {
            eventType = LI_TOUCH_EVENT_HOVER;
        }
        else {
            eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
        }

        uint8_t toolType = (pen.penFlags & (PEN_FLAG_INVERTED | PEN_FLAG_ERASER)) ?
                               LI_TOOL_TYPE_ERASER : LI_TOOL_TYPE_PEN;
        uint8_t penButtons = (pen.penFlags & PEN_FLAG_BARREL) ? LI_PEN_BUTTON_PRIMARY : 0;

        // Contact: pressure 0..1 (Windows reports 0..1024); 0.0 means unknown.
        // Hover: distance, which Windows doesn't report, so 0.0 (unknown) as well.
        float pressureOrDistance = 0.0f;
        if ((flags & POINTER_FLAG_INCONTACT) && (pen.penMask & PEN_MASK_PRESSURE)) {
            pressureOrDistance = qMin(pen.pressure / 1024.0f, 1.0f);
        }

        // Wintab, when current, gives the tablet's full pressure range (any model)
        auto raw = (WacomRawReader*)m_WacomRaw;
        bool rawFresh = raw != nullptr && raw->fresh();
        if (!rawFresh && wt != nullptr && wt->fresh() && (flags & POINTER_FLAG_INCONTACT)) {
            uint32_t wintabPressure = wt->pressure;
            if (wintabPressure > 0) {
                pressureOrDistance = qMin(wintabPressure / wt->maxPressure, 1.0f);
            }
        }

        // Raw Wacom state, when current, gives both barrel buttons (hovering too) and 8192 levels
        if (rawFresh) {
            uint8_t rawFlags = raw->flags;
            penButtons = ((rawFlags & WacomRawReader::k_FlagBarrel1) ? LI_PEN_BUTTON_PRIMARY : 0) |
                         ((rawFlags & WacomRawReader::k_FlagBarrel2) ? LI_PEN_BUTTON_SECONDARY : 0);
            if (flags & POINTER_FLAG_INCONTACT) {
                uint16_t rawPressure = raw->pressure;
                if (rawPressure > 0) {
                    pressureOrDistance = qMin(rawPressure / WacomRawReader::k_MaxPressure, 1.0f);
                }
            }
            else if (eventType == LI_TOUCH_EVENT_HOVER) {
                // Hover height: 1.0 = farthest the tablet senses; 0.0 would mean "unknown"
                uint8_t rawDistance = qMax<uint8_t>(raw->distance, 1);
                pressureOrDistance = qMin(rawDistance / WacomRawReader::k_MaxDistance, 1.0f);
            }
        }

        // Windows gives X/Y tilt; the protocol wants tilt from vertical plus azimuth.
        // This is the exact inverse of the host-side conversion in Sunshine/Apollo.
        uint16_t rotation = LI_ROT_UNKNOWN;
        uint8_t tilt = LI_TILT_UNKNOWN;
        if ((pen.penMask & PEN_MASK_TILT_X) && (pen.penMask & PEN_MASK_TILT_Y)) {
            double a = std::tan(pen.tiltX * k_Pi / 180.0);
            double b = std::tan(pen.tiltY * k_Pi / 180.0);
            double tiltDeg = std::atan(std::sqrt(a * a + b * b)) * 180.0 / k_Pi;
            double azimuth = std::atan2(-a, b) * 180.0 / k_Pi;
            if (azimuth < 0) {
                azimuth += 360.0;
            }
            tilt = (uint8_t)qMin(90L, std::lround(tiltDeg));
            rotation = (uint16_t)(std::lround(azimuth) % 360);
        }

        LiSendPenEvent(eventType, toolType, penButtons, m_LastPenX, m_LastPenY, pressureOrDistance,
                       0.0f, 0.0f, rotation, tilt);
        s_Stats.sent++;
        if (flags & POINTER_FLAG_INCONTACT) {
            s_Stats.contactSamples++;
            if (i > 0) {
                s_Stats.sharedPressure++;  // an older history sample paired with the latest raw pressure
            }
        }
    }
    s_Stats.messages++;
    logPenStats((WacomRawReader*)m_WacomRaw, wt);

    return true;
}

bool SdlInputHandler::handleNativePenMouseButton(unsigned int msg, uintptr_t wParam)
{
    // Mouse input synthesized for a pen carries the MI_WP_SIGNATURE in the extra info;
    // bit 7 set means touch rather than pen. Right clicks are left alone: they come from
    // barrel + tap, which the host already derives from the pen's barrel flag.
    LPARAM extraInfo = GetMessageExtraInfo();
    if ((extraInfo & 0xFFFFFF00) != 0xFF515700 || (extraInfo & 0x80)) {
        return false;
    }
    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) || !isCaptureActive()) {
        return false;
    }

    // With raw Wacom reports the side buttons already travel as pen buttons; drop the local
    // driver's synthesized click so the host doesn't get the press twice
    auto raw = (WacomRawReader*)m_WacomRaw;
    if (raw != nullptr && raw->fresh()) {
        return true;
    }

    int button;
    switch (msg) {
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        button = BUTTON_MIDDLE;
        break;
    default:
        button = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? BUTTON_X1 : BUTTON_X2;
        break;
    }

    bool down = (msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN);
    LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
    return true;
}

#else

void SdlInputHandler::installNativePenHook() {}
void SdlInputHandler::removeNativePenHook() {}
bool SdlInputHandler::handleNativePenMessage(void*, unsigned int, uintptr_t) { return false; }
bool SdlInputHandler::handleNativePenMouseButton(unsigned int, uintptr_t) { return false; }

#endif
