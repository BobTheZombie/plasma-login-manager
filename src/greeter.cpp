// SPDX-License-Identifier: GPL-2.0-or-later
#include "backend.h"
#include "wallpaper.h"
#include <QGuiApplication>
#include <QScreen>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QHash>
#include <QTimer>
#include <QDebug>

static constexpr auto Endpoint = "/run/plm/control.sock";
Backend::Backend(QObject *parent) : QObject(parent)
{
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        setMessage(QString());
        emit connectedChanged();
        if (m_uiReady) ready();
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &Backend::onData);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        setBusy(false);
        m_awaitingResponse = false;
        m_challenge.clear();
        emit challengeChanged();
        setMessage("Login service disconnected");
        emit connectedChanged();
        QTimer::singleShot(1500, this, [this] { connectSocket(); });
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) {
        setMessage("Waiting for login service…");
        QTimer::singleShot(1500, this, [this] { connectSocket(); });
    });
    connectSocket();
}

bool Backend::isConnected() const
{
    return m_socket.state() == QLocalSocket::ConnectedState;
}
void Backend::setMessage(const QString &value)
{
    if (m_message == value) return;
    m_message = value;
    emit messageChanged();
}
void Backend::setBusy(bool value)
{
    if (m_busy == value) return;
    m_busy = value;
    emit busyChanged();
}
void Backend::connectSocket()
{
    if (m_socket.state() != QLocalSocket::UnconnectedState) return;
    m_socket.connectToServer(Endpoint, QIODevice::ReadWrite);
}
void Backend::ready()
{
    m_uiReady = true;
    if (!isConnected()) return;
    m_socket.write("{\"type\":\"ready\"}\n");
    m_socket.flush();
}

void Backend::login(QString user, QString password)
{
    if (!isConnected() || m_busy) {
        setMessage("Login service unavailable");
        password.fill(QChar(0));
        return;
    }
    if (user.isEmpty() || password.isEmpty()) {
        setMessage("Enter your username and password");
        password.fill(QChar(0));
        return;
    }
    QByteArray data = QJsonDocument(QJsonObject{
        {"type","auth"}, {"user",user}, {"password",password}
    }).toJson(QJsonDocument::Compact);
    data.append('\n');
    m_socket.write(data);
    m_socket.flush();
    if (!data.isEmpty()) explicit_bzero(data.data(), data.size());
    password.fill(QChar(0));
    setMessage("Authenticating…");
    setBusy(true);
}
void Backend::answer(QString response)
{
    if (!isConnected() || !m_awaitingResponse || m_busy) {
        response.fill(QChar(0));
        return;
    }
    QByteArray packet = QJsonDocument(QJsonObject{
        {"type","answer"}, {"value",response}
    }).toJson(QJsonDocument::Compact);
    packet.append('\n');
    m_socket.write(packet);
    m_socket.flush();
    if (!packet.isEmpty()) explicit_bzero(packet.data(), packet.size());
    response.fill(QChar(0));
    m_awaitingResponse = false;
    m_challenge.clear();
    emit challengeChanged();
    setBusy(true);
    setMessage("Checking credentials…");
}

void Backend::power(const QString &action)
{
    if (!isConnected() ||
        (action != "reboot" && action != "poweroff" && action != "suspend")) return;
    QByteArray msg = QJsonDocument(QJsonObject{
        {"type","power"}, {"action",action}
    }).toJson(QJsonDocument::Compact);
    msg.append('\n');
    m_socket.write(msg);
    m_socket.flush();
}
void Backend::onData()
{
    m_incoming.append(m_socket.readAll());
    if (m_incoming.size() > 16384) {
        m_incoming.clear();
        setBusy(false);
        setMessage("Invalid response from login service");
        return;
    }
    while (true) {
        const qsizetype n = m_incoming.indexOf('\n');
        if (n < 0) break;
        const QByteArray raw = m_incoming.left(n);
        m_incoming.remove(0, n + 1);
        const auto parsed = QJsonDocument::fromJson(raw);
        if (!parsed.isObject()) continue;
        const auto object = parsed.object();
        const QString type = object.value("type").toString();
        if (type == "hello") {
            const QVariantList candidates = object.value("users").toArray().toVariantList();
            if (m_users != candidates) {
                m_users = candidates;
                emit usersChanged();
            }
            setMessage(QString());
        } else if (type == "challenge") {
            m_challenge = object.value("message").toString().left(512);
            m_challengeEcho = object.value("echo").toBool();
            m_awaitingResponse = true;
            emit challengeChanged();
            setBusy(false);
            setMessage(m_challenge);
        } else if (type == "notice") {
            setMessage(object.value("message").toString().left(512));
        } else if (type == "busy") {
            setBusy(true);
            setMessage(object.value("message").toString());
        } else if (type == "starting") {
            setBusy(true);
            setMessage("Welcome to Plasma");
        } else if (type == "session-ended") {
            m_awaitingResponse = false;
            m_challenge.clear();
            emit challengeChanged();
            setBusy(false);
            setMessage(QString());
        } else if (type == "error") {
            m_awaitingResponse = false;
            m_challenge.clear();
            emit challengeChanged();
            setBusy(false);
            setMessage(object.value("message").toString());
        }
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "wayland");
    QGuiApplication app(argc, argv);
    app.setApplicationName("Plasma-login-manager");
    app.setOrganizationName("LPM Linux");

    Backend backend;
    QQmlApplicationEngine engine;
    engine.addImageProvider("plmwallpaper", new WallpaperProvider());
    engine.rootContext()->setContextProperty("backend", &backend);
    // Override only for unprivileged nested-compositor smoke tests.
    // The privileged PAM worker constructs a clean environment, so normal
    // system startup always loads the installed /usr/share/plm/Main.qml.
    const QString qmlPath = qEnvironmentVariable(
        "PLM_QML_PATH", "/usr/share/plm/Main.qml");
    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
    if (component.isError()) {
        qCritical() << component.errors();
        return 2;
    }

    QHash<QScreen *, QQuickWindow *> windows;
    auto showScreen = [&](QScreen *screen) {
        if (!screen || windows.contains(screen)) return;
        auto *ctx = new QQmlContext(engine.rootContext(), &engine);
        ctx->setContextProperty("primaryScreen", screen == app.primaryScreen());
        QObject *object = component.create(ctx);
        auto *window = qobject_cast<QQuickWindow *>(object);
        if (!window) {
            qCritical() << component.errors();
            delete object;
            return;
        }
        QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
        window->setScreen(screen);
        window->setFlags(Qt::FramelessWindowHint);
        window->setVisibility(QWindow::FullScreen);
        window->show();
        windows.insert(screen, window);
    };
    for (QScreen *screen : app.screens()) showScreen(screen);
    QObject::connect(&app, &QGuiApplication::screenAdded,
                     &app, [&](QScreen *screen) { showScreen(screen); });
    QObject::connect(&app, &QGuiApplication::screenRemoved,
                     &app, [&](QScreen *screen) {
        if (auto *window = windows.take(screen)) window->deleteLater();
    });
    return app.exec();
}
