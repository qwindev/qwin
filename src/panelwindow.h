#pragma once

#include <QQuickWindow>

// The `PanelWindow` QML type: docks to a screen edge and reserves its space
// through the Windows AppBar API, so maximized applications stop at its
// border, like the taskbar. Panels on the same edge stack automatically.
//
//   PanelWindow { edge: Qt.TopEdge; thickness: 36; visible: true }
//
// The panel owns its geometry - it spans its edge, `thickness` logical
// pixels deep - so width/height set in QML are ignored.
class PanelWindow : public QQuickWindow
{
    Q_OBJECT
    Q_PROPERTY(Qt::Edge edge READ edge WRITE setEdge NOTIFY edgeChanged)
    Q_PROPERTY(int thickness READ thickness WRITE setThickness NOTIFY thicknessChanged)
public:
    explicit PanelWindow(QWindow *parent = nullptr);
    ~PanelWindow() override;

    Qt::Edge edge() const { return m_edge; }
    void setEdge(Qt::Edge edge);

    int thickness() const { return m_thickness; }
    void setThickness(int thickness);

    void updateAppBar();

signals:
    void edgeChanged();
    void thicknessChanged();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void removeAppBar();

    Qt::Edge m_edge = Qt::TopEdge;
    int m_thickness = 36;
    bool m_registered = false;
    void *m_hwnd = nullptr; // captured at registration, valid until removal
};
