// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root
    color: "#0a1020"
    visible: false
    width: 1366
    height: 768
    title: "Plasma Login"
    flags: Qt.FramelessWindowHint
    property bool primary: primaryScreen
    property date now: new Date()
    Component.onCompleted: backend.ready()

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: root.now = new Date()
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#182c51" }
            GradientStop { position: 0.48; color: "#0d182d" }
            GradientStop { position: 1.0; color: "#090c19" }
        }
    }
    Rectangle {
        width: root.width * 0.65
        height: width
        radius: width / 2
        x: root.width * 0.55
        y: -height * 0.58
        color: "#2f7cf0"
        opacity: 0.09
    }
    Rectangle {
        width: root.width * 0.55
        height: width
        radius: width / 2
        x: -width * 0.64
        y: root.height * 0.54
        color: "#8d68e8"
        opacity: 0.09
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Math.max(38, root.height * 0.085)
        spacing: 4

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatTime(root.now, "hh:mm")
            color: "#f3f7ff"
            font.pixelSize: Math.max(56, Math.min(100, root.height * 0.125))
            font.weight: Font.Light
            font.letterSpacing: 1.2
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatDate(root.now, "dddd, MMMM d")
            color: "#b4c7e2"
            font.pixelSize: 17
        }
    }

    Rectangle {
        id: card
        visible: root.primary
        anchors.centerIn: parent
        anchors.verticalCenterOffset: Math.max(24, root.height * 0.07)
        width: Math.min(root.width - 32, 414)
        height: 330
        radius: 22
        color: "#d9192940"
        border.color: "#436187"
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 13

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "PLASMA"
                font.pixelSize: 24
                font.weight: Font.DemiBold
                font.letterSpacing: 5
                color: "#eef5ff"
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "LPM Linux · Wayland"
                font.pixelSize: 12
                color: "#a8c1e6"
            }

            TextField {
                id: username
                focus: true
                Layout.fillWidth: true
                height: 48
                placeholderText: "Username"
                font.pixelSize: 16
                color: "white"
                selectByMouse: false
                enabled: !backend.busy
                Keys.onReturnPressed: password.forceActiveFocus()
                background: Rectangle {
                    radius: 11
                    color: "#233451"
                    border.color: username.activeFocus ? "#68a7ff" : "#4c617e"
                }
            }
            TextField {
                id: password
                Layout.fillWidth: true
                height: 48
                placeholderText: "Password"
                font.pixelSize: 16
                color: "white"
                echoMode: TextInput.Password
                enabled: !backend.busy
                Keys.onReturnPressed: loginButton.clicked()
                background: Rectangle {
                    radius: 11
                    color: "#233451"
                    border.color: password.activeFocus ? "#68a7ff" : "#4c617e"
                }
            }
            Button {
                id: loginButton
                Layout.fillWidth: true
                height: 48
                enabled: !backend.busy && backend.connected
                text: backend.busy ? "Please wait…" : "Sign in"
                font.pixelSize: 15
                font.weight: Font.DemiBold
                onClicked: {
                    backend.login(username.text.trim(), password.text)
                    password.text = ""
                }
                background: Rectangle {
                    radius: 11
                    gradient: Gradient {
                        GradientStop { position: 0; color: loginButton.enabled ? "#579bf8" : "#46617c" }
                        GradientStop { position: 1; color: loginButton.enabled ? "#3375dc" : "#394f67" }
                    }
                }
                contentItem: Text {
                    text: loginButton.text
                    font: loginButton.font
                    color: "white"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.minimumHeight: 22
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: backend.message
                color: "#ffccae"
                font.pixelSize: 12
            }
        }
    }

    Row {
        visible: root.primary
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 33
        spacing: 22

        Button {
            text: "Restart"
            flat: true
            onClicked: { powerDialog.action = "reboot"; powerDialog.open() }
        }
        Rectangle { height: 20; width: 1; color: "#6b819e" }
        Text {
            text: "Plasma 6 · Wayland"
            font.pixelSize: 12
            color: "#8fa6c4"
            anchors.verticalCenter: parent.verticalCenter
        }
        Rectangle { height: 20; width: 1; color: "#6b819e" }
        Button {
            text: "Power off"
            flat: true
            onClicked: { powerDialog.action = "poweroff"; powerDialog.open() }
        }
    }
    Dialog {
        id: powerDialog
        property string action: ""
        title: action === "reboot" ? "Restart computer?" : "Power off computer?"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: backend.power(action)
    }
    Text {
        visible: !root.primary
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 32
        color: "#a6b8d2"
        text: "Plasma · LPM Linux"
    }
}
