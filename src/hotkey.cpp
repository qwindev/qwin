#include "hotkey.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QKeySequence>

#include <windows.h>

#ifndef MOD_NOREPEAT // needs _WIN32_WINNT >= Windows 7; be safe
#define MOD_NOREPEAT 0x4000
#endif

namespace {

// A null HWND posts WM_HOTKEY to the registering thread's queue, and a
// native event filter is the only place such window-less messages surface.
// One process-wide filter dispatches to the matching Hotkey by id.
class HotkeyDispatcher : public QAbstractNativeEventFilter
{
public:
    QHash<int, Hotkey *> hotkeys;
    int nextId = 1; // application RegisterHotKey ids may use 0x0000..0xBFFF

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override
    {
        if (eventType != "windows_generic_MSG")
            return false;
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message != WM_HOTKEY)
            return false;
        Hotkey *hotkey = hotkeys.value(int(msg->wParam));
        if (!hotkey)
            return false;
        hotkey->trigger();
        return true;
    }
};

HotkeyDispatcher *dispatcher()
{
    static HotkeyDispatcher *instance = [] {
        auto *d = new HotkeyDispatcher; // leaked: must outlive every Hotkey
        QCoreApplication::instance()->installNativeEventFilter(d);
        return d;
    }();
    return instance;
}

// Qt key -> virtual-key code, for the keys that make sense in a global
// chord; 0 for the rest.
UINT toVirtualKey(Qt::Key key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return UINT('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return UINT('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return UINT(VK_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Pause: return VK_PAUSE;
    case Qt::Key_Print: return VK_SNAPSHOT;
    case Qt::Key_Comma: return VK_OEM_COMMA;
    case Qt::Key_Period: return VK_OEM_PERIOD;
    case Qt::Key_Plus: return VK_OEM_PLUS;
    case Qt::Key_Minus: return VK_OEM_MINUS;
    default: return 0;
    }
}

// Parses e.g. "Shift+Alt+1" ("Meta" = the Windows key). False for anything
// that is not exactly one chord with a mappable main key.
bool toNativeHotkey(const QString &sequence, UINT *modifiers, UINT *virtualKey)
{
    const QKeySequence seq(sequence, QKeySequence::PortableText);
    if (seq.count() != 1)
        return false;

    const QKeyCombination combo = seq[0];
    UINT mods = MOD_NOREPEAT; // once per press, not per autorepeat
    if (combo.keyboardModifiers() & Qt::ShiftModifier)
        mods |= MOD_SHIFT;
    if (combo.keyboardModifiers() & Qt::ControlModifier)
        mods |= MOD_CONTROL;
    if (combo.keyboardModifiers() & Qt::AltModifier)
        mods |= MOD_ALT;
    if (combo.keyboardModifiers() & Qt::MetaModifier)
        mods |= MOD_WIN;

    const UINT vk = toVirtualKey(combo.key());
    if (vk == 0)
        return false;

    *modifiers = mods;
    *virtualKey = vk;
    return true;
}

} // namespace

Hotkey::Hotkey(QObject *parent)
    : QObject(parent)
    , m_id(dispatcher()->nextId++)
{
}

Hotkey::~Hotkey()
{
    unregister();
}

void Hotkey::setSequence(const QString &sequence)
{
    if (m_sequence == sequence)
        return;
    m_sequence = sequence;
    emit sequenceChanged();
    update();
}

void Hotkey::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    emit enabledChanged();
    update();
}

void Hotkey::componentComplete()
{
    m_complete = true;
    update();
}

// Deferred until componentComplete so a declaration registers once rather
// than once per property write.
void Hotkey::update()
{
    if (!m_complete)
        return;

    unregister();

    if (!m_enabled || m_sequence.isEmpty())
        return;

    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!toNativeHotkey(m_sequence, &modifiers, &virtualKey)) {
        qWarning() << "Hotkey: cannot use" << m_sequence
                   << "- expected a single chord like \"Shift+Alt+1\" or \"Ctrl+F9\"";
        return;
    }
    if (!RegisterHotKey(nullptr, m_id, modifiers, virtualKey)) {
        qWarning() << "Hotkey: failed to register" << m_sequence
                   << "- the chord is likely taken by another application";
        return;
    }

    dispatcher()->hotkeys.insert(m_id, this);
    m_registered = true;
    emit registeredChanged();
}

void Hotkey::unregister()
{
    if (!m_registered)
        return;
    UnregisterHotKey(nullptr, m_id);
    dispatcher()->hotkeys.remove(m_id);
    m_registered = false;
    emit registeredChanged();
}
