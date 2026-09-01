#include "foregroundwindow.h"

#include "windowfocus.h"

#include <QBuffer>
#include <QByteArray>
#include <QDebug>
#include <QFileInfo>
#include <QImage>

#include <windows.h>
#include <shellapi.h> // ExtractIconExW
#include <winver.h>   // GetFileVersionInfo*, VerQueryValueW

#include <string>

namespace {

constexpr int kIconCacheCap = 64;
constexpr int kIconTimeoutMs = 50; // a hung app must not stall the bar

ForegroundWindow *g_instance = nullptr; // exactly one ever exists; see main.cpp

// WINEVENT_OUTOFCONTEXT hooks arrive as ordinary messages pumped by the
// registering thread's own loop, unlike a COM/WinRT callback. The hooks are
// installed from the constructor, on Qt's main thread, so this runs there
// too and needs no marshalling before touching state or emitting.
void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                            LONG idChild, DWORD, DWORD)
{
    if (!g_instance)
        return;
    switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
        g_instance->onForegroundChanged();
        break;
    case EVENT_OBJECT_NAMECHANGE:
        // Unfiltered, this fires constantly for controls all over the
        // system. Only the foreground window's own title is worth reacting to.
        if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd == GetForegroundWindow())
            g_instance->onNameChanged();
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
        g_instance->onMinimizeStateChanged();
        break;
    default:
        break;
    }
}

// Denied for elevated/protected processes even with
// PROCESS_QUERY_LIMITED_INFORMATION, so empty is expected, not a failure.
QString processImagePath(DWORD pid)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return QString();
    wchar_t buffer[MAX_PATH];
    DWORD size = MAX_PATH;
    QString path;
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size))
        path = QString::fromWCharArray(buffer, int(size));
    CloseHandle(proc);
    return path;
}

// FileDescription from the exe's version resource - "Google Chrome" rather
// than "chrome.exe". Plenty of small tools ship no version resource, hence
// the empty return and the caller's fallback to the file name.
QString fileDescription(const QString &exePath)
{
    const std::wstring path = exePath.toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
        return QString();

    QByteArray data(int(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, data.data()))
        return QString();

    struct LangCodePage { WORD language; WORD codePage; };
    LangCodePage *translations = nullptr;
    UINT translationsLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                         reinterpret_cast<void **>(&translations), &translationsLen)
        || translationsLen < sizeof(LangCodePage))
        return QString();

    // Strings are keyed by language+codepage: ask for the block the file
    // itself declares, as a fixed guess misses non-English-US builds.
    const QString subBlock = QStringLiteral("\\StringFileInfo\\%1%2\\FileDescription")
        .arg(uint(translations[0].language), 4, 16, QLatin1Char('0'))
        .arg(uint(translations[0].codePage), 4, 16, QLatin1Char('0'));

    wchar_t *description = nullptr;
    UINT descriptionLen = 0;
    if (!VerQueryValueW(data.data(), reinterpret_cast<const wchar_t *>(subBlock.utf16()),
                         reinterpret_cast<void **>(&description), &descriptionLen)
        || descriptionLen == 0)
        return QString();

    // descriptionLen includes the trailing NUL.
    return QString::fromWCharArray(description, int(descriptionLen) - 1).trimmed();
}

} // namespace

ForegroundWindow::ForegroundWindow(QObject *parent)
    : QObject(parent)
{
    g_instance = this;

    m_hookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                        nullptr, winEventProc, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Adjacent event ids, so one hook covers both. Insurance for the
    // last-window-minimized case, which usually also fires FOREGROUND.
    m_hookMinimize = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, winEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    m_hookNameChange = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                                        nullptr, winEventProc, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!m_hookForeground) {
        qWarning() << "ActiveWindow: SetWinEventHook(EVENT_SYSTEM_FOREGROUND) failed"
                   << "- focused-window tracking disabled";
    }

    refresh(); // prime from whatever already has focus
}

ForegroundWindow::~ForegroundWindow()
{
    if (m_hookForeground)
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hookForeground));
    if (m_hookMinimize)
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hookMinimize));
    if (m_hookNameChange)
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hookNameChange));
    g_instance = nullptr;
}

void ForegroundWindow::onForegroundChanged()
{
    refresh();
}

void ForegroundWindow::onNameChanged()
{
    // Cheap despite firing on every keystroke in a browser's address bar:
    // with the hwnd unchanged, refresh() skips process/icon re-resolution.
    refresh();
}

void ForegroundWindow::onMinimizeStateChanged()
{
    refresh();
}

void ForegroundWindow::refresh()
{
    HWND fg = GetForegroundWindow();
    if (!fg || !windowfocus::isFocusableAppWindow(fg)) {
        if (m_available) {
            m_available = false;
            m_title.clear();
            m_processName.clear();
            m_appName.clear();
            m_iconSource.clear();
            m_hwnd = nullptr;
            emit changed();
        }
        return;
    }

    QString title;
    const int titleLen = GetWindowTextLengthW(fg);
    if (titleLen > 0) {
        std::wstring buffer(titleLen + 1, L'\0');
        const int copied = GetWindowTextW(fg, buffer.data(), titleLen + 1);
        title = QString::fromWCharArray(buffer.data(), copied);
    }

    QString processName = m_processName;
    QString appName = m_appName;
    QString iconSource = m_iconSource;

    if (fg != m_hwnd) {
        // New window: re-resolve process/app/icon. A title-only change skips
        // this - OpenProcess and version lookups per keystroke are wasted.
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        const QString exePath = processImagePath(pid);
        if (exePath.isEmpty()) {
            processName.clear();
            appName.clear();
            iconSource.clear();
        } else {
            const QFileInfo info(exePath);
            processName = info.fileName();
            const QString description = fileDescription(exePath);
            appName = description.isEmpty() ? info.completeBaseName() : description;
            iconSource = iconDataUrl(exePath, fg);
        }
    }

    const bool changedAny = !m_available || title != m_title || processName != m_processName
                          || appName != m_appName || iconSource != m_iconSource;

    m_available = true;
    m_title = title;
    m_processName = processName;
    m_appName = appName;
    m_iconSource = iconSource;
    m_hwnd = fg;

    if (changedAny)
        emit changed();
}

QString ForegroundWindow::iconDataUrl(const QString &exePath, void *hwndVoid)
{
    if (const auto it = m_iconCache.constFind(exePath); it != m_iconCache.constEnd())
        return it.value();

    const HWND hwnd = static_cast<HWND>(hwndVoid);
    HICON icon = nullptr;
    bool ownsIcon = false; // only an ExtractIconExW handle is ours to destroy

    static constexpr WPARAM kIconKinds[] = { ICON_SMALL2, ICON_SMALL, ICON_BIG };
    for (WPARAM which : kIconKinds) {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, which, 0, SMTO_ABORTIFHUNG,
                                 kIconTimeoutMs, &result) && result) {
            icon = reinterpret_cast<HICON>(result);
            break;
        }
    }
    if (!icon) {
        LONG_PTR classIcon = GetClassLongPtrW(hwnd, GCLP_HICONSM);
        if (!classIcon)
            classIcon = GetClassLongPtrW(hwnd, GCLP_HICON);
        if (classIcon)
            icon = reinterpret_cast<HICON>(classIcon);
    }
    if (!icon) {
        HICON extracted = nullptr;
        const std::wstring path = exePath.toStdWString();
        if (ExtractIconExW(path.c_str(), 0, nullptr, &extracted, 1) > 0 && extracted) {
            icon = extracted;
            ownsIcon = true;
        }
    }

    QString dataUrl;
    if (icon) {
        QImage image = QImage::fromHICON(icon);
        if (!image.isNull()) {
            if (image.width() > 32 || image.height() > 32)
                image = image.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            dataUrl = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        }
    }
    // WM_GETICON and the class icon are owned by the window or its class -
    // destroying those breaks the app's own icon. Only ExtractIconExW
    // returns a copy the caller must free.
    if (ownsIcon && icon)
        DestroyIcon(icon);

    if (m_iconCache.size() >= kIconCacheCap)
        m_iconCache.clear(); // simplest bound: reset rather than grow forever
    m_iconCache.insert(exePath, dataUrl);
    return dataUrl;
}
