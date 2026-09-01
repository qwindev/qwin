#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QSystemTrayIcon>

#include "appsapi.h"
#include "audioapi.h"
#include "bluetoothapi.h"
#include "colorpalette.h"
#include "foregroundwindow.h"
#include "hotkey.h"
#include "mediaapi.h"
#include "panelwindow.h"
#include "powerapi.h"
#include "systemapi.h"
#include "tilingapi.h"
#include "virtualdesktops.h"
#include "pluginmanager.h"
#include "pluginregistry.h"
#include "wifiapi.h"

#include <windows.h>

// Drawn at runtime so there is no resource file.
static QIcon makeTrayIcon()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(0x2E, 0x7D, 0x6B));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(2, 2, 28, 28, 8, 8);
    p.setPen(Qt::white);
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(18);
    p.setFont(font);
    p.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("Q"));
    return QIcon(pixmap);
}

static bool copyDirRecursive(const QString &srcPath, const QString &dstPath)
{
    QDir dst(dstPath);
    if (!dst.mkpath(QStringLiteral(".")))
        return false;
    const QFileInfoList entries
        = QDir(srcPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString target = dst.filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirRecursive(entry.absoluteFilePath(), target))
                return false;
        } else if (!QFile::copy(entry.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

// First run against the default location: seed it from the examples bundled
// next to the exe, so unzip-and-double-click shows a working bar instead of
// an empty desktop. Only a *missing* dir seeds - one the user emptied on
// purpose stays empty, and --plugins-dir runs never seed.
static void seedBundledPlugins(const QString &pluginsDir)
{
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    if (!QDir(bundled).exists()
        || QDir(bundled).absolutePath().compare(pluginsDir, Qt::CaseInsensitive) == 0)
        return;
    if (copyDirRecursive(bundled, pluginsDir))
        qInfo().noquote() << "first run: seeded" << pluginsDir << "from" << bundled;
    else
        qWarning().noquote() << "first run: failed to seed" << pluginsDir << "from" << bundled;
}

int main(int argc, char *argv[])
{
    // The app quits only from the tray (quitOnLastWindowClosed is false
    // below), so a double launch - a stray shortcut, double-clicking the
    // exe again - is a real path to two instances fighting over the same
    // hotkeys, tray icon and hooks. A named mutex, held for the process
    // lifetime, is the standard way to detect that before doing any work.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\qwin-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        qWarning() << "qwin: already running, exiting";
        return 0;
    }
    // Leaked deliberately: the OS closes the handle, releasing the mutex,
    // when this process exits - there is no earlier point at which closing
    // it ourselves would be correct.
    Q_UNUSED(instanceMutex);

    QApplication app(argc, argv); // QApplication (not QGuiApplication): QSystemTrayIcon needs Widgets
    QCoreApplication::setApplicationName(QStringLiteral("qwin"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QWIN_VERSION_STR));

    // Survive having no windows (mid-reload, none installed); quit is tray-only.
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("QML desktop plugin host"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption pluginsDirOption(
        QStringLiteral("plugins-dir"),
        QStringLiteral("Load plugins from <dir> instead of %APPDATA%\\qwin\\plugins."),
        QStringLiteral("dir"));
    parser.addOption(pluginsDirOption);
    parser.process(app);

    QString pluginsDir = parser.value(pluginsDirOption);
    const bool defaultLocation = pluginsDir.isEmpty();
    if (defaultLocation) {
        pluginsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                     + QStringLiteral("/plugins");
    }
    pluginsDir = QDir(pluginsDir).absolutePath();
    if (defaultLocation && !QDir(pluginsDir).exists())
        seedBundledPlugins(pluginsDir);
    QDir().mkpath(pluginsDir);

    SystemApi systemApi(pluginsDir);
    VirtualDesktops desktops;
    ColorPalette colors(pluginsDir);
    WifiApi wifi;
    MediaApi media;
    AudioApi audio;
    ForegroundWindow activeWindow;
    PowerApi power;
    BluetoothApi bluetooth;
    AppsApi apps;
    TilingApi tiling;
    PluginRegistry registry(pluginsDir);

    const auto registerQmlTypes = [&] {
        qmlRegisterSingletonInstance("qwin", 1, 0, "System", &systemApi);
        qmlRegisterType<PanelWindow>("qwin", 1, 0, "PanelWindow");
        qmlRegisterType<Hotkey>("qwin", 1, 0, "Hotkey");
        qmlRegisterSingletonInstance("qwin", 1, 0, "Desktops", &desktops);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Colors", &colors);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Wifi", &wifi);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Media", &media);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Audio", &audio);
        qmlRegisterSingletonInstance("qwin", 1, 0, "ActiveWindow", &activeWindow);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Power", &power);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Bluetooth", &bluetooth);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Apps", &apps);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Tiler", &tiling);
        qmlRegisterSingletonInstance("qwin", 1, 0, "Plugins", &registry);
    };
    registerQmlTypes();

    // Wired here rather than inside tilingapi.cpp: the tiler keys its layouts
    // by each window's own virtual desktop, but has to keep working on a
    // machine whose VirtualDesktopAccessor.dll is missing, or missing just
    // this export - leaving the provider unset is how it finds out.
    if (desktops.supportsWindowDesktopId()) {
        tiling.setDesktopGuidProvider(
            [&desktops](void *hwnd) { return desktops.windowDesktopId(hwnd); });
    }
    QObject::connect(&desktops, &VirtualDesktops::changed, &tiling, &TilingApi::rescan);

    // The manager rebuilds the engine on shared/ reloads; each rebuild must
    // redo the registrations, as a by-instance singleton only serves the
    // engine it was registered for.
    PluginManager manager(pluginsDir, &registry);
    manager.setReregisterHook([&registerQmlTypes] {
        qmlClearTypeRegistrations();
        registerQmlTypes();
    });
    manager.loadAll();

    QSystemTrayIcon tray(makeTrayIcon());
    QMenu trayMenu;
    QObject::connect(trayMenu.addAction(QStringLiteral("Reload all plugins")),
                     &QAction::triggered, &manager, &PluginManager::reloadAll);
    QObject::connect(trayMenu.addAction(QStringLiteral("Open plugins folder")),
                     &QAction::triggered, &app, [&pluginsDir] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(pluginsDir));
    });
    trayMenu.addSeparator();
    QObject::connect(trayMenu.addAction(QStringLiteral("Quit")),
                     &QAction::triggered, &app, &QCoreApplication::quit);
    tray.setContextMenu(&trayMenu);
    tray.setToolTip(QStringLiteral("qwin — %1").arg(pluginsDir));
    tray.show();

    return app.exec();
}
