#include "systemapi.h"

#include "windowfocus.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
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
    updateStats();   // primes the CPU-time baseline; first real value is a second out
    updateBattery(); // so the first frame shows real state, not the defaults

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        updateStats();
        emit statsChanged();
        updateBattery(); // emits only when something actually moved
    });
    m_timer.start();
}

QString SystemApi::hostname() const
{
    return QSysInfo::machineHostName();
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

void SystemApi::updateBattery()
{
    SYSTEM_POWER_STATUS status;
    if (!GetSystemPowerStatus(&status))
        return;

    // BatteryFlag bit 7 is "no system battery" - what desktops and some VMs
    // report instead of leaving the other fields meaningful.
    const bool available = (status.BatteryFlag & 128) == 0;
    // 255 = unknown.
    const int percent = (status.BatteryLifePercent == 255) ? -1 : int(status.BatteryLifePercent);
    const bool charging = (status.BatteryFlag & 8) != 0;
    const bool ac = status.ACLineStatus == 1;
    // (DWORD)-1 = unknown, distinct from 0.
    const int timeLeft = (status.BatteryLifeTime == DWORD(-1)) ? -1 : int(status.BatteryLifeTime);
    // The battery-saver bit; older SDKs named this field Reserved1.
    const bool saver = status.SystemStatusFlag == 1;

    const bool changed = available != m_batteryAvailable
                       || percent != m_batteryPercent
                       || charging != m_batteryCharging
                       || ac != m_acPower
                       || timeLeft != m_batteryTimeLeft
                       || saver != m_batterySaver;

    m_batteryAvailable = available;
    m_batteryPercent = percent;
    m_batteryCharging = charging;
    m_acPower = ac;
    m_batteryTimeLeft = timeLeft;
    m_batterySaver = saver;

    // Battery rarely changes, unlike CPU/RAM: on statsChanged it would re-run
    // every unrelated binding once a second.
    if (changed)
        emit batteryChanged();
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
