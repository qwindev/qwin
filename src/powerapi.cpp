#include "powerapi.h"

#include <QDebug>

#include <windows.h>
#include <powrprof.h>

namespace {

// Every call below pairs this with EWX_FORCEIFHUNG, so one unresponsive
// process cannot silently swallow the action. EWX_FORCE is deliberately
// never used: it discards unsaved work in every running app.
constexpr DWORD kShutdownReason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER
                                 | SHTDN_REASON_FLAG_PLANNED;

} // namespace

PowerApi::PowerApi(QObject *parent)
    : QObject(parent)
{
    SYSTEM_POWER_CAPABILITIES caps = {};
    // SystemS4 reports hardware support and stays true after "powercfg
    // /hibernate off", which deletes hiberfil.sys and makes SetSuspendState
    // fail outright - HiberFilePresent is what gates it right now, so both
    // must hold. Read off the documented fields; not reproduced here.
    m_hibernateAvailable = GetPwrCapabilities(&caps) && caps.SystemS4 && caps.HiberFilePresent;
}

PowerApi::~PowerApi()
{
    // The flag outlives this object until cleared, so without this the host
    // could exit leaving the machine unable to sleep.
    if (m_keepAwake)
        SetThreadExecutionState(ES_CONTINUOUS);
}

void PowerApi::setKeepAwake(bool awake)
{
    if (awake == m_keepAwake)
        return;

    // ES_* flags are per CALLING thread, not process-wide, so every
    // arm/disarm must run on the main thread or the state latches on a
    // thread nothing ever clears it from.
    if (awake)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    else
        SetThreadExecutionState(ES_CONTINUOUS);

    m_keepAwake = awake;
    qInfo().noquote() << "Power: keepAwake" << (awake ? "armed" : "disarmed");
    emit keepAwakeChanged();
}

void PowerApi::lock()
{
    if (!LockWorkStation())
        qWarning().noquote() << "Power: LockWorkStation failed, error" << GetLastError();
}

void PowerApi::sleep()
{
    // No special case for caffeine: ES_SYSTEM_REQUIRED blocks the IDLE sleep
    // timer, not an explicit suspend like this one. Read off the
    // SetThreadExecutionState docs; not reproduced here.
    if (!SetSuspendState(FALSE, FALSE, FALSE))
        qWarning().noquote() << "Power: SetSuspendState(sleep) failed";
}

void PowerApi::hibernate()
{
    if (!m_hibernateAvailable) {
        qWarning().noquote() << "Power: hibernate requested but hibernateAvailable is false";
        return;
    }
    if (!SetSuspendState(TRUE, FALSE, FALSE))
        qWarning().noquote() << "Power: SetSuspendState(hibernate) failed";
}

void PowerApi::signOut()
{
    // No privilege needed, unlike restart/shutdown below.
    if (!ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, kShutdownReason))
        qWarning().noquote() << "Power: ExitWindowsEx(EWX_LOGOFF) failed, error" << GetLastError();
}

void PowerApi::restart()
{
    if (!enableShutdownPrivilege())
        qWarning().noquote() << "Power: could not enable SE_SHUTDOWN_NAME, restart may fail";
    if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, kShutdownReason))
        qWarning().noquote() << "Power: ExitWindowsEx(EWX_REBOOT) failed, error" << GetLastError();
}

void PowerApi::shutdown()
{
    if (!enableShutdownPrivilege())
        qWarning().noquote() << "Power: could not enable SE_SHUTDOWN_NAME, shutdown may fail";
    if (!ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCEIFHUNG, kShutdownReason))
        qWarning().noquote() << "Power: ExitWindowsEx(EWX_SHUTDOWN) failed, error" << GetLastError();
}

bool PowerApi::enableShutdownPrivilege() const
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES priv;
    priv.PrivilegeCount = 1;
    priv.Privileges[0].Luid = luid;
    priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // AdjustTokenPrivileges returns TRUE even when it granted nothing
    // (ERROR_NOT_ALL_ASSIGNED under policy), so GetLastError() right after
    // the call is the only reliable success check.
    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &priv, sizeof(priv), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    return adjusted && err == ERROR_SUCCESS;
}
