// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QByteArray>
#include <QString>
#include <QVariantList>

class Backend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QVariantList users READ users NOTIFY usersChanged)
    Q_PROPERTY(QString challenge READ challenge NOTIFY challengeChanged)
    Q_PROPERTY(bool awaitingResponse READ awaitingResponse NOTIFY challengeChanged)
    Q_PROPERTY(bool challengeEcho READ challengeEcho NOTIFY challengeChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    QString message() const { return m_message; }
    QVariantList users() const { return m_users; }
    QString challenge() const { return m_challenge; }
    bool awaitingResponse() const { return m_awaitingResponse; }
    bool challengeEcho() const { return m_challengeEcho; }
    bool busy() const { return m_busy; }
    bool isConnected() const;
    Q_INVOKABLE void ready();
    Q_INVOKABLE void login(QString user, QString password);
    Q_INVOKABLE void answer(QString response);
    Q_INVOKABLE void power(const QString &action);

signals:
    void messageChanged();
    void busyChanged();
    void connectedChanged();
    void usersChanged();
    void challengeChanged();

private:
    void connectSocket();
    void onData();
    void setMessage(const QString &message);
    void setBusy(bool value);
    QLocalSocket m_socket;
    QByteArray m_incoming;
    QString m_message;
    bool m_busy = false;
    bool m_uiReady = false;
    QVariantList m_users;
    QString m_challenge;
    bool m_challengeEcho = false;
    bool m_awaitingResponse = false;
};
