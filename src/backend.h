// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QByteArray>
#include <QString>

class Backend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    QString message() const { return m_message; }
    bool busy() const { return m_busy; }
    bool isConnected() const;
    Q_INVOKABLE void login(QString user, QString password);
    Q_INVOKABLE void power(const QString &action);

signals:
    void messageChanged();
    void busyChanged();
    void connectedChanged();

private:
    void connectSocket();
    void onData();
    void setMessage(const QString &message);
    void setBusy(bool value);
    QLocalSocket m_socket;
    QByteArray m_incoming;
    QString m_message;
    bool m_busy = false;
};
