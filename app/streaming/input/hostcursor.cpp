// The host's pointer drawn here as this PC's own pointer (Apollo with vwacom).
//
// Without it the pointer is part of the host's picture, one network round trip behind the hand
// (and the pen). With it, the host sends the pointer's shape and whether it is shown when they
// change, leaves the pointer out of its picture, and Windows draws the shape here as the
// ordinary pointer, moved by this PC's mouse and pen at once. What apps draw into their own
// window (a brush outline) stays in the picture, as with any streaming service.
//
// The messages come on the control stream (moonlight-common-c with scripts/patch-common-c.py):
// 0x7C01 a piece of a shape, 0x7C02 shown or not, and 0x7C03 from us: draw it here (1) or not (0).

#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/streamutils.h"

#include <QtGlobal>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef Q_OS_WIN32
#include <windows.h>

namespace {

constexpr uint16_t k_CursorShapeType = 0x7C01;
constexpr uint16_t k_CursorStateType = 0x7C02;
constexpr uint16_t k_CursorModeType = 0x7C03;

constexpr uint8_t k_KindMonochrome = 1;
constexpr uint8_t k_KindColor = 2;
constexpr uint8_t k_KindMaskedColor = 4;

constexpr uint8_t k_FlagHere = 0x01;   // shown on this screen
constexpr uint8_t k_FlagShown = 0x02;  // shown on the desktop (not here: on another screen)

constexpr int k_MaxSize = 512;
constexpr uint32_t k_MaxData = 4 * k_MaxSize * k_MaxSize;
constexpr int k_MaxCursorSize = 256;

// Moving from another screen's window into this one, the host's pointer takes a round trip to
// arrive on this screen: meanwhile the pointer stays as it was instead of blinking out
constexpr uint64_t k_EnterGraceMs = 500;

struct Shape {
    uint32_t serial = 0;
    uint8_t kind = 0;
    int width = 0, height = 0, pitch = 0, hotX = 0, hotY = 0;
    QByteArray data;
};

// Filled on the control stream's thread, taken on the main thread
std::mutex s_Mutex;
Shape s_Assembly;
uint32_t s_AssemblyReceived = 0;
bool s_AssemblyActive = false;
Shape s_PendingShape;
bool s_HavePendingShape = false;
uint8_t s_PendingFlags = 0;
bool s_HavePendingState = false;
bool s_EventQueued = false;
std::atomic<bool> s_Pending { false };

uint16_t le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void resetMessages()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Assembly = Shape();
    s_AssemblyReceived = 0;
    s_AssemblyActive = false;
    s_PendingShape = Shape();
    s_HavePendingShape = false;
    s_HavePendingState = false;
    s_EventQueued = false;
    s_Pending = false;
}

// The picture's place in the window (physical pixels) and its scale
double videoRect(HWND hwnd, int streamWidth, int streamHeight, RECT& video)
{
    RECT client;
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0 || streamWidth <= 0 || streamHeight <= 0) {
        video = { 0, 0, 0, 0 };
        return 1.0;
    }
    SDL_Rect src = { 0, 0, streamWidth, streamHeight };
    SDL_Rect dst = { 0, 0, (int)client.right, (int)client.bottom };
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    video = { dst.x, dst.y, dst.x + dst.w, dst.y + dst.h };
    double scale = (double)dst.w / streamWidth;
    // About the picture's own size: the shape as it is, pixel for pixel
    return qAbs(scale - 1.0) < 0.06 ? 1.0 : scale;
}

void setMaskBit(std::vector<uint8_t>& bits, int pitch, int x, int y)
{
    bits[(size_t)y * pitch + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

// A Windows cursor from the host's shape, scaled like the picture. Monochrome and masked color
// shapes keep their inverted pixels (the text cursor stays visible on any background).
HCURSOR buildCursor(uint8_t kind, int w, int h, int pitch, int hotX, int hotY, const QByteArray& data, double scale)
{
    int tw = qBound(1, (int)std::lround(w * scale), k_MaxCursorSize);
    int th = qBound(1, (int)std::lround(h * scale), k_MaxCursorSize);
    double sx = (double)w / tw, sy = (double)h / th;  // source pixels per cursor pixel
    int maskPitch = ((tw + 15) / 16) * 2;             // monochrome bitmap rows are WORD aligned
    const uint8_t* src = (const uint8_t*)data.constData();

    auto srcX = [&](int x) { return qMin(w - 1, (int)((x + 0.5) * sx)); };
    auto srcY = [&](int y) { return qMin(h - 1, (int)((y + 0.5) * sy)); };

    ICONINFO info = {};
    info.fIcon = FALSE;
    info.xHotspot = (DWORD)qBound(0, (int)std::lround(hotX / sx), tw - 1);
    info.yHotspot = (DWORD)qBound(0, (int)std::lround(hotY / sy), th - 1);

    if (kind == k_KindMonochrome) {
        // The AND mask's rows, then the XOR mask's: one bitmap twice as high, no color bitmap
        std::vector<uint8_t> bits((size_t)maskPitch * th * 2, 0);
        for (int plane = 0; plane < 2; plane++) {
            for (int y = 0; y < th; y++) {
                const uint8_t* row = src + (size_t)(plane * h + srcY(y)) * pitch;
                for (int x = 0; x < tw; x++) {
                    int s = srcX(x);
                    if (row[s >> 3] & (0x80 >> (s & 7))) {
                        setMaskBit(bits, maskPitch, x, plane * th + y);
                    }
                }
            }
        }
        info.hbmMask = CreateBitmap(tw, th * 2, 1, 1, bits.data());
        info.hbmColor = nullptr;
    }
    else {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = tw;
        bmi.bmiHeader.biHeight = -th;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* colorBits = nullptr;
        info.hbmColor = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &colorBits, nullptr, 0);
        if (info.hbmColor == nullptr || colorBits == nullptr) {
            if (info.hbmColor != nullptr) {
                DeleteObject(info.hbmColor);
            }
            return nullptr;
        }

        std::vector<uint8_t> mask((size_t)maskPitch * th, 0);
        uint8_t* out = (uint8_t*)colorBits;
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++, out += 4) {
                uint8_t b, g, r, a;
                if (kind == k_KindColor && scale < 1.0) {
                    // Smaller: the average of the pixels it covers (premultiplied, so the
                    // transparent ones don't darken the edges)
                    int x0 = qMin(w - 1, (int)(x * sx)), x1 = qBound(x0 + 1, (int)std::ceil((x + 1) * sx), w);
                    int y0 = qMin(h - 1, (int)(y * sy)), y1 = qBound(y0 + 1, (int)std::ceil((y + 1) * sy), h);
                    uint32_t sb = 0, sg = 0, sr = 0, sa = 0, n = 0;
                    for (int yy = y0; yy < y1; yy++) {
                        const uint8_t* p = src + (size_t)yy * pitch + (size_t)x0 * 4;
                        for (int xx = x0; xx < x1; xx++, p += 4, n++) {
                            sb += p[0] * p[3];
                            sg += p[1] * p[3];
                            sr += p[2] * p[3];
                            sa += p[3];
                        }
                    }
                    a = (uint8_t)(sa / n);
                    b = sa ? (uint8_t)qMin<uint32_t>(255, sb / sa) : 0;
                    g = sa ? (uint8_t)qMin<uint32_t>(255, sg / sa) : 0;
                    r = sa ? (uint8_t)qMin<uint32_t>(255, sr / sa) : 0;
                }
                else {
                    const uint8_t* p = src + (size_t)srcY(y) * pitch + (size_t)srcX(x) * 4;
                    b = p[0];
                    g = p[1];
                    r = p[2];
                    a = p[3];
                }

                out[0] = b;
                out[1] = g;
                out[2] = r;
                if (kind == k_KindColor) {
                    out[3] = a;
                    if (a == 0) {
                        setMaskBit(mask, maskPitch, x, y);
                    }
                }
                else {
                    // Masked color: alpha 0xFF XORs the color with the screen (mask bit set),
                    // 0 replaces it; Windows reads the mask only when the color has no alpha
                    out[3] = 0;
                    if (a == 0xFF) {
                        setMaskBit(mask, maskPitch, x, y);
                    }
                }
            }
        }
        info.hbmMask = CreateBitmap(tw, th, 1, 1, mask.data());
    }

    HCURSOR cursor = info.hbmMask != nullptr ? (HCURSOR)CreateIconIndirect(&info) : nullptr;
    if (info.hbmMask != nullptr) {
        DeleteObject(info.hbmMask);
    }
    if (info.hbmColor != nullptr) {
        DeleteObject(info.hbmColor);
    }
    return cursor;
}

}

bool SdlInputHandler::queueHostMessage(uint16_t type, const uint8_t* data, uint32_t length)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    if (type == k_CursorShapeType) {
        // serial, kind, reserved, width, height, pitch, hot x, hot y, total size, offset; then data
        if (length < 24) {
            return false;
        }
        uint32_t serial = le32(data);
        uint8_t kind = data[4];
        int width = le16(data + 6), height = le16(data + 8), pitch = le16(data + 10);
        int hotX = (int16_t)le16(data + 12), hotY = (int16_t)le16(data + 14);
        uint32_t total = le32(data + 16), offset = le32(data + 20);
        uint32_t piece = length - 24;

        if (offset == 0) {
            uint32_t rows = kind == k_KindMonochrome ? 2u * height : (uint32_t)height;
            uint32_t rowBytes = kind == k_KindMonochrome ? (uint32_t)(width + 7) / 8 : 4u * width;
            bool valid = (kind == k_KindMonochrome || kind == k_KindColor || kind == k_KindMaskedColor) &&
                         width > 0 && height > 0 && width <= k_MaxSize && height <= k_MaxSize &&
                         (uint32_t)pitch >= rowBytes && total == rows * (uint32_t)pitch && total <= 2 * k_MaxData;
            if (!valid) {
                s_AssemblyActive = false;
                return false;
            }
            s_Assembly.serial = serial;
            s_Assembly.kind = kind;
            s_Assembly.width = width;
            s_Assembly.height = height;
            s_Assembly.pitch = pitch;
            s_Assembly.hotX = hotX;
            s_Assembly.hotY = hotY;
            s_Assembly.data = QByteArray((int)total, '\0');
            s_AssemblyReceived = 0;
            s_AssemblyActive = true;
        }

        // The pieces come in order (a reliable channel): anything else drops this shape
        if (!s_AssemblyActive || serial != s_Assembly.serial || offset != s_AssemblyReceived ||
                piece > (uint32_t)s_Assembly.data.size() - offset) {
            s_AssemblyActive = false;
            return false;
        }
        memcpy(s_Assembly.data.data() + offset, data + 24, piece);
        s_AssemblyReceived += piece;
        if (s_AssemblyReceived < (uint32_t)s_Assembly.data.size()) {
            return false;
        }
        s_PendingShape = std::move(s_Assembly);
        s_Assembly = Shape();
        s_HavePendingShape = true;
        s_AssemblyActive = false;
    }
    else if (type == k_CursorStateType) {
        // flags, reserved, serial, x, y
        if (length < 8) {
            return false;
        }
        s_PendingFlags = data[0];
        s_HavePendingState = true;
    }
    else {
        return false;
    }

    s_Pending = true;
    if (s_EventQueued) {
        return false;
    }
    s_EventQueued = true;
    return true;
}

void SdlInputHandler::applyHostCursor()
{
    Shape shape;
    bool haveShape, haveState;
    uint8_t flags;
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_EventQueued = false;
        s_Pending = false;
        haveShape = s_HavePendingShape;
        if (haveShape) {
            shape = std::move(s_PendingShape);
            s_PendingShape = Shape();
            s_HavePendingShape = false;
        }
        haveState = s_HavePendingState;
        flags = s_PendingFlags;
        s_HavePendingState = false;
    }

    // Late ones, after we asked the host to stop
    if (!m_HostCursorRequested || (!haveShape && !haveState)) {
        return;
    }

    if (haveShape) {
        m_HostCursorSerial = shape.serial;
        m_HostCursorKind = shape.kind;
        m_HostCursorWidth = shape.width;
        m_HostCursorHeight = shape.height;
        m_HostCursorPitch = shape.pitch;
        m_HostCursorHotX = shape.hotX;
        m_HostCursorHotY = shape.hotY;
        m_HostCursorShape = std::move(shape.data);
        m_HostCursorScale = 0;  // build it again (the old one goes once the new one is shown)
    }
    if (haveState) {
        m_HostCursorFlags = flags;
    }

    if (!m_HostCursorActive) {
        m_HostCursorActive = true;
        m_HostCursorSupported = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host pointer: drawn here, the host leaves it out of its picture (%s)",
                    (m_HostCursorFlags & k_FlagHere) ? "on this screen" : "not on this screen");
    }

    showHostCursorNow();
}

void SdlInputHandler::setHostCursorWanted(bool wanted)
{
    if (wanted && !m_HostCursorRequested) {
        resetMessages();
    }
    m_HostCursorWanted = wanted;
    updateHostCursorMode();
    showHostCursorNow();
}

void SdlInputHandler::updateHostCursorMode()
{
    bool want = m_HostCursorWanted && m_AbsoluteMouseMode;
    if (want == m_HostCursorRequested) {
        return;
    }

    uint8_t mode[4] = { (uint8_t)(want ? 1 : 0), 0, 0, 0 };
    if (LiSendHostMessage(k_CursorModeType, mode, sizeof(mode)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Host pointer: couldn't ask the host");
        return;
    }
    m_HostCursorRequested = want;

    if (!want) {
        // The host draws it into its picture again: the pointer here as before (hidden)
        bool wasActive = m_HostCursorActive;
        m_HostCursorActive = false;
        if (wasActive) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Host pointer: back in the host's picture");
            if (isCaptureActive() && m_MouseWasInVideoRegion) {
                SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
            }
        }
    }
}

bool SdlInputHandler::hostCursorInUse() const
{
    return m_HostCursorActive && m_HostCursorRequested && m_AbsoluteMouseMode;
}

bool SdlInputHandler::hostCursorVisibleNow() const
{
    if (m_HostCursorFlags & k_FlagHere) {
        return true;
    }
    // On another screen and on its way here
    return (m_HostCursorFlags & k_FlagShown) && SDL_GetTicks64() - m_PointerEnteredMs < k_EnterGraceMs;
}

int SdlInputHandler::capturedCursorState() const
{
    if (!hostCursorInUse()) {
        return m_MouseCursorCapturedVisibilityState;
    }
    return hostCursorVisibleNow() ? SDL_ENABLE : SDL_DISABLE;
}

void* SdlInputHandler::hostCursorForScale(double scale)
{
    if (m_HostCursorShape.isEmpty()) {
        // Shown before any shape came: the arrow
        return LoadCursor(nullptr, IDC_ARROW);
    }
    if (m_HostCursorHandle != nullptr && m_HostCursorScale == scale) {
        return m_HostCursorHandle;
    }

    HCURSOR cursor = buildCursor(m_HostCursorKind, m_HostCursorWidth, m_HostCursorHeight, m_HostCursorPitch,
                                 m_HostCursorHotX, m_HostCursorHotY, m_HostCursorShape, scale);
    if (cursor == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Host pointer: couldn't make a cursor of the shape (%dx%d, kind %d): %lu",
                    m_HostCursorWidth, m_HostCursorHeight, m_HostCursorKind, GetLastError());
        return m_HostCursorHandle != nullptr ? m_HostCursorHandle : (void*)LoadCursor(nullptr, IDC_ARROW);
    }

    // The old one goes after the new one is the pointer
    HCURSOR old = (HCURSOR)m_HostCursorHandle;
    m_HostCursorHandle = cursor;
    m_HostCursorScale = scale;
    if (old != nullptr) {
        if (GetCursor() == old) {
            SetCursor(cursor);
        }
        DestroyCursor(old);
    }
    return cursor;
}

void SdlInputHandler::showHostCursorNow()
{
    HWND hwnd = (HWND)m_NativePenHwnd;
    if (!hostCursorInUse() || hwnd == nullptr || !isCaptureActive()) {
        return;
    }

    POINT pt;
    if (!GetCursorPos(&pt)) {
        return;
    }
    HWND over = WindowFromPoint(pt);
    if (GetCapture() != hwnd && (over == nullptr || GetAncestor(over, GA_ROOT) != hwnd)) {
        return;  // the pointer is over something else
    }
    RECT video;
    double scale = videoRect(hwnd, m_StreamWidth, m_StreamHeight, video);
    POINT local = pt;
    if (!ScreenToClient(hwnd, &local) || !PtInRect(&video, local)) {
        return;  // outside the picture: this PC's arrow (mouse.cpp)
    }

    bool visible = hostCursorVisibleNow();
    if (SDL_ShowCursor(SDL_QUERY) != (visible ? SDL_ENABLE : SDL_DISABLE)) {
        SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
    }
    SetCursor(visible ? (HCURSOR)hostCursorForScale(scale) : nullptr);
}

bool SdlInputHandler::handleSetCursor(void* hwndPtr, intptr_t lParam)
{
    // A wake-up event that went missing
    if (s_Pending) {
        applyHostCursor();
    }

    if (!hostCursorInUse() || LOWORD(lParam) != HTCLIENT || !isCaptureActive()) {
        return false;
    }

    // The mouse or the pen just came over the window
    uint64_t now = SDL_GetTicks64();
    if (now - m_LastSetCursorMs > 200) {
        m_PointerEnteredMs = now;
    }
    m_LastSetCursorMs = now;

    HWND hwnd = (HWND)hwndPtr;
    POINT pt;
    RECT video;
    double scale = videoRect(hwnd, m_StreamWidth, m_StreamHeight, video);
    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !PtInRect(&video, pt)) {
        return false;  // outside the picture: SDL's own (the arrow)
    }

    SetCursor(hostCursorVisibleNow() ? (HCURSOR)hostCursorForScale(scale) : nullptr);
    return true;
}

void SdlInputHandler::destroyHostCursor()
{
    if (m_HostCursorHandle != nullptr) {
        if (GetCursor() == (HCURSOR)m_HostCursorHandle) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        DestroyCursor((HCURSOR)m_HostCursorHandle);
        m_HostCursorHandle = nullptr;
    }
    m_HostCursorScale = 0;
}

#else

bool SdlInputHandler::queueHostMessage(uint16_t, const uint8_t*, uint32_t) { return false; }
void SdlInputHandler::applyHostCursor() {}
void SdlInputHandler::setHostCursorWanted(bool) {}
void SdlInputHandler::updateHostCursorMode() {}
bool SdlInputHandler::hostCursorInUse() const { return false; }
bool SdlInputHandler::hostCursorVisibleNow() const { return false; }
int SdlInputHandler::capturedCursorState() const { return m_MouseCursorCapturedVisibilityState; }
void* SdlInputHandler::hostCursorForScale(double) { return nullptr; }
void SdlInputHandler::showHostCursorNow() {}
bool SdlInputHandler::handleSetCursor(void*, intptr_t) { return false; }
void SdlInputHandler::destroyHostCursor() {}

#endif
