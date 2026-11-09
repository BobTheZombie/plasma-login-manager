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

public:
    explicit Backend(QObject *parent = nullptr);
    QString message() const { return m_message; }
    QVariantList users() const { return m_users; }
    bool busy() const { return m_busy; }
    bool isConnected() const;
    Q_INVOKABLE void ready();
    Q_INVOKABLE void login(QString user, QString password);
    Q_INVOKABLE void power(const QString &action);

signals:
    void messageChanged();
    void busyChanged();
    void connectedChanged();
    void usersChanged();

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
};
