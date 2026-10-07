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
#include <map>
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
    std::atomic<bool> seenBarrel2 { false };  // the upper switch shows here (not with every pen and driver setting)

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
            if (report[1] & k_FlagBarrel2) {
                seenBarrel2 = true;
            }
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

// Any tablet's own reports (Wacom, Huion, XP-Pen, other standard HID pens), through Raw Input.
//
// Windows keeps a HID pen's collection to itself (another reader gets a sharing violation) but
// hands copies of its reports to Raw Input. Each report is decoded with the device's own report
// descriptor (its preparsed data), so no model needs to be known: the pressure over the tablet's
// full range (0..its logical maximum, e.g. 8191 or 16383, where Windows Ink stops at 1024) and
// the barrel buttons, hovering too. Wacom's driver delivers them on its Digitizer collection
// (usage 0x01), other tablets on their Pen collection (0x02). A hidden window on its own thread
// receives them in the background too: every screen's window needs the latest state, focused or
// not. Position, tilt, eraser and timing still come from WM_POINTER.

struct HidPenReader
{
    std::thread thread;
    HANDLE ready = nullptr;
    std::atomic<DWORD> threadId { 0 };
    std::atomic<float> pressure { 0.0f };  // 0..1 of the tablet's range
    std::atomic<uint8_t> buttons { 0 };
    std::atomic<bool> hasBarrel { false };  // the tablet reports its barrel buttons
    std::atomic<bool> seenBarrel2 { false };  // ...the second one too (Wacom's driver turns it into a click)
    std::atomic<uint64_t> lastReportMs { 0 };
    std::atomic<bool> seenReport { false };

    static constexpr uint8_t k_Barrel1 = 0x01;
    static constexpr uint8_t k_Barrel2 = 0x02;

    struct Device
    {
        std::vector<uint8_t> preparsed;
        LONG pressureMax = 0;
        bool hasBarrel = false;
    };
    std::map<HANDLE, Device> devices;  // reader thread only

    // Only trust state that is current
    bool fresh() const
    {
        return seenReport && GetTickCount64() - lastReportMs < 100;
    }

    Device& device(HANDLE h)
    {
        auto known = devices.find(h);
        if (known != devices.end()) {
            return known->second;
        }
        Device& d = devices[h];
        UINT size = 0;
        if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size) == 0 && size > 0) {
            d.preparsed.resize(size);
            if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.preparsed.data(), &size) == (UINT)-1) {
                d.preparsed.clear();
            }
        }
        if (!d.preparsed.empty()) {
            auto ppd = (PHIDP_PREPARSED_DATA)d.preparsed.data();
            HIDP_VALUE_CAPS values[8];
            USHORT count = 8;
            if (HidP_GetSpecificValueCaps(HidP_Input, 0x0D, 0, 0x30, values, &count, ppd) == HIDP_STATUS_SUCCESS) {
                for (USHORT i = 0; i < count; i++) {
                    LONG max = values[i].LogicalMax;
                    if (max <= values[i].LogicalMin && values[i].BitSize < 32) {
                        max = (LONG)((1UL << values[i].BitSize) - 1);  // an unsigned maximum read as negative
                    }
                    d.pressureMax = qMax(d.pressureMax, max);
                }
            }
            HIDP_BUTTON_CAPS barrels[8];
            count = 8;
            d.hasBarrel = HidP_GetSpecificButtonCaps(HidP_Input, 0x0D, 0, 0x44, barrels, &count, ppd) == HIDP_STATUS_SUCCESS &&
                          count > 0;
        }
        return d;
    }

    void handle(HANDLE h, BYTE* report, DWORD size)
    {
        Device& d = device(h);
        if (d.preparsed.empty() || d.pressureMax <= 0) {
            return;
        }
        auto ppd = (PHIDP_PREPARSED_DATA)d.preparsed.data();
        ULONG value = 0;
        if (HidP_GetUsageValue(HidP_Input, 0x0D, 0, 0x30, &value, ppd, (PCHAR)report, size) != HIDP_STATUS_SUCCESS) {
            return;  // another of the device's reports
        }
        uint8_t pressed = 0;
        USAGE usages[16];
        ULONG count = 16;
        if (HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &count, ppd, (PCHAR)report, size) == HIDP_STATUS_SUCCESS) {
            for (ULONG i = 0; i < count; i++) {
                if (usages[i] == 0x44) {
                    pressed |= k_Barrel1;  // Barrel Switch
                }
                else if (usages[i] == 0x5A || usages[i] == 0x43) {
                    pressed |= k_Barrel2;  // Secondary Barrel Switch (Huion: Secondary Tip Switch)
                }
            }
        }
        pressure = qMin((float)value / d.pressureMax, 1.0f);
        buttons = pressed;
        if (pressed & k_Barrel2) {
            seenBarrel2 = true;
        }
        hasBarrel = d.hasBarrel;
        lastReportMs = GetTickCount64();
        if (!seenReport) {
            seenReport = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using the tablet's own reports (Raw Input): pressure 0..%ld%s",
                        d.pressureMax, d.hasBarrel ? ", barrel buttons" : "");
        }
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_INPUT) {
            auto self = (HidPenReader*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            UINT size = 0;
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            static thread_local std::vector<uint8_t> buf;
            buf.resize(size);
            if (self != nullptr && size > 0 &&
                    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
                auto input = (RAWINPUT*)buf.data();
                if (input->header.dwType == RIM_TYPEHID) {
                    for (DWORD i = 0; i < input->data.hid.dwCount; i++) {
                        self->handle(input->header.hDevice, input->data.hid.bRawData + i * input->data.hid.dwSizeHid,
                                     input->data.hid.dwSizeHid);
                    }
                }
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void run()
    {
        // A message queue first, so the quit message can't be lost
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        threadId = GetCurrentThreadId();

        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"MoonlightRawPen";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        bool registered = false;
        RAWINPUTDEVICE rid[2] = {};
        if (hwnd != nullptr) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
            rid[0] = { 0x0D, 0x02, RIDEV_INPUTSINK, hwnd };  // Pen
            rid[1] = { 0x0D, 0x01, RIDEV_INPUTSINK, hwnd };  // Digitizer (Wacom's driver)
            registered = RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
            if (!registered) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Raw Input for pen tablets unavailable: %lu", GetLastError());
            }
        }
        SetEvent(ready);

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (registered) {
            for (auto& r : rid) {
                r.dwFlags = RIDEV_REMOVE;
                r.hwndTarget = nullptr;
            }
            RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
        }
        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
        }
    }

    static HidPenReader* open()
    {
        auto reader = new HidPenReader();
        reader->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        reader->thread = std::thread(&HidPenReader::run, reader);
        WaitForSingleObject(reader->ready, 2000);
        return reader;
    }

    ~HidPenReader()
    {
        if (thread.joinable()) {
            while (threadId == 0) {
                Sleep(1);
            }
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
            thread.join();
        }
        if (ready != nullptr) {
            CloseHandle(ready);
        }
    }
};

// Wintab.
//
// The tablet driver's own pen API (Wacom's; most other makers ship one too) reports the
// tablet's full pressure range, tilt, buttons and eraser for any model, where Windows Ink stops
// at 1024 levels. It's all or nothing: while a window has a Wintab context open, Wacom's driver
// sends it plain mouse input instead of Windows Ink pen messages. So in Wintab mode the whole
// pen comes from Wintab packets: position from the cursor (which the driver still moves along
// its screen mapping), the rest from the packet; and the driver's mouse input is dropped while
// the pen is over the stream, so the host doesn't get it twice. Wintab32.dll is loaded at
// runtime. Wintab sends packets to one context at a time, the top of its overlap order: the
// stream window the cursor enters brings its context to the top (several screens' windows).

namespace wintab {
    DECLARE_HANDLE(HCTX);
    typedef DWORD WTPKT;
    typedef DWORD FIX32;

    constexpr UINT WTI_DEFCONTEXT = 3;
    constexpr UINT WTI_DEVICES = 100;
    constexpr UINT DVC_NPRESSURE = 15;
    constexpr UINT CXO_MESSAGES = 0x0004;
    constexpr UINT TPS_PROXIMITY = 0x0001;
    constexpr UINT TPS_INVERT = 0x0010;
    constexpr WTPKT PK_STATUS = 0x0002;
    constexpr WTPKT PK_TIME = 0x0004;
    constexpr WTPKT PK_CURSOR = 0x0020;
    constexpr WTPKT PK_BUTTONS = 0x0040;
    constexpr WTPKT PK_NORMAL_PRESSURE = 0x0400;
    constexpr WTPKT PK_ORIENTATION = 0x1000;
    constexpr UINT WT_DEFBASE = 0x7FF0;
    constexpr UINT WT_PACKET = WT_DEFBASE;
    constexpr UINT WT_PROXIMITY = WT_DEFBASE + 5;

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

    struct ORIENTATION {
        int orAzimuth;   // tenths of a degree, clockwise from the tablet's top
        int orAltitude;  // tenths of a degree above the tablet (900 = upright)
        int orTwist;
    };

    // Fields in the order of their PK_ bits
    struct PACKET {
        UINT status;          // PK_STATUS
        DWORD time;           // PK_TIME
        UINT cursor;          // PK_CURSOR
        DWORD buttons;        // PK_BUTTONS
        UINT normalPressure;  // PK_NORMAL_PRESSURE
        ORIENTATION orientation;  // PK_ORIENTATION
    };
}

struct WintabPen
{
    HMODULE dll = nullptr;
    wintab::HCTX ctx = nullptr;
    float maxPressure = 1023.0f;
    std::atomic<uint32_t> packetCount { 0 };  // for the pen statistics log line

    // Pen state, UI thread only
    bool inProximity = false;
    bool inside = false;    // the cursor is over this stream window
    bool contact = false;
    uint64_t lastPacketMs = 0;
    bool loggedFirst = false;

    UINT (WINAPI* fnInfo)(UINT, UINT, LPVOID) = nullptr;
    wintab::HCTX (WINAPI* fnOpen)(HWND, wintab::LOGCONTEXTW*, BOOL) = nullptr;
    BOOL (WINAPI* fnClose)(wintab::HCTX) = nullptr;
    BOOL (WINAPI* fnPacket)(wintab::HCTX, UINT, LPVOID) = nullptr;
    BOOL (WINAPI* fnOverlap)(wintab::HCTX, BOOL) = nullptr;

    static WintabPen* open(HWND hwnd)
    {
        HMODULE dll = LoadLibraryW(L"Wintab32.dll");
        if (dll == nullptr) {
            return nullptr;
        }

        auto pen = new WintabPen();
        pen->dll = dll;
        pen->fnInfo = (decltype(fnInfo))GetProcAddress(dll, "WTInfoW");
        pen->fnOpen = (decltype(fnOpen))GetProcAddress(dll, "WTOpenW");
        pen->fnClose = (decltype(fnClose))GetProcAddress(dll, "WTClose");
        pen->fnPacket = (decltype(fnPacket))GetProcAddress(dll, "WTPacket");
        pen->fnOverlap = (decltype(fnOverlap))GetProcAddress(dll, "WTOverlap");
        if (!pen->fnInfo || !pen->fnOpen || !pen->fnClose || !pen->fnPacket || !pen->fnOverlap ||
                pen->fnInfo(0, 0, nullptr) == 0) {
            // No Wintab service running (e.g. the driver's Wintab is off)
            delete pen;
            return nullptr;
        }

        wintab::AXIS pressureAxis = {};
        if (pen->fnInfo(wintab::WTI_DEVICES, wintab::DVC_NPRESSURE, &pressureAxis) && pressureAxis.axMax > 0) {
            pen->maxPressure = (float)pressureAxis.axMax;
        }

        // A digitizing context (the driver keeps moving the cursor along its own mapping) that
        // posts WT_PACKET and WT_PROXIMITY to the stream window
        wintab::LOGCONTEXTW lc = {};
        if (!pen->fnInfo(wintab::WTI_DEFCONTEXT, 0, &lc)) {
            delete pen;
            return nullptr;
        }
        wcscpy_s(lc.lcName, L"Moonlight pen");
        lc.lcOptions |= wintab::CXO_MESSAGES;
        lc.lcMsgBase = wintab::WT_DEFBASE;
        lc.lcPktData = wintab::PK_STATUS | wintab::PK_TIME | wintab::PK_CURSOR | wintab::PK_BUTTONS |
                       wintab::PK_NORMAL_PRESSURE | wintab::PK_ORIENTATION;
        lc.lcPktMode = 0;  // absolute
        lc.lcMoveMask = lc.lcPktData;
        lc.lcBtnDnMask = lc.lcBtnUpMask = 0xFFFFFFFF;
        pen->ctx = pen->fnOpen(hwnd, &lc, TRUE);
        if (pen->ctx == nullptr) {
            delete pen;
            return nullptr;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Wintab pen: %.0f pressure levels", pen->maxPressure + 1);
        return pen;
    }

    // The pen (or the mouse) is over this window: its context gets the packets from now on
    void bringToTop()
    {
        fnOverlap(ctx, TRUE);
    }

    // While the pen is near the tablet and has been talking to us, its mouse input is its own
    bool ownsMouse() const
    {
        return inProximity && GetTickCount64() - lastPacketMs < 2000;
    }

    ~WintabPen()
    {
        if (ctx != nullptr) {
            fnClose(ctx);
        }
        FreeLibrary(dll);
    }
};

// Windows' X/Y tilt convention (positive X = toward the right, positive Y = toward the user)
// to the protocol's tilt from vertical plus azimuth: the exact inverse of the host-side
// conversion in Sunshine/Apollo
static void tiltXYToProtocol(double tiltXDeg, double tiltYDeg, uint16_t& rotation, uint8_t& tilt)
{
    double a = std::tan(tiltXDeg * k_Pi / 180.0);
    double b = std::tan(tiltYDeg * k_Pi / 180.0);
    double tiltDeg = std::atan(std::sqrt(a * a + b * b)) * 180.0 / k_Pi;
    double azimuth = std::atan2(-a, b) * 180.0 / k_Pi;
    if (azimuth < 0) {
        azimuth += 360.0;
    }
    tilt = (uint8_t)qMin(90L, std::lround(tiltDeg));
    rotation = (uint16_t)(std::lround(azimuth) % 360);
}

// Pen statistics, logged every 2 s while the pen is active ("Pen stats: ..."), to see how
// evenly and how often samples leave this PC. Only touched on the UI thread.
static struct {
    uint64_t windowStart = 0;
    uint64_t lastMessage = 0;
    uint32_t maxGapMs = 0;
    uint32_t messages = 0, sent = 0, contactSamples = 0, sharedPressure = 0;
    uint32_t rawAtStart = 0, wintabAtStart = 0;
} s_Stats;

static void logPenStats(WacomRawReader* raw, WintabPen* wt)
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

// Drags across the screens' windows. While a button is held, Windows keeps sending the mouse to
// the window where the drag started, and that one only knows its own screen: the host's cursor
// stopped at that screen's edge. So the starting window tells the window under the pointer,
// which moves the host's cursor onto its screen (every screen's window drives the same desktop,
// so the dragged window or file follows). Stream windows are marked with a window property.
static constexpr const wchar_t* k_StreamWindowProp = L"MoonlightStreamWindow";

static UINT foreignPointerMessage()
{
    static UINT message = RegisterWindowMessageW(L"MoonlightForeignPointer");
    return message;
}

bool SdlInputHandler::forwardPointerToOtherScreen()
{
    if (!m_MultiScreen || m_NativePenHwnd == nullptr) {
        return false;
    }
    POINT pt;
    if (!GetCursorPos(&pt)) {
        return false;
    }
    HWND over = WindowFromPoint(pt);
    HWND root = over != nullptr ? GetAncestor(over, GA_ROOT) : nullptr;
    if (root == nullptr || root == (HWND)m_NativePenHwnd || GetPropW(root, k_StreamWindowProp) == nullptr) {
        return false;
    }
    PostMessageW(root, foreignPointerMessage(), 0, MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y));
    return true;
}

void SdlInputHandler::handleForeignPointer(int screenX, int screenY)
{
    HWND hwnd = (HWND)m_NativePenHwnd;
    if (hwnd == nullptr || m_Window == nullptr) {
        return;
    }
    POINT pt = { screenX, screenY };
    RECT client;
    if (!ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) {
        return;
    }

    // Physical pixels to window coordinates, then into the video region (as mouse.cpp)
    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    int x = pt.x * windowWidth / client.right;
    int y = pt.y * windowHeight / client.bottom;

    SDL_Rect src = { 0, 0, m_StreamWidth, m_StreamHeight };
    SDL_Rect dst = { 0, 0, windowWidth, windowHeight };
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    x = qMin(qMax(x - dst.x, 0), dst.w);
    y = qMin(qMax(y - dst.y, 0), dst.h);
    LiSendMousePositionEvent((short)x, (short)y, dst.w, dst.h);
}

static LRESULT CALLBACK penSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR refData)
{
    if (msg == foreignPointerMessage()) {
        ((SdlInputHandler*)refData)->handleForeignPointer((short)LOWORD(lParam), (short)HIWORD(lParam));
        return 0;
    }

    switch (msg) {
    case WM_POINTERENTER:
    case WM_POINTERLEAVE:
    case WM_POINTERDOWN:
    case WM_POINTERUP:
    case WM_POINTERUPDATE:
    case WM_POINTERCAPTURECHANGED:
        if (((SdlInputHandler*)refData)->handleNativePenMessage(hwnd, msg, wParam)) {
            return 0;
        }
        break;
    case wintab::WT_PACKET:
    case wintab::WT_PROXIMITY:
        if (((SdlInputHandler*)refData)->handleWintabMessage(hwnd, msg, wParam, lParam)) {
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDBLCLK:
        if (((SdlInputHandler*)refData)->handleWintabMouse(msg)) {
            return 0;
        }
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (((SdlInputHandler*)refData)->handleWintabMouse(msg)) {
            return (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP) ? TRUE : 0;
        }
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
        SetPropW(info.info.win.window, k_StreamWindowProp, (HANDLE)1);

        // Pen input setting: 2 = Wintab (the whole pen from the tablet driver's Wintab); 0
        // (automatic) and 1 = Windows Ink, with the raw Wacom reports where we know them
        if (m_PenInputMode == 2) {
            m_Wintab = WintabPen::open(info.info.win.window);
            if (m_Wintab == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Wintab isn't available: using Windows Ink for the pen");
            }
        }
        if (m_Wintab == nullptr) {
            m_WacomRaw = WacomRawReader::open();
            m_HidPen = HidPenReader::open();
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
            RemovePropW((HWND)m_NativePenHwnd, k_StreamWindowProp);
        }
        m_NativePenHwnd = nullptr;
    }

    delete (WacomRawReader*)m_WacomRaw;
    m_WacomRaw = nullptr;
    delete (HidPenReader*)m_HidPen;
    m_HidPen = nullptr;
    delete (WintabPen*)m_Wintab;
    m_Wintab = nullptr;
}

bool SdlInputHandler::handleNativePenMessage(void* hwndPtr, unsigned int msg, uintptr_t wParam)
{
    HWND hwnd = (HWND)hwndPtr;
    UINT32 pointerId = GET_POINTERID_WPARAM(wParam);

    POINTER_INPUT_TYPE pointerType;
    if (!GetPointerType(pointerId, &pointerType) || pointerType != PT_PEN) {
        return false;
    }

    // Whether the pen is in range, for the driver's unmarked middle click (handleNativePenMouseButton)
    m_LastPenMessageMs = GetTickCount64();
    m_PenInRange = msg != WM_POINTERLEAVE && msg != WM_POINTERCAPTURECHANGED;

    // Without host support, let Windows turn the pen into mouse input as before
    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
        return false;
    }

    // The raw Wacom reader opens again if it isn't running: a pen display's tablet goes away while
    // its screen sleeps, so at a stream's start it may not have been there (no full pressure range,
    // no side buttons for the whole stream), or it went away since. On the pen's use, every 2 s at most.
    if (m_Wintab == nullptr && (m_WacomRaw == nullptr || ((WacomRawReader*)m_WacomRaw)->exited)) {
        // Backing off to once a minute: with another tablet (no Wacom) there's nothing to find
        uint64_t now = GetTickCount64();
        if (now - m_LastRawReaderAttemptMs >= m_RawReaderRetryMs) {
            m_LastRawReaderAttemptMs = now;
            delete (WacomRawReader*)m_WacomRaw;
            m_WacomRaw = WacomRawReader::open();
            if (m_WacomRaw != nullptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Raw Wacom reader opened (again)");
                m_RawReaderRetryMs = 2000;
            }
            else {
                m_RawReaderRetryMs = qMin<uint32_t>(m_RawReaderRetryMs * 2, 60000);
            }
        }
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

        // Raw Wacom state, when current, gives both barrel buttons (hovering too) and 8192 levels
        auto raw = (WacomRawReader*)m_WacomRaw;
        if (raw != nullptr && raw->fresh()) {
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
        // Any other tablet's own reports: its full pressure range, and its barrel buttons
        else if (auto hidPen = (HidPenReader*)m_HidPen; hidPen != nullptr && hidPen->fresh()) {
            if (hidPen->hasBarrel) {
                // Added to what Windows Ink says, never instead of it: a driver that turns a side
                // button into a click (Wacom's, per its button settings) doesn't report it here
                uint8_t pressed = hidPen->buttons;
                penButtons |= ((pressed & HidPenReader::k_Barrel1) ? LI_PEN_BUTTON_PRIMARY : 0) |
                              ((pressed & HidPenReader::k_Barrel2) ? LI_PEN_BUTTON_SECONDARY : 0);
            }
            if (flags & POINTER_FLAG_INCONTACT) {
                float hidPressure = hidPen->pressure;
                if (hidPressure > 0.0f) {
                    pressureOrDistance = hidPressure;
                }
            }
        }

        // The upper switch as the driver's middle click (handleNativePenMouseButton)
        if (eventType == LI_TOUCH_EVENT_HOVER_LEAVE) {
            m_DriverMiddleHeld = false;
        }
        if (m_DriverMiddleHeld) {
            penButtons |= LI_PEN_BUTTON_SECONDARY;
        }
        m_LastPenButtons = penButtons;
        m_LastPenTool = toolType;

        // Windows gives X/Y tilt; the protocol wants tilt from vertical plus azimuth
        uint16_t rotation = LI_ROT_UNKNOWN;
        uint8_t tilt = LI_TILT_UNKNOWN;
        if ((pen.penMask & PEN_MASK_TILT_X) && (pen.penMask & PEN_MASK_TILT_Y)) {
            tiltXYToProtocol(pen.tiltX, pen.tiltY, rotation, tilt);
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
    logPenStats((WacomRawReader*)m_WacomRaw, (WintabPen*)m_Wintab);

    return true;
}

bool SdlInputHandler::handleNativePenMouseButton(unsigned int msg, uintptr_t wParam)
{
    // Mouse input synthesized for a pen carries the MI_WP_SIGNATURE in the extra info;
    // bit 7 set means touch rather than pen. Right clicks are left alone: they come from
    // barrel + tap, which the host already derives from the pen's barrel flag.
    LPARAM extraInfo = GetMessageExtraInfo();
    bool fromPen = (extraInfo & 0xFFFFFF00) == 0xFF515700 && !(extraInfo & 0x80);

    // Wacom's driver injects the middle click of its upper-switch setting as a plain mouse click,
    // not marked as the pen's (its left/right clicks are). With the pen in range (seen in the last
    // quarter second), or that click's press taken as the pen's, it's the pen's.
    if (!fromPen && (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) &&
            (m_DriverMiddleHeld || (m_PenInRange && GetTickCount64() - m_LastPenMessageMs < 250))) {
        fromPen = true;
    }
    if (!fromPen) {
        return false;
    }
    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) || !isCaptureActive()) {
        return false;
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
    uint8_t bit = (uint8_t)(1 << (button & 7));

    // The upper side switch, which Wacom's driver turns into a middle click, goes to the host as
    // the pen's second barrel button: its virtual Wacom tablet has it as the upper switch, and
    // apps see a stylus button, as with the tablet plugged in there. As a mouse middle click in
    // the middle of tablet input, Krita started panning and never saw it end.
    if (button == BUTTON_MIDDLE) {
        if (m_PenClicksForwarded & bit) {
            m_PenClicksForwarded &= ~bit;  // from before: let it go
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
        }
        if (m_DriverMiddleHeld != down) {
            m_DriverMiddleHeld = down;
            uint8_t buttons = (m_LastPenButtons & ~LI_PEN_BUTTON_SECONDARY) | (down ? LI_PEN_BUTTON_SECONDARY : 0);
            m_LastPenButtons = buttons;
            LiSendPenEvent(LI_TOUCH_EVENT_BUTTON_ONLY, m_LastPenTool, buttons, m_LastPenX, m_LastPenY, 0.0f,
                           0.0f, 0.0f, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
        }
        return true;
    }

    // A click whose press went to the host always gets its release, whatever the raw readers
    // say by then: deciding the release separately could swallow it (a middle button held
    // forever on the host: endless panning)
    if (!down && (m_PenClicksForwarded & bit)) {
        m_PenClicksForwarded &= ~bit;
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
        return true;
    }

    // With raw Wacom reports the side buttons already travel as pen buttons; drop the local
    // driver's synthesized click so the host doesn't get the press twice. Only once the raw
    // reports have shown the upper switch: with some pens and driver settings they never do,
    // and the click is then the only way the button gets to the host.
    auto raw = (WacomRawReader*)m_WacomRaw;
    if (raw != nullptr && raw->fresh() && raw->seenBarrel2) {
        return true;
    }
    // Likewise once the tablet's own reports have shown the second barrel button (a driver that
    // turns it into a click instead, like Wacom's, never reports it: its click stays)
    auto hidPen = (HidPenReader*)m_HidPen;
    if (hidPen != nullptr && hidPen->fresh() && hidPen->seenBarrel2) {
        return true;
    }

    if (down) {
        m_PenClicksForwarded |= bit;
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, button);
    }
    // (a release whose press didn't go to the host stays here)
    return true;
}

bool SdlInputHandler::handleWintabMessage(void* hwndPtr, unsigned int msg, uintptr_t wParam, intptr_t lParam)
{
    auto wt = (WintabPen*)m_Wintab;
    if (wt == nullptr) {
        return false;
    }
    HWND hwnd = (HWND)hwndPtr;
    bool hostPen = (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0;

    if (msg == wintab::WT_PROXIMITY) {
        bool entering = LOWORD(lParam) != 0;
        if (!entering && hostPen) {
            if (wt->contact) {
                LiSendPenEvent(LI_TOUCH_EVENT_UP, LI_TOOL_TYPE_PEN, 0, m_LastPenX, m_LastPenY, 0.0f,
                               0.0f, 0.0f, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
            }
            if (wt->inside) {
                LiSendPenEvent(LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOOL_TYPE_PEN, 0, m_LastPenX, m_LastPenY, 0.0f,
                               0.0f, 0.0f, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
            }
        }
        if (!entering) {
            wt->contact = wt->inside = false;
        }
        wt->inProximity = entering;
        return true;
    }

    // WT_PACKET
    wintab::PACKET packet;
    if (!wt->fnPacket(wt->ctx, (UINT)wParam, &packet)) {
        return true;
    }
    wt->packetCount++;
    wt->lastPacketMs = GetTickCount64();
    wt->inProximity = !(packet.status & wintab::TPS_PROXIMITY);
    if (!hostPen) {
        return true;  // the driver's mouse input goes to the host as usual
    }
    if (!wt->loggedFirst) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using Wintab for the pen");
        wt->loggedFirst = true;
    }

    // Position: the cursor, which the driver moves along its screen mapping (physical pixels:
    // Moonlight is per-monitor DPI aware)
    POINT cursor;
    POINT clientOrigin = {};
    RECT clientRect;
    GetCursorPos(&cursor);
    ClientToScreen(hwnd, &clientOrigin);
    GetClientRect(hwnd, &clientRect);
    if (clientRect.right <= 0 || clientRect.bottom <= 0) {
        return true;
    }
    bool inside = cursor.x >= clientOrigin.x && cursor.x < clientOrigin.x + clientRect.right &&
                  cursor.y >= clientOrigin.y && cursor.y < clientOrigin.y + clientRect.bottom;

    // A stroke that leaves the window keeps going (clamped to the edge), like pointer capture
    if (!inside && !wt->contact) {
        if (wt->inside) {
            LiSendPenEvent(LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOOL_TYPE_PEN, 0, m_LastPenX, m_LastPenY, 0.0f,
                           0.0f, 0.0f, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
            wt->inside = false;
        }
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

    float windowX = (float)(cursor.x - clientOrigin.x) * windowWidth / clientRect.right;
    float windowY = (float)(cursor.y - clientOrigin.y) * windowHeight / clientRect.bottom;
    float vidrelx = qMin(qMax(windowX, (float)dst.x), (float)(dst.x + dst.w)) - dst.x;
    float vidrely = qMin(qMax(windowY, (float)dst.y), (float)(dst.y + dst.h)) - dst.y;
    m_LastPenX = vidrelx / dst.w;
    m_LastPenY = vidrely / dst.h;

    bool contact = wt->inProximity && (packet.normalPressure > 0 || (packet.buttons & 0x1));
    uint8_t eventType;
    if (contact) {
        eventType = wt->contact ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_DOWN;
    }
    else if (wt->contact) {
        eventType = LI_TOUCH_EVENT_UP;
    }
    else if (wt->inProximity) {
        eventType = LI_TOUCH_EVENT_HOVER;
    }
    else {
        eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
    }

    uint8_t toolType = (packet.status & wintab::TPS_INVERT) ? LI_TOOL_TYPE_ERASER : LI_TOOL_TYPE_PEN;
    // Pen buttons: 0 tip, 1 lower side switch, 2 upper side switch
    uint8_t penButtons = ((packet.buttons & 0x2) ? LI_PEN_BUTTON_PRIMARY : 0) |
                         ((packet.buttons & 0x4) ? LI_PEN_BUTTON_SECONDARY : 0);

    // Contact: pressure 0..1 at the tablet's full resolution; hover distance isn't known (0.0)
    float pressureOrDistance = contact ? qMin(packet.normalPressure / wt->maxPressure, 1.0f) : 0.0f;

    // Tilt: altitude/azimuth to Windows' X/Y tilt (as Qt's Wintab support does), then as Ink
    uint16_t rotation = LI_ROT_UNKNOWN;
    uint8_t tilt = LI_TILT_UNKNOWN;
    double altitude = std::abs(packet.orientation.orAltitude) / 10.0;
    if (altitude > 0.0 && altitude < 90.0) {
        double azimuth = packet.orientation.orAzimuth / 10.0 * k_Pi / 180.0;
        double tanAltitude = std::tan(altitude * k_Pi / 180.0);
        double tiltX = std::atan(std::sin(azimuth) / tanAltitude) * 180.0 / k_Pi;
        double tiltY = -std::atan(std::cos(azimuth) / tanAltitude) * 180.0 / k_Pi;
        tiltXYToProtocol(tiltX, tiltY, rotation, tilt);
    }
    else if (altitude >= 90.0) {
        rotation = 0;
        tilt = 0;
    }

    LiSendPenEvent(eventType, toolType, penButtons, m_LastPenX, m_LastPenY, pressureOrDistance,
                   0.0f, 0.0f, rotation, tilt);
    wt->contact = contact;
    wt->inside = inside || contact;

    s_Stats.sent++;
    if (contact) {
        s_Stats.contactSamples++;
    }
    s_Stats.messages++;
    logPenStats((WacomRawReader*)m_WacomRaw, wt);
    return true;
}

bool SdlInputHandler::handleWintabMouse(unsigned int msg)
{
    auto wt = (WintabPen*)m_Wintab;
    if (wt == nullptr) {
        return false;
    }

    // The cursor over this window: packets come here (at most every 100 ms, it's a driver call)
    static uint64_t lastOverlapMs = 0;
    if (msg == WM_MOUSEMOVE && GetTickCount64() - lastOverlapMs > 100) {
        wt->bringToTop();
        lastOverlapMs = GetTickCount64();
    }

    // The driver's mouse input for the pen: the pen already sends it to the host
    return (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) && wt->ownsMouse();
}

#else

void SdlInputHandler::installNativePenHook() {}
void SdlInputHandler::removeNativePenHook() {}
bool SdlInputHandler::forwardPointerToOtherScreen() { return false; }
void SdlInputHandler::handleForeignPointer(int, int) {}
bool SdlInputHandler::handleNativePenMessage(void*, unsigned int, uintptr_t) { return false; }
bool SdlInputHandler::handleNativePenMouseButton(unsigned int, uintptr_t) { return false; }
bool SdlInputHandler::handleWintabMessage(void*, unsigned int, uintptr_t, intptr_t) { return false; }
bool SdlInputHandler::handleWintabMouse(unsigned int) { return false; }

#endif
