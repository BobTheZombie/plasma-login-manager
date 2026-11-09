// SPDX-License-Identifier: GPL-2.0-or-later
// plmd: single-seat experimental privileged supervisor. No public password DBus.
#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
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
                                {"session","Plasma (Wayland)"}});
            }
        });

        startGreeter();
        sd_notify(0, "READY=1\nSTATUS=plmd listening; greeter launching");
        qInfo() << "plmd ready; Wayland greeter on seat0 VT1";
        return true;
    }

private:
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
            const QByteArray frame = buffer.left(i);
            buffer.remove(0, i + 1);
            QJsonParseError parse{};
            const QJsonDocument json = QJsonDocument::fromJson(frame, &parse);
            if (parse.error != QJsonParseError::NoError || !json.isObject()) {
                sendMessage(s, {{"type","error"},{"message","Invalid request"}});
                continue;
            }
            const QJsonObject obj = json.object();
            const QString kind = obj.value("type").toString();
            if (kind == "auth") {
                authenticate(s, obj);
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
        if (m_userProcess || now - m_lastAttempt < 1750) {
            sendMessage(socket, {{"type","busy"},{"message","Please try again shortly"}});
            password.fill(QChar(0));
            return;
        }
        m_lastAttempt = now;
        m_userData.clear();
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
            m_userProcess = nullptr;
            m_userActive = false;
            if (!wasActive) {
                emitAll({{"type","error"},{"message","Authentication failed"}});
            } else {
                emitAll({{"type","session-ended"},{"message","Session ended"}});
                if (!activateSession(m_greeterSession))
                    qWarning() << "Could not reactivate greeter session";
            }
            qInfo() << "User session worker exited" << code << state;
            worker->deleteLater();
        });

        worker->start();
        if (!worker->waitForStarted(2000)) {
            m_userProcess = nullptr;
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
        worker->closeWriteChannel();
        if (!secret.isEmpty()) explicit_bzero(secret.data(), secret.size());
        if (!data.isEmpty()) explicit_bzero(data.data(), data.size());
        password.fill(QChar(0));
        sendMessage(socket, {{"type","busy"},{"message","Authenticating…"}});
    }

    void power(QLocalSocket *s, const QString &action)
    {
        if (action != "reboot" && action != "poweroff") {
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
        manager.asyncCall(action == "reboot" ? "Reboot" : "PowerOff", true);
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
        connect(worker, &QProcess::finished, this, [this, worker] {
            if (m_greeterProcess == worker) m_greeterProcess = nullptr;
            m_greeterSession.clear();
            worker->deleteLater();
            qWarning() << "Greeter process stopped";
            QTimer::singleShot(2000, this, [this] {
                if (!m_greeterProcess) startGreeter();
            });
        });
        worker->start();
    }

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
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("plmd");
    Supervisor supervisor;
    if (!supervisor.start()) return 1;
    return app.exec();
}
