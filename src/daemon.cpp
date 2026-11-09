// SPDX-License-Identifier: GPL-2.0-or-later
// plmd: single-seat experimental privileged supervisor. No public password DBus.
#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QJsonArray>
#include <QVariantList>
#include <vector>
#include <QCoreApplication>
#include <algorithm>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QDebug>

#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>

static constexpr auto SocketName = "/run/plm/control.sock";
static constexpr auto SessionWorker = "/usr/libexec/plm-session";
static constexpr qsizetype MaxFrame = 16384;

static void sendMessage(QLocalSocket *socket, const QJsonObject &obj)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return;
    QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    socket->write(bytes);
    socket->flush();
}

static QJsonArray localUsers()
{
    struct Person { QString name; QString display; };
    std::vector<Person> users;
    setpwent();
    while (const passwd *pw = getpwent()) {
        if (!pw->pw_name || !pw->pw_shell || pw->pw_uid < 1000 ||
            QString::fromLocal8Bit(pw->pw_name) == "plm") continue;
        const QString shell = QString::fromLocal8Bit(pw->pw_shell);
        if (shell.endsWith("/nologin") || shell.endsWith("/false")) continue;
        const QString name = QString::fromLocal8Bit(pw->pw_name);
        const QString gecos = QString::fromLocal8Bit(pw->pw_gecos ? pw->pw_gecos : "");
        const QString display = gecos.section(',', 0, 0).trimmed();
        users.push_back({name.left(32), (display.isEmpty() ? name : display).left(64)});
        if (users.size() >= 64) break;
    }
    endpwent();
    std::sort(users.begin(), users.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
    QJsonArray array;
    for (const auto &user : users) {
        array.append(QJsonObject{{"name",user.name},{"displayName",user.display}});
    }
    return array;
}

static bool activateSession(const QString &id)
{
    if (id.isEmpty()) return false;
    QDBusInterface manager("org.freedesktop.login1",
                            "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager",
                            QDBusConnection::systemBus());
    if (!manager.isValid()) return false;
    return manager.call("ActivateSession", id).type() != QDBusMessage::ErrorMessage;
}

class Supervisor final : public QObject {
public:
    explicit Supervisor(QObject *parent = nullptr) : QObject(parent) {}

    bool start()
    {
        if (geteuid() != 0) {
            qCritical() << "plmd must run as root (systemd service)";
            return false;
        }
        const passwd *pw = getpwnam("plm");
        if (!pw || pw->pw_uid == 0) {
            qCritical() << "The unprivileged plm system account is missing";
            return false;
        }
        m_greeterUid = pw->pw_uid;
        m_greeterGid = pw->pw_gid;
        m_users = localUsers();

        char **seats = nullptr;
        int seatCount = sd_get_seats(&seats);
        bool found = false;
        if (seatCount >= 0) {
            for (int i = 0; i < seatCount; ++i) {
                if (QByteArray(seats[i]) == "seat0") found = true;
                free(seats[i]);
            }
            free(seats);
        }
        if (!found || sd_seat_can_tty("seat0") <= 0) {
            qCritical() << "No VT-capable seat0 detected by logind";
            return false;
        }

        QDir().mkpath("/run/plm");
        if (chown("/run/plm", 0, m_greeterGid) != 0
                || chmod("/run/plm", 0710) != 0) {
            qCritical() << "Could not secure /run/plm";
            return false;
        }
        QLocalServer::removeServer(SocketName);
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!m_server.listen(SocketName)) {
            qCritical() << "Could not bind control socket:" << m_server.errorString();
            return false;
        }
        if (chown(SocketName, m_greeterUid, m_greeterGid) != 0
                || chmod(SocketName, 0600) != 0) {
            qCritical() << "Could not restrict control socket";
            return false;
        }
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket *s = m_server.nextPendingConnection()) {
                ucred peer{};
                socklen_t len = sizeof(peer);
                if (getsockopt(s->socketDescriptor(), SOL_SOCKET,
                               SO_PEERCRED, &peer, &len) != 0
                               || peer.uid != m_greeterUid) {
                    qWarning() << "Rejected unauthorized local control connection";
                    s->disconnectFromServer();
                    s->deleteLater();
                    continue;
                }
                m_clients.insert(s);
                connect(s, &QLocalSocket::readyRead, this,
                        [this, s] { onData(s); });
                connect(s, &QLocalSocket::disconnected, this, [this, s] {
                    m_clients.remove(s);
                    m_buffers.remove(s);
                    s->deleteLater();
                });
                sendMessage(s, {{"type","hello"},
                                {"seat","seat0"},
                                {"session","Plasma (Wayland)"},
                                {"users",m_users}});
            }
        });

        recoverExistingSession();
        if (!m_userActive) startGreeter();
        m_recoveryMonitor.setInterval(2000);
        connect(&m_recoveryMonitor, &QTimer::timeout, this, [this] {
            if (m_recoveredSession.isEmpty() || m_userProcess) return;
            const QByteArray id = m_recoveredSession.toUtf8();
            uid_t uid = 0;
            if (sd_session_get_uid(id.constData(), &uid) < 0) {
                qInfo() << "Recovered user session ended; restoring greeter";
                m_recoveredSession.clear();
                m_userActive = false;
                if (!m_greeterProcess) startGreeter();
                else activateSession(m_greeterSession);
            }
        });
        m_recoveryMonitor.start();
        uint64_t watchdog = 0;
        if (sd_watchdog_enabled(0, &watchdog) > 0 && watchdog > 0) {
            // Watchdog heartbeats are tied to the daemon's event loop.
            const auto millis = static_cast<int>(
                std::max<uint64_t>(1000, std::min<uint64_t>(10000, watchdog / 3000)));
            m_watchdog.setInterval(millis);
            connect(&m_watchdog, &QTimer::timeout, this,
                    [] { sd_notify(0, "WATCHDOG=1"); });
            m_watchdog.start();
        }
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, [this] {
            sd_notify(0, "STOPPING=1");
            if (m_userProcess) m_userProcess->terminate();
            if (m_greeterProcess) m_greeterProcess->terminate();
        });
        sd_notify(0, "READY=1\nSTATUS=plmd listening; greeter launching");
        qInfo() << "plmd ready; Wayland greeter on seat0 VT1";
        return true;
    }

private:
    void recoverExistingSession()
    {
        char **ids = nullptr;
        const int count = sd_get_sessions(&ids);
        if (count < 0) return;
        for (int i = 0; i < count; ++i) {
            char *seat = nullptr, *type = nullptr, *klass = nullptr;
            unsigned int vt = 0;
            const int seatOk = sd_session_get_seat(ids[i], &seat);
            const int typeOk = sd_session_get_type(ids[i], &type);
            const int classOk = sd_session_get_class(ids[i], &klass);
            const int vtOk = sd_session_get_vt(ids[i], &vt);
            // Be conservative: never start a new desktop on an occupied VT2,
            // even if the existing Wayland session omits KDE's desktop label.
            const bool match = seatOk >= 0 && seat && QByteArray(seat) == "seat0"
                && typeOk >= 0 && type && QByteArray(type) == "wayland"
                && classOk >= 0 && klass && QByteArray(klass) == "user"
                && vtOk >= 0 && vt == 2;
            if (match) {
                m_recoveredSession = QString::fromUtf8(ids[i]);
                m_userActive = true;
                qWarning() << "Found existing Plasma logind session:"
                           << m_recoveredSession;
            }
            free(seat); free(type); free(klass); free(ids[i]);
        }
        free(ids);
    }

    void emitAll(const QJsonObject &message)
    {
        for (QLocalSocket *s : m_clients) sendMessage(s, message);
    }

    void onData(QLocalSocket *s)
    {
        QByteArray &buffer = m_buffers[s];
        buffer.append(s->readAll());
        if (buffer.size() > MaxFrame) {
            sendMessage(s, {{"type","error"},{"message","Request too large"}});
            s->disconnectFromServer();
            return;
        }
        while (true) {
            const qsizetype i = buffer.indexOf('\n');
            if (i < 0) break;
            QByteArray frame = buffer.left(i);
            if (i >= 0) explicit_bzero(buffer.data(), static_cast<size_t>(i + 1));
            buffer.remove(0, i + 1);
            QJsonParseError parse{};
            const QJsonDocument json = QJsonDocument::fromJson(frame, &parse);
            if (!frame.isEmpty()) explicit_bzero(frame.data(), frame.size());
            if (parse.error != QJsonParseError::NoError || !json.isObject()) {
                sendMessage(s, {{"type","error"},{"message","Invalid request"}});
                continue;
            }
            const QJsonObject obj = json.object();
            const QString kind = obj.value("type").toString();
            if (kind == "auth") {
                authenticate(s, obj);
            } else if (kind == "answer") {
                answer(s, obj.value("value").toString());
            } else if (kind == "ready") {
                m_greeterReady = true;
                sendMessage(s, {{"type","ready"},{"seat","seat0"}});
            } else if (kind == "power") {
                power(s, obj.value("action").toString());
            } else {
                sendMessage(s, {{"type","error"},{"message","Unknown request"}});
            }
        }
    }

    void authenticate(QLocalSocket *socket, const QJsonObject &obj)
    {
        const QString username = obj.value("user").toString();
        QString password = obj.value("password").toString();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        static const QRegularExpression validName("^[a-z_][a-z0-9_-]{0,31}$");
        if (!validName.match(username).hasMatch() || username == "root"
                || username == "plm" || password.isEmpty()
                || password.toUtf8().size() > 1024) {
            sendMessage(socket, {{"type","error"},{"message","Invalid credentials"}});
            password.fill(QChar(0));
            return;
        }
        if (!m_greeterReady || m_userActive || m_userProcess ||
            now < m_backoffUntil || now - m_lastAttempt < 1750) {
            sendMessage(socket, {{"type","error"},{"message","Please try again shortly"}});
            password.fill(QChar(0));
            return;
        }
        m_lastAttempt = now;
        m_userData.clear();
        m_authClient = socket;
        m_waitingForAnswer = false;
        auto *worker = new QProcess(this);
        m_userProcess = worker;
        worker->setProgram(SessionWorker);
        worker->setArguments({"--mode","user","--user",username,"--vt","2"});
        worker->setProcessChannelMode(QProcess::SeparateChannels);

        connect(worker, &QProcess::readyReadStandardOutput, this,
                [this, worker] { readWorker(worker, false); });
        connect(worker, &QProcess::readyReadStandardError, this,
                [worker] {
                    // Worker stderr never contains a password or PAM conversation.
                    qWarning().noquote() << "session-helper:" <<
                        QString::fromUtf8(worker->readAllStandardError()).trimmed();
                });
        connect(worker, &QProcess::finished, this,
                [this, worker](int code, QProcess::ExitStatus state) {
            if (m_userProcess != worker) return;
            const bool wasActive = m_userActive;
            if (!wasActive) {
                m_failedAttempts = std::min(m_failedAttempts + 1, 5);
                m_backoffUntil = QDateTime::currentMSecsSinceEpoch()
                                 + (1000LL << m_failedAttempts);
            }
            m_userProcess = nullptr;
            m_userActive = false;
            m_waitingForAnswer = false;
            m_authClient.clear();
            if (!wasActive) {
                emitAll({{"type","error"},{"message","Authentication failed"}});
            } else {
                emitAll({{"type","session-ended"},{"message","Session ended"}});
                if (!m_greeterProcess) {
                    startGreeter();
                } else if (!activateSession(m_greeterSession)) {
                    qWarning() << "Could not reactivate greeter session";
                }
            }
            qInfo() << "User session worker exited" << code << state;
            worker->deleteLater();
        });

        worker->start();
        if (!worker->waitForStarted(2000)) {
            m_userProcess = nullptr;
            m_authClient.clear();
            sendMessage(socket, {{"type","error"},{"message","Session helper failed to start"}});
            worker->deleteLater();
            password.fill(QChar(0));
            return;
        }

        QByteArray secret = password.toUtf8();
        const uint32_t n = htonl(static_cast<uint32_t>(secret.size()));
        QByteArray data(reinterpret_cast<const char *>(&n), 4);
        data.append(secret);
        worker->write(data);
        // Keep stdin open for further PAM prompts, including MFA and
        // password-change challenges. Close after SESSION is announced.
        QTimer::singleShot(120000, worker, [this, worker] {
            if (m_userProcess == worker && !m_userActive) {
                qWarning() << "PAM conversation timed out";
                worker->kill();
            }
        });
        if (!secret.isEmpty()) explicit_bzero(secret.data(), secret.size());
        if (!data.isEmpty()) explicit_bzero(data.data(), data.size());
        password.fill(QChar(0));
        sendMessage(socket, {{"type","busy"},{"message","Authenticating…"}});
    }

    void answer(QLocalSocket *socket, QString value)
    {
        if (m_authClient != socket || !m_waitingForAnswer ||
            !m_userProcess || m_userActive) {
            sendMessage(socket, {{"type","error"},{"message","No pending PAM challenge"}});
            value.fill(QChar(0));
            return;
        }
        QByteArray bytes = value.toUtf8();
        if (bytes.isEmpty() || bytes.size() > 1024 || bytes.contains('\0')) {
            sendMessage(socket, {{"type","error"},{"message","Invalid challenge response"}});
            if (!bytes.isEmpty()) explicit_bzero(bytes.data(), bytes.size());
            value.fill(QChar(0));
            return;
        }
        const uint32_t wire = htonl(static_cast<uint32_t>(bytes.size()));
        QByteArray frame(reinterpret_cast<const char *>(&wire), sizeof(wire));
        frame.append(bytes);
        const qint64 written = m_userProcess->write(frame);
        if (written != frame.size()) {
            sendMessage(socket, {{"type","error"},{"message","PAM helper unavailable"}});
        } else {
            m_waitingForAnswer = false;
            sendMessage(socket, {{"type","busy"},{"message","Checking credentials…"}});
        }
        if (!frame.isEmpty()) explicit_bzero(frame.data(), frame.size());
        if (!bytes.isEmpty()) explicit_bzero(bytes.data(), bytes.size());
        value.fill(QChar(0));
    }

    void power(QLocalSocket *s, const QString &action)
    {
        if (action != "reboot" && action != "poweroff" && action != "suspend") {
            sendMessage(s, {{"type","error"},{"message","Unsupported power action"}});
            return;
        }
        QDBusInterface manager("org.freedesktop.login1",
                               "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager",
                               QDBusConnection::systemBus());
        if (!manager.isValid()) {
            sendMessage(s, {{"type","error"},{"message","logind unavailable"}});
            return;
        }
        manager.asyncCall(action == "reboot" ? "Reboot" :
                          (action == "suspend" ? "Suspend" : "PowerOff"), true);
    }

    void readWorker(QProcess *worker, bool greeter)
    {
        QByteArray &buffer = greeter ? m_greeterData : m_userData;
        buffer.append(worker->readAllStandardOutput());
        if (buffer.size() > 4096) { buffer.clear(); return; }
        while (true) {
            const qsizetype pos = buffer.indexOf('\n');
            if (pos < 0) break;
            const QByteArray line = buffer.left(pos);
            buffer.remove(0, pos + 1);
            if (!greeter && (line.startsWith("PROMPT=") ||
                             line.startsWith("NOTICE="))) {
                const bool prompt = line.startsWith("PROMPT=");
                const QByteArray packet = line.mid(7);
                if (packet.size() < 2 || packet.at(1) != ':' ||
                    packet.size() > 1200) continue;
                const QByteArray encoded = packet.mid(2);
                const QByteArray decoded = QByteArray::fromHex(encoded);
                const QString message = QString::fromUtf8(decoded);
                if (prompt && m_authClient && !m_userActive) {
                    m_waitingForAnswer = true;
                    sendMessage(m_authClient, {{"type","challenge"},
                               {"message",message},
                               {"echo",packet.at(0) == '1'}});
                } else if (!prompt && m_authClient) {
                    sendMessage(m_authClient, {{"type","notice"},{"message",message}});
                }
                continue;
            }
            if (!line.startsWith("SESSION=")) continue;
            const QString id = QString::fromUtf8(line.mid(8));
            if (id.isEmpty()) continue;
            if (greeter) {
                m_greeterSession = id;
                qInfo() << "Greeter logind session" << id;
                if (!m_userActive && !activateSession(id))
                    qWarning() << "Could not activate greeter VT";
            } else {
                m_userActive = true;
                m_recoveredSession.clear();
                m_waitingForAnswer = false;
                if (m_userProcess) m_userProcess->closeWriteChannel();
                m_authClient.clear();
                m_failedAttempts = 0;
                m_backoffUntil = 0;
                emitAll({{"type","starting"},{"message","Starting Plasma…"}});
                if (!activateSession(id)) {
                    qWarning() << "Failed to activate user logind session" << id;
                }
            }
        }
    }

    void startGreeter()
    {
        if (m_greeterProcess) return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_greeterCrashWindow > 60000) {
            m_greeterCrashWindow = now;
            m_greeterCrashCount = 0;
        }
        if (++m_greeterCrashCount > 5) {
            qCritical() << "Greeter restarted too often; exiting for systemd recovery";
            QCoreApplication::exit(1);
            return;
        }
        m_greeterReady = false;
        m_greeterData.clear();
        auto *worker = new QProcess(this);
        m_greeterProcess = worker;
        worker->setProgram(SessionWorker);
        worker->setArguments({"--mode","greeter","--user","plm","--vt","1"});
        connect(worker, &QProcess::readyReadStandardOutput, this,
                [this, worker] { readWorker(worker, true); });
        connect(worker, &QProcess::readyReadStandardError, this,
                [worker] {
                    qWarning().noquote() << "greeter-helper:" <<
                        QString::fromUtf8(worker->readAllStandardError()).trimmed();
                });
        const auto onGreeterExit = [this, worker] {
            if (m_greeterProcess != worker) return;
            m_greeterProcess = nullptr;
            m_greeterSession.clear();
            worker->deleteLater();
            qWarning() << "Greeter process stopped";
            if (!m_userActive) {
                QTimer::singleShot(2000, this, [this] {
                    if (!m_greeterProcess) startGreeter();
                });
            }
        };
        connect(worker, &QProcess::finished, this, [onGreeterExit](int, QProcess::ExitStatus) {
            onGreeterExit();
        });
        connect(worker, &QProcess::errorOccurred, this, [onGreeterExit](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) onGreeterExit();
        });
        worker->start();
    }

    QJsonArray m_users;
    uid_t m_greeterUid{};
    gid_t m_greeterGid{};
    QLocalServer m_server;
    QSet<QLocalSocket *> m_clients;
    QHash<QLocalSocket *, QByteArray> m_buffers;
    QProcess *m_greeterProcess = nullptr;
    QProcess *m_userProcess = nullptr;
    QByteArray m_greeterData;
    QByteArray m_userData;
    QString m_greeterSession;
    bool m_userActive = false;
    qint64 m_lastAttempt = 0;
    qint64 m_backoffUntil = 0;
    qint64 m_greeterCrashWindow = 0;
    int m_greeterCrashCount = 0;
    int m_failedAttempts = 0;
    QPointer<QLocalSocket> m_authClient;
    bool m_waitingForAnswer = false;
    bool m_greeterReady = false;
    QString m_recoveredSession;
    QTimer m_recoveryMonitor;
    QTimer m_watchdog;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("plmd");
    Supervisor supervisor;
    if (!supervisor.start()) return 1;
    return app.exec();
}
