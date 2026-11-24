#include "clipboardsync.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"

#include <SDL_syswm.h>

#include <QReadLocker>
#include <QSslCertificate>
#include <QString>
#include <QThreadPool>

#include <atomic>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <commctrl.h>
#endif

// The host instance of this window's screen, for requests from a worker thread after the session
// may have ended (they then just go nowhere)
struct ClipboardSync::Shared {
    QString address;
    uint16_t httpPort = 0;
    uint16_t httpsPort = 0;
    QSslCertificate serverCert;
    std::atomic<bool> denied { false };  // the host refused (permissions): stop asking
    std::atomic<bool> busyPull { false };
};

#ifdef Q_OS_WIN32

namespace {

// Larger text stays where it is (a paste of it over the network would stall)
constexpr int k_MaxBytes = 4 * 1024 * 1024;

constexpr const wchar_t* k_StreamWindowProp = L"MoonlightStreamWindow";  // pen.cpp marks stream windows
constexpr UINT_PTR k_ClipboardSubclassId = 0x434C4950;  // 'CLIP'

// The clipboard's sequence number when its content was last on both sides (pushed or pulled),
// shared by the screens' windows (one process each)
volatile LONG* sharedSyncedSequence()
{
    static volatile LONG* s_Value = []() -> volatile LONG* {
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, L"Local\\MoonlightClipboardSync");
        if (mapping == nullptr) {
            return nullptr;
        }
        // Kept open for the process' life
        return (volatile LONG*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 64);
    }();
    return s_Value;
}

LONG s_LocalSynced = 0;  // without the shared value

DWORD syncedSequence()
{
    volatile LONG* shared = sharedSyncedSequence();
    return (DWORD)(shared != nullptr ? InterlockedCompareExchange(shared, 0, 0) : s_LocalSynced);
}

void setSyncedSequence(DWORD sequence)
{
    volatile LONG* shared = sharedSyncedSequence();
    if (shared != nullptr) {
        InterlockedExchange(shared, (LONG)sequence);
    }
    else {
        s_LocalSynced = (LONG)sequence;
    }
}

bool openClipboard(HWND owner)
{
    // Another app may have it open for a moment
    for (int attempt = 0; attempt < 5; attempt++) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

// Marked by password managers and the like: not for other programs (or other PCs)
bool clipboardIsPrivate()
{
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT viewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    static const UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if ((exclude && IsClipboardFormatAvailable(exclude)) || (viewerIgnore && IsClipboardFormatAvailable(viewerIgnore))) {
        return true;
    }
    if (history && IsClipboardFormatAvailable(history)) {
        // Needs the clipboard open (the caller has it)
        if (HANDLE data = GetClipboardData(history)) {
            bool off = false;
            if (auto value = (const DWORD*)GlobalLock(data)) {
                off = GlobalSize(data) >= sizeof(DWORD) && *value == 0;
                GlobalUnlock(data);
            }
            return off;
        }
    }
    return false;
}

// This PC's clipboard text (false: none, or private)
bool readText(QString& text, bool& isPrivate)
{
    isPrivate = false;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !openClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    isPrivate = clipboardIsPrivate();
    if (!isPrivate) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (auto chars = (const wchar_t*)GlobalLock(data)) {
                size_t max = GlobalSize(data) / sizeof(wchar_t);
                text = QString::fromWCharArray(chars, (int)wcsnlen(chars, max));
                GlobalUnlock(data);
                ok = true;
            }
        }
    }
    CloseClipboard();
    return ok;
}

bool writeText(const QString& text, HWND fallbackOwner)
{
    std::wstring wide = text.toStdWString();
    for (HWND owner : { (HWND)nullptr, fallbackOwner }) {
        if (!openClipboard(owner)) {
            continue;
        }
        bool ok = false;
        if (EmptyClipboard()) {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
            if (memory != nullptr) {
                if (auto chars = (wchar_t*)GlobalLock(memory)) {
                    memcpy(chars, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(memory);
                    ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                }
                if (!ok) {
                    GlobalFree(memory);
                }
            }
        }
        CloseClipboard();
        if (ok) {
            return true;
        }
        // Without an owner window SetClipboardData may refuse: once more with the stream window
        if (fallbackOwner == nullptr) {
            break;
        }
    }
    return false;
}

void logFailure(ClipboardSync::Shared* shared, const char* what, int status, const QString& error)
{
    // 401: the device isn't allowed to (Apollo's per-device permissions). (403, no stream yet,
    // is only logged.)
    if (status == 401 || status == QNetworkReply::AuthenticationRequiredError) {
        if (!shared->denied.exchange(true)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard: the host doesn't let this device %s the clipboard (Apollo: Clients, this device's permissions)", what);
        }
        return;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: couldn't %s the host's clipboard: %s", what, qPrintable(error));
}

LRESULT CALLBACK clipboardSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    if (msg == WM_CLIPBOARDUPDATE) {
        ((ClipboardSync*)refData)->clipboardChanged();
    }
    else if (msg == WM_NCDESTROY) {
        RemoveClipboardFormatListener(hwnd);
        RemoveWindowSubclass(hwnd, clipboardSubclassProc, subclassId);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

}

ClipboardSync::ClipboardSync(NvComputer* computer)
    : m_Shared(std::make_shared<Shared>())
{
    QReadLocker lock(&computer->lock);
    m_Shared->address = computer->activeAddress.address();
    m_Shared->httpPort = computer->activeAddress.port() ? computer->activeAddress.port() : DEFAULT_HTTP_PORT;
    m_Shared->httpsPort = computer->activeHttpsPort ? computer->activeHttpsPort : DEFAULT_HTTPS_PORT;
    m_Shared->serverCert = computer->serverCert;
}

ClipboardSync::~ClipboardSync()
{
    HWND hwnd = (HWND)m_Hwnd;
    if (hwnd != nullptr && IsWindow(hwnd)) {
        RemoveClipboardFormatListener(hwnd);
        RemoveWindowSubclass(hwnd, clipboardSubclassProc, k_ClipboardSubclassId);
    }
}

void ClipboardSync::setWindow(SDL_Window* window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return;
    }
    HWND hwnd = info.info.win.window;
    if (SetWindowSubclass(hwnd, clipboardSubclassProc, k_ClipboardSubclassId, (DWORD_PTR)this)) {
        m_Hwnd = hwnd;
        AddClipboardFormatListener(hwnd);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: text is shared with the host");

    // Already focused: what was copied here before the stream started goes now
    if (GetForegroundWindow() == hwnd) {
        pushToHost();
    }
}

void ClipboardSync::focusGained()
{
    pushToHost();
}

void ClipboardSync::clipboardChanged()
{
    // Copied here by another app while this window has the focus (our own writes are synced)
    if (m_Hwnd != nullptr && GetForegroundWindow() == (HWND)m_Hwnd) {
        pushToHost();
    }
}

void ClipboardSync::pushToHost()
{
    if (m_Shared->denied) {
        return;
    }
    DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == 0 || sequence == syncedSequence()) {
        return;  // nothing new here
    }
    setSyncedSequence(sequence);  // looked at: not again, whether it goes or not

    QString text;
    bool isPrivate = false;
    if (!readText(text, isPrivate)) {
        if (isPrivate) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: marked private here (a password manager?), not sent");
        }
        return;
    }
    QByteArray utf8 = text.toUtf8();
    if (utf8.isEmpty()) {
        return;
    }
    if (utf8.size() > k_MaxBytes) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: %d KB of text is too much to send, not sent", (int)(utf8.size() / 1024));
        return;
    }

    std::shared_ptr<Shared> shared = m_Shared;
    QThreadPool::globalInstance()->start([shared, utf8]() {
        NvHTTP http(NvAddress(shared->address, shared->httpPort), shared->httpsPort, shared->serverCert, true);
        try {
            http.setClipboardText(utf8);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: this PC's text went to the host (%d characters)", (int)QString::fromUtf8(utf8).size());
        } catch (const GfeHttpResponseException& e) {
            logFailure(shared.get(), "set", e.getStatusCode(), e.toQString());
        } catch (const QtNetworkReplyException& e) {
            logFailure(shared.get(), "set", e.getError(), e.toQString());
        }
    });
}

void ClipboardSync::focusLost()
{
    if (m_Shared->denied) {
        return;
    }
    // Another screen's window: still the host
    HWND foreground = GetForegroundWindow();
    if (foreground != nullptr && GetPropW(foreground, k_StreamWindowProp) != nullptr) {
        return;
    }
    if (m_Shared->busyPull.exchange(true)) {
        return;
    }

    DWORD sequenceAtLoss = GetClipboardSequenceNumber();
    std::shared_ptr<Shared> shared = m_Shared;
    HWND owner = (HWND)m_Hwnd;
    QThreadPool::globalInstance()->start([shared, sequenceAtLoss, owner]() {
        QByteArray utf8;
        NvHTTP http(NvAddress(shared->address, shared->httpPort), shared->httpsPort, shared->serverCert, true);
        try {
            utf8 = http.getClipboardText();
        } catch (const GfeHttpResponseException& e) {
            logFailure(shared.get(), "read", e.getStatusCode(), e.toQString());
        } catch (const QtNetworkReplyException& e) {
            logFailure(shared.get(), "read", e.getError(), e.toQString());
        }
        shared->busyPull = false;
        if (utf8.isEmpty() || utf8.size() > k_MaxBytes) {
            return;
        }

        QString text = QString::fromUtf8(utf8);
        QString current;
        bool isPrivate = false;
        if (readText(current, isPrivate) && current == text) {
            // Already the same here
            setSyncedSequence(GetClipboardSequenceNumber());
            return;
        }
        if (GetClipboardSequenceNumber() != sequenceAtLoss) {
            return;  // copied here meanwhile: that stays
        }
        if (writeText(text, owner)) {
            setSyncedSequence(GetClipboardSequenceNumber());
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: the host's text is here now (%d characters)", (int)text.size());
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: couldn't put the host's text on this PC's clipboard: %lu", GetLastError());
        }
    });
}

#else

ClipboardSync::ClipboardSync(NvComputer*) : m_Shared(std::make_shared<Shared>()) {}
ClipboardSync::~ClipboardSync() {}
void ClipboardSync::setWindow(SDL_Window*) {}
void ClipboardSync::focusGained() {}
void ClipboardSync::focusLost() {}
void ClipboardSync::clipboardChanged() {}
void ClipboardSync::pushToHost() {}

#endif
