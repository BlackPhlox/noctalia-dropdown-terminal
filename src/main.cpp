/*
    dropterm — standalone Yakuake-style dropdown terminal.

    Owns its own wlr-layer-shell surface via LayerShellQt instead of being
    embedded in a shell's panel slot. The terminal itself (TextRender,
    VTermBridge, PtyIFace) is unchanged from the noctalia plugin — only the
    window ownership moved here.

    Two run modes, deliberately separate processes:
      terminal  — the layer-shell dropdown (default)
      settings  — an ordinary window (`dropterm settings`)
    See selectShellIntegration() for why they cannot share a process.

    Copyright 2026 ajunca — MIT License
*/

#include <QDir>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlEngine>
#include <QQuickView>
#include <QSurfaceFormat>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include <LayerShellQt/window.h>

#include "ipc.h"
#include "settings.h"
#include "windowcontroller.h"

namespace {

// Argument handling is done by hand, before any Qt object exists.
// QCommandLineParser would need a QCoreApplication, and the whole point is to
// reach the socket without constructing an application at all.
std::string peekCommand(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            return argv[i];
        }
    }
    return "toggle";
}

bool hasFlag(int argc, char *argv[], const char *shortFlag, const char *longFlag)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], shortFlag) == 0 || std::strcmp(argv[i], longFlag) == 0) {
            return true;
        }
    }
    return false;
}

// `-e` ends dropterm's own arguments: everything after it is the command to
// run, its flags included. Returns argc when there is no `-e`.
int findExec(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-e") == 0) {
            return i;
        }
    }
    return argc;
}

// The socket message for `-e`: "exec", the caller's directory, then the
// command's arguments, NUL-separated so that arguments keep their spaces. A
// single argument is a command line for the shell, which is what a launcher
// passing `-e "htop -d 5"` expects; several are run as given.
std::string execRequest(int argc, char *argv[], int execAt)
{
    std::error_code ec;
    std::string request = "exec";
    request += '\0';
    request += std::filesystem::current_path(ec).string();
    if (argc - execAt == 2) {
        request += '\0';
        request += "/bin/sh";
        request += '\0';
        request += "-c";
    }
    for (int i = execAt + 1; i < argc; ++i) {
        request += '\0';
        request += argv[i];
    }
    return request;
}

void printUsage()
{
    std::puts("Yakuake-style dropdown terminal on wlr-layer-shell.\n"
              "\n"
              "Usage: dropterm [command]\n"
              "       dropterm -e <program> [args...]\n"
              "\n"
              "Commands:\n"
              "  toggle     show the terminal, or hide it if visible (default)\n"
              "  show       show the terminal\n"
              "  hide       hide the terminal\n"
              "  reload     re-read configuration, including theme.conf\n"
              "  settings   open the settings window\n"
              "\n"
              "Options:\n"
              "  -e <program>   run program in a new tab and show the terminal; the\n"
              "                 rest of the line is its arguments\n"
              "  -h, --help     show this help\n"
              "  -v, --version  show version information\n"
              "\n"
              "The first invocation starts the terminal; later ones are forwarded to it\n"
              "over a socket in $XDG_RUNTIME_DIR.");
}

bool isKnownCommand(const std::string &c)
{
    return c == "toggle" || c == "show" || c == "hide" || c == "reload" || c == "settings";
}

// Commands that are only meaningful against a running terminal. Without this,
// falling through would *start* one — so a theme engine's post-hook firing on a
// wallpaper change would open a terminal on the user's desktop.
bool isRemoteOnly(const std::string &c)
{
    return c == "hide" || c == "reload";
}

// LayerShellQt's integration returns a layer surface for *every* window in the
// process, with no xdg-shell fallback, so this is a whole-process decision --
// which is exactly why settings is a separate invocation rather than a second
// window here.
//
// Terminal: LayerShellQt::Shell::useLayerShell() is deprecated as "not needed
// since Qt 6.5", but that is wrong. Without the variable the process starts,
// loads its QML and runs while never mapping a surface at all, and reports no
// error. An explicit override from the environment still wins.
//
// Settings: clear it, so the value inherited from the terminal that spawned us
// cannot turn an ordinary window into a layer surface.
void selectShellIntegration(bool wantsLayerShell)
{
    if (wantsLayerShell) {
        if (!qEnvironmentVariableIsSet("QT_WAYLAND_SHELL_INTEGRATION")) {
            qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
        }
    } else {
        qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
    }
}

int runSettings(QGuiApplication &app)
{
    Settings settings;
    // These live on the stack and outlive the view. Ownership is CppOwnership
    // by default for objects QML did not create, but say so explicitly: if the
    // engine ever decided it owned them it would delete stack memory.
    QQmlEngine::setObjectOwnership(&settings, QQmlEngine::CppOwnership);

    QQuickView view;
    view.setTitle(QStringLiteral("dropterm settings"));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setInitialProperties({
        {QStringLiteral("settings"), QVariant::fromValue(static_cast<QObject *>(&settings))},
    });
    view.setSource(QUrl(QStringLiteral("qrc:/qt/qml/dropterm/SettingsWindow.qml")));
    if (view.status() == QQuickView::Error) {
        qCritical("dropterm: failed to load settings QML");
        return 1;
    }
    view.setMinimumSize(QSize(440, 340));
    view.resize(520, 430);
    view.show();
    return app.exec();
}

// One request, from the socket or from the invocation that started us.
void handleRequest(const QByteArray &request, WindowController &controller, Settings &settings)
{
    const QList<QByteArray> parts = request.split('\0');
    if (parts.size() > 2 && parts[0] == "exec") {
        QStringList argv;
        for (qsizetype i = 2; i < parts.size(); ++i) {
            argv << QString::fromLocal8Bit(parts[i]);
        }
        controller.execute(argv, QString::fromLocal8Bit(parts[1]));
        return;
    }

    const QByteArray cmd = request.trimmed();
    if (cmd == "reload") {
        // Sent by the settings process after it flushes a write.
        settings.reload();
    } else if (cmd == "show") {
        controller.show();
    } else if (cmd == "hide") {
        controller.requestHide();
    } else if (cmd == "toggle") {
        controller.toggle();
    } else if (cmd == "settings") {
        controller.openSettings();
    }
}

int runTerminal(QGuiApplication &app, const std::string &request)
{
    // The window is hidden rather than closed when dismissed, but a compositor
    // config reload can dismiss the surface outright. Without this the daemon
    // would quit at that point and the keybind would silently stop working.
    app.setQuitOnLastWindowClosed(false);

    Settings settings;

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setColor(Qt::transparent);

    WindowController controller(&view, &settings);
    QQmlEngine::setObjectOwnership(&settings, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&controller, QQmlEngine::CppOwnership);
    view.setInitialProperties({
        {QStringLiteral("settings"), QVariant::fromValue(static_cast<QObject *>(&settings))},
        {QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller))},
    });

    // ── Layer surface ───────────────────────────────────────────────
    // The surface spans the whole usable output rather than just the terminal.
    // That is deliberate, and it is what the host shell used to do for us with
    // its own full-output backdrop layer: a surface covering only the terminal
    // is never told about clicks landing elsewhere, so outside-click dismissal
    // cannot be implemented from inside it.
    //
    // Anchoring all four edges with exclusiveZone(0) makes the compositor size
    // us to the output minus everyone else's exclusive zones, so we begin
    // exactly at the bar's lower edge with no hardcoded bar height. The QML
    // animates the panel down from above its own top edge; because a surface is
    // clipped to its own bounds, the panel emerges from under the bar and can
    // never draw over it.
    //
    // Exclusive keyboard interactivity is the other half. With on-demand,
    // focus-follows-mouse hands the keyboard to whatever the pointer drifts
    // over, so typing would silently go elsewhere. Exclusive keeps it here
    // while the terminal is mapped; compositor keybinds still take priority.
    LayerShellQt::Window *layer = LayerShellQt::Window::get(&view);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    LayerShellQt::Window::Anchors anchors;
    anchors.setFlag(LayerShellQt::Window::AnchorTop);
    anchors.setFlag(LayerShellQt::Window::AnchorBottom);
    anchors.setFlag(LayerShellQt::Window::AnchorLeft);
    anchors.setFlag(LayerShellQt::Window::AnchorRight);
    layer->setAnchors(anchors);
    layer->setExclusiveZone(0);
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    layer->setScope(QStringLiteral("dropterm"));
    layer->setActivateOnShow(true);
    // Follow the focused output, and outlive that output disappearing (a
    // virtual display being torn down) instead of being closed with it.
    layer->setWantsToBeOnActiveScreen(true);
    layer->setCloseOnDismissed(false);

    view.setSource(QUrl(QStringLiteral("qrc:/qt/qml/dropterm/Panel.qml")));
    if (view.status() == QQuickView::Error) {
        qCritical("dropterm: failed to load QML");
        return 1;
    }

    // ── Toggle IPC ──────────────────────────────────────────────────
    // Safe to clear: we only get here after failing to reach a live instance.
    const QString sockPath = QString::fromStdString(ipc::socketPath());
    QLocalServer::removeServer(sockPath);
    QLocalServer server;
    if (!server.listen(sockPath)) {
        qCritical("dropterm: cannot listen on %s: %s", qPrintable(sockPath),
                  qPrintable(server.errorString()));
        return 1;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (QLocalSocket *conn = server.nextPendingConnection()) {
            // Read once the client has finished writing: an -e command line
            // is not guaranteed to arrive in a single read.
            QObject::connect(conn, &QLocalSocket::readChannelFinished, conn, [conn, &controller, &settings] {
                handleRequest(conn->readAll(), controller, settings);
                conn->disconnectFromServer();
            });
            QObject::connect(conn, &QLocalSocket::disconnected, conn, &QObject::deleteLater);
        }
    });

    // Whatever started us: show, toggle (which shows) or exec.
    handleRequest(QByteArray::fromStdString(request), controller, settings);
    return app.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    // Only the arguments before `-e` are ours.
    int ownArgc = findExec(argc, argv);
    const bool isExec = ownArgc < argc;
    const std::string command = isExec ? "exec" : peekCommand(ownArgc, argv);

    if (hasFlag(ownArgc, argv, "-h", "--help")) {
        printUsage();
        return 0;
    }
    if (hasFlag(ownArgc, argv, "-v", "--version")) {
        std::puts("dropterm " DROPTERM_VERSION);
        return 0;
    }
    if (isExec && ownArgc + 1 == argc) {
        std::fputs("dropterm: -e needs a program to run\n", stderr);
        return 2;
    }
    if (!isExec && !isKnownCommand(command)) {
        std::fprintf(stderr,
                     "dropterm: unknown command '%s' (expected toggle, show, hide, reload or settings)\n",
                     command.c_str());
        return 2;
    }

    // ── Client path: no Qt, deliberately ────────────────────────────
    // If a terminal is already running this invocation is only a remote
    // control, and it must reach the socket without constructing an
    // application: QGuiApplication's constructor calls qFatal() (abort) when it
    // cannot create a platform plugin, so doing this the other way round meant
    // `dropterm toggle` dumped core whenever it ran without a usable display.
    // It is also simply faster — no Wayland connection, no EGL, no QML engine.
    const std::string request = isExec ? execRequest(argc, argv, ownArgc) : command;
    if (command != "settings") {
        if (ipc::send(request)) {
            return 0;
        }
        if (isRemoteOnly(command)) {
            return 0; // nothing running: nothing to hide, nothing to reload
        }
    }

    // ── From here we are the process that owns a window ─────────────
    const bool isSettings = (command == "settings");
    selectShellIntegration(!isSettings);

    // The terminal background is translucent; the alpha channel has to be
    // requested before any window exists or the compositor gets an opaque
    // surface and the rounded corners come out black.
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(fmt);

    // Not the -e command's arguments: Qt would act on ones like `-platform`.
    QGuiApplication app(ownArgc, argv);
    app.setApplicationName(QStringLiteral("dropterm"));
    app.setApplicationVersion(QStringLiteral(DROPTERM_VERSION));
    app.setDesktopFileName(QStringLiteral("dropterm"));

    return isSettings ? runSettings(app) : runTerminal(app, request);
}
