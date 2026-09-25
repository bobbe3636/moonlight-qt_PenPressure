#include "input.h"

#include <QtGlobal>
#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32

#include <SDL_syswm.h>
#include <commctrl.h>

#include <cmath>
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
        if (((SdlInputHandler*)refData)->handleNativePenMessage(hwnd, msg, wParam)) {
            return 0;
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
}

bool SdlInputHandler::handleNativePenMessage(void* hwndPtr, unsigned int msg, uintptr_t wParam)
{
    HWND hwnd = (HWND)hwndPtr;
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
    std::vector<POINTER_PEN_INFO> samples(count);
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
    }

    return true;
}

#else

void SdlInputHandler::installNativePenHook() {}
void SdlInputHandler::removeNativePenHook() {}
bool SdlInputHandler::handleNativePenMessage(void*, unsigned int, uintptr_t) { return false; }

#endif
