#pragma once

#include <QQmlParserStatus>
#include <QQuickWindow>
#include <QString>

// The `PanelWindow` QML type: docks to a screen edge and reserves its space
// through the Windows AppBar API, so maximized applications stop at its
// border, like the taskbar. Panels on the same edge stack automatically.
//
//   PanelWindow { edge: Qt.TopEdge; thickness: 36; visible: true }
//
// The panel owns its geometry - it spans its edge, `thickness` logical
// pixels deep - so width/height set in QML are ignored.
//
// `screenName` (szDevice, see screendevice.h) picks which monitor to dock
// on; empty means "wherever the window ends up" - MonitorFromWindow, as
// before this property existed. `device` reports back which monitor that
// resolved to, szDevice either way, so bar modules embedded in a panel can
// read their own monitor through the `Window.window` attached property.
// Never named `screen`: PanelWindow derives from QQuickWindow/QWindow, and a
// `screen` property would shadow QWindow::screen() (the same trap as the
// setParent(QObject*) note in CLAUDE.md).
class PanelWindow : public QQuickWindow, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(Qt::Edge edge READ edge WRITE setEdge NOTIFY edgeChanged)
    Q_PROPERTY(int thickness READ thickness WRITE setThickness NOTIFY thicknessChanged)
    Q_PROPERTY(QString screenName READ screenName WRITE setScreenName NOTIFY screenNameChanged)
    Q_PROPERTY(QString device READ device NOTIFY deviceChanged)
public:
    explicit PanelWindow(QWindow *parent = nullptr);
    ~PanelWindow() override;

    Qt::Edge edge() const { return m_edge; }
    void setEdge(Qt::Edge edge);

    int thickness() const { return m_thickness; }
    void setThickness(int thickness);

    QString screenName() const { return m_screenName; }
    void setScreenName(const QString &screenName);

    QString device() const { return m_device; }

    void updateAppBar();

    // Deferred until componentComplete(): QML's property assignment order is
    // unspecified, so `visible: true` could otherwise register the AppBar on
    // whatever monitor MonitorFromWindow answers before `screenName` arrives,
    // flapping the primary's work area at every start. Defaults to true so a
    // PanelWindow built straight from C++ (never through classBegin()) works
    // unblocked.
    void classBegin() override { m_complete = false; }
    void componentComplete() override;

signals:
    void edgeChanged();
    void thicknessChanged();
    void screenNameChanged();
    void deviceChanged();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void removeAppBar();
    void setDevice(const QString &device);

    Qt::Edge m_edge = Qt::TopEdge;
    int m_thickness = 36;
    QString m_screenName;
    QString m_device;
    bool m_registered = false;
    bool m_complete = true;
    void *m_hwnd = nullptr; // captured at registration, valid until removal
};
