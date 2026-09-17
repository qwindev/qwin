#include "systemapi.h"

#include "screendevice.h"
#include "windowfocus.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QSysInfo>
#include <QVariantMap>
#include <QDebug>

#include <windows.h>

namespace {

quint64 toQuad(const FILETIME &ft)
{
    return (quint64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

} // namespace

SystemApi::SystemApi(const QString &pluginsDir, QObject *parent)
    : QObject(parent)
    , m_pluginsDir(QDir(pluginsDir).absolutePath())
{
    updateStats(); // primes the CPU-time baseline; first real value is a second out

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        updateStats();
        emit statsChanged();
    });
    m_timer.start();

    // A monitor plugged/unplugged mid-session, or the primary reassigned,
    // must reach the bar's Instantiator so it can add/remove/reorder a bar.
    connect(qApp, &QGuiApplication::screenAdded, this, &SystemApi::screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, &SystemApi::screensChanged);
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, &SystemApi::screensChanged);
}

QString SystemApi::hostname() const
{
    return QSysInfo::machineHostName();
}

QVariantList SystemApi::screens() const
{
    QVariantList out;
    QScreen *primary = QGuiApplication::primaryScreen();
    if (primary) {
        QVariantMap entry;
        entry.insert(QStringLiteral("device"), screendevice::nameOf(primary));
        entry.insert(QStringLiteral("name"), primary->name());
        entry.insert(QStringLiteral("primary"), true);
        out.append(entry);
    }
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen == primary)
            continue;
        QVariantMap entry;
        entry.insert(QStringLiteral("device"), screendevice::nameOf(screen));
        entry.insert(QStringLiteral("name"), screen->name());
        entry.insert(QStringLiteral("primary"), false);
        out.append(entry);
    }
    return out;
}

void SystemApi::updateStats()
{
    FILETIME idleFt, kernelFt, userFt;
    if (GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
        const quint64 idle = toQuad(idleFt);
        const quint64 kernel = toQuad(kernelFt); // includes idle time
        const quint64 user = toQuad(userFt);

        const quint64 total = (kernel - m_lastKernel) + (user - m_lastUser);
        const quint64 idleDelta = idle - m_lastIdle;
        if (m_lastKernel != 0 && total > 0)
            m_cpuUsage = qBound(0.0, 100.0 * double(total - idleDelta) / double(total), 100.0);

        m_lastIdle = idle;
        m_lastKernel = kernel;
        m_lastUser = user;
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        m_memoryUsagePercent = double(mem.dwMemoryLoad);
}

QString SystemApi::readTextFile(const QString &path) const
{
    // Sandbox: inside the plugins directory only. Canonicalizing BEFORE the
    // containment check is what rejects "sub/../../secret.txt".
    QFileInfo info(path);
    if (info.isRelative())
        info.setFile(m_pluginsDir + QLatin1Char('/') + path);

    const QString canonical = info.canonicalFilePath(); // empty if nonexistent
    const QString root = QFileInfo(m_pluginsDir).canonicalFilePath();
    const bool inside = !canonical.isEmpty() && !root.isEmpty()
                        && canonical.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
    if (!inside) {
        qWarning().noquote() << "readTextFile rejected:" << path;
        return QString();
    }

    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning().noquote() << "readTextFile cannot open:" << canonical;
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

void SystemApi::openStartMenu()
{
    // Ctrl+Esc, not a synthesized VK_LWIN: the Win key is a modifier, so a
    // press that never lands its release - a remapper or a fullscreen game
    // swallowing it - leaves every following keystroke a Win chord. Ctrl+Esc
    // is the shell's own equivalent and carries no such tail.
    //
    // No rememberFocus()/restoreFocus() bracket: the bar is WS_EX_NOACTIVATE,
    // so the click that got here never moved the foreground, and the Start
    // menu takes it from whatever held it - exactly as pressing the key does.
    constexpr UINT count = 4;
    INPUT keys[count] = {};
    for (INPUT &key : keys)
        key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_CONTROL;
    keys[1].ki.wVk = VK_ESCAPE;
    keys[2].ki.wVk = VK_ESCAPE;
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_CONTROL;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;

    // Degrade, never fail: UIPI blocks injection into a higher-integrity
    // foreground window, and there is nothing to do about it but say so.
    if (SendInput(count, keys, sizeof(INPUT)) != count)
        qWarning() << "openStartMenu: SendInput failed," << GetLastError();
}

void SystemApi::rememberFocus()
{
    const HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    // Remembering one of our own windows would make restoreFocus() a no-op.
    m_savedFocus = (fg && pid != GetCurrentProcessId()) ? fg : nullptr;
}

void SystemApi::restoreFocus()
{
    // Hand back only when whatever holds the keyboard cannot use it. Another
    // process means the user clicked straight into it; one of our own windows
    // may still be a real target (the launcher). Only the panel's own
    // WS_EX_NOACTIVATE says "nobody here can type" - and Windows parks the
    // foreground there anyway when a popup it owns hides.
    const HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        const bool ours = pid == GetCurrentProcessId();
        const bool noActivate = GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_NOACTIVATE;
        if (!ours || !noActivate)
            return;
    }

    HWND saved = static_cast<HWND>(m_savedFocus);
    m_savedFocus = nullptr;
    // Covers the saved window having been closed, minimized or moved to
    // another desktop meanwhile; topmost is the better answer for all of those.
    if (saved && windowfocus::isFocusableAppWindow(saved))
        SetForegroundWindow(saved);
    else
        windowfocus::focusTopmostWindow();
}
