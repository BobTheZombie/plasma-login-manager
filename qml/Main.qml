// SPDX-License-Identifier: GPL-2.0-or-later
// Plasma-login-manager visual shell: cached SIMD wallpaper, GPU Qt Quick scenegraph.
// The controls below are backed by the current protocol. There is no fake guest
// session, session picker, brightness control or keyboard layout control.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root
    visible: false
    color: "#081124"
    width: 1600
    height: 900
    flags: Qt.FramelessWindowHint
    title: "Plasma Login Manager"
    property bool primary: primaryScreen
    property date now: new Date()
    property string selectedName: ""
    property string selectedLabel: ""
    property bool revealPassword: false
    property bool reducedMotion: true
    readonly property real scaleFactor: Math.min(width / 1600, height / 900)
    readonly property real margin: Math.max(18, width * 0.046)
    readonly property real cardWidth: Math.min(510, width * 0.65)
    readonly property real cardHeight: Math.min(484, height * 0.62)

    function selectUser(account) {
        if (!account || !account.name || backend.busy || backend.awaitingResponse)
            return
        selectedName = account.name
        selectedLabel = account.displayName || account.name
        username.text = account.name
        secret.text = ""
        secret.forceActiveFocus()
    }
    function doLogin() {
        if (backend.busy || !backend.connected || secret.text.length === 0)
            return
        if (backend.awaitingResponse)
            backend.answer(secret.text)
        else
            backend.login(username.text.trim(), secret.text)
        secret.text = ""
        revealPassword = false
    }
    function chooseAccount(name) {
        for (var i = 0; i < backend.users.length; ++i) {
            if (backend.users[i].name === name) {
                selectUser(backend.users[i])
                return
            }
        }
    }
    Component.onCompleted: {
        // C++ signals ready once the first frame is presented.
        if (backend.users.length > 0)
            selectUser(backend.users[0])
    }
    Connections {
        target: backend
        function onUsersChanged() {
            if (backend.users.length > 0 && root.selectedName === "")
                root.selectUser(backend.users[0])
        }
        function onChallengeChanged() {
            if (backend.awaitingResponse)
                Qt.callLater(function() { secret.forceActiveFocus() })
        }
        function onMessageChanged() {
            if (!backend.busy && !backend.awaitingResponse && backend.message !== "")
                secret.text = ""
        }
    }
    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: root.now = new Date()
    }

    // All overlays and controls are Qt Quick scenegraph objects; KWin and Qt RHI
    // composite on the GPU. Wallpaper pixels are synthesized once per resolution,
    // with optional runtime AVX2, then cached as one GPU texture.
    Image {
        id: wallpaper
        anchors.fill: parent
        source: "image://plmwallpaper/" +
                Math.min(2560, Math.max(320, Math.round(root.width))) + "x" +
                Math.min(1440, Math.max(240, Math.round(root.height)))
        fillMode: Image.PreserveAspectCrop
        smooth: true
        // Full-screen native-size texture: mipmaps add GPU startup work
        // without improving quality at 1:1 scaling.
        mipmap: false
        cache: true
        asynchronous: true
    }
    Rectangle {
        anchors.fill: parent
        color: "#0a0e24"
        opacity: 0.12
    }
    Rectangle {
        id: ambientGlow
        width: Math.max(root.width * .55, root.height * .8)
        height: width
        radius: width / 2
        x: root.width * .47
        y: -height * .48
        color: "#236dbd"
        opacity: .09
        SequentialAnimation on opacity {
            running: !root.reducedMotion
            loops: Animation.Infinite
            NumberAnimation { from: .065; to: .13; duration: 11000; easing.type: Easing.InOutSine }
            NumberAnimation { from: .13; to: .065; duration: 11000; easing.type: Easing.InOutSine }
        }
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#10000817" }
            GradientStop { position: .54; color: "#00000000" }
            GradientStop { position: 1; color: "#63020719" }
        }
    }

    // Branding, kept top-left and clear of credential content.
    Row {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: root.margin
        anchors.topMargin: Math.max(19, root.height * .032)
        spacing: 15
        Rectangle {
            width: 48; height: 48; radius: 15
            gradient: Gradient {
                GradientStop { position: 0; color: "#29baff" }
                GradientStop { position: 1; color: "#2b6bee" }
            }
            Text {
                anchors.centerIn: parent
                text: "✦"
                color: "white"
                font.pixelSize: 30
                font.weight: Font.Bold
            }
        }
        Column {
            spacing: 2
            Text {
                text: "Plasma-login-manager"
                color: "#f4f8ff"
                font.pixelSize: Math.max(15, Math.min(23, root.width * .014))
                font.weight: Font.DemiBold
            }
            Text {
                text: "KDE Plasma 6"
                color: "#aec3e4"
                font.pixelSize: 14
            }
        }
    }

    // Clock follows KScreenLocker visual hierarchy. Its text updates at 1 Hz;
    // the GPU background stays cached between ticks.
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(50, root.height * .076)
        spacing: 2
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatTime(root.now, "h:mm AP")
            color: "#f6f9ff"
            font.pixelSize: Math.max(48, Math.min(84, root.height * .094))
            font.weight: Font.Light
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatDate(root.now, "dddd, MMMM d, yyyy")
            color: "#cad9f2"
            font.pixelSize: Math.max(14, Math.min(19, root.height * .024))
        }
    }

    // User rail. Actual local accounts only; never imply guest sessions exist.
    Rectangle {
        id: accountRail
        visible: root.primary && root.width >= 1120
        x: root.margin
        anchors.verticalCenter: loginCard.verticalCenter
        width: Math.min(300, root.width * .19)
        height: Math.min(390, Math.max(180, userRepeater.count * 70 + 30))
        radius: 19
        color: "#461e345d"
        border.color: "#536a94"
        border.width: 1
        clip: true
        ListView {
            id: userRepeater
            anchors.fill: parent
            anchors.margins: 12
            spacing: 6
            model: backend.users
            clip: true
            interactive: contentHeight > height
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                height: 61
                radius: 13
                readonly property bool chosen: modelData.name === root.selectedName
                color: chosen ? "#693776bd" : (accountMouse.containsMouse ? "#3e526f9e" : "#00000000")
                border.width: chosen ? 1 : 0
                border.color: "#65bfff"
                Row {
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 13
                    Rectangle {
                        width: 38; height: 38; radius: 19
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#36adff" }
                            GradientStop { position: 1; color: "#6562dc" }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: modelData.name.length ? modelData.name.charAt(0).toUpperCase() : "?"
                            color: "white"
                            font.pixelSize: 19
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.displayName || modelData.name
                        color: "#f7faff"
                        font.pixelSize: 16
                        font.weight: chosen ? Font.DemiBold : Font.Normal
                        width: Math.max(60, accountRail.width - 100)
                        elide: Text.ElideRight
                    }
                }
                MouseArea {
                    id: accountMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: !backend.busy && !backend.awaitingResponse
                    onClicked: root.selectUser(modelData)
                }
            }
        }
        Text {
            anchors.centerIn: parent
            visible: userRepeater.count === 0
            text: "Enter a username to sign in"
            color: "#b7cae3"
            font.pixelSize: 13
        }
    }

    Rectangle {
        id: loginCard
        visible: root.primary
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: Math.max(18, root.height * .055)
        width: root.cardWidth
        height: root.cardHeight
        radius: 25
        color: "#9e213b69"
        border.color: "#8397c3"
        border.width: 1
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: parent.radius - 1
            gradient: Gradient {
                GradientStop { position: 0; color: "#64375286" }
                GradientStop { position: .60; color: "#6c1b3259" }
                GradientStop { position: 1; color: "#814e529a" }
            }
        }
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 54, 395)
            spacing: 13
            Rectangle {
                width: 112; height: 112; radius: 56
                anchors.horizontalCenter: parent.horizontalCenter
                gradient: Gradient {
                    GradientStop { position: 0; color: "#23c4fc" }
                    GradientStop { position: .52; color: "#407beb" }
                    GradientStop { position: 1; color: "#8052da" }
                }
                border.color: "#8dd7ff"
                border.width: 2
                Text {
                    anchors.centerIn: parent
                    text: root.selectedName.length ? root.selectedName.charAt(0).toUpperCase() : "✦"
                    color: "white"
                    font.pixelSize: 59
                    font.weight: Font.Bold
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.selectedLabel.length ? root.selectedLabel : "Welcome"
                color: "white"
                font.pixelSize: 25
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Local account · Plasma (Wayland)"
                color: "#c2cee6"
                font.pixelSize: 13
            }
            TextField {
                id: username
                width: parent.width
                height: 46
                visible: root.selectedName === "" || backend.users.length === 0 || root.width < 1120
                placeholderText: "Username"
                color: "white"
                font.pixelSize: 15
                selectByMouse: false
                enabled: backend.connected && !backend.busy && !backend.awaitingResponse
                onTextEdited: {
                    root.selectedName = text
                    root.selectedLabel = text
                }
                Keys.onReturnPressed: secret.forceActiveFocus()
                background: Rectangle {
                    radius: 12
                    color: "#8e233353"
                    border.width: 1
                    border.color: username.activeFocus ? "#5ac8ff" : "#7889ae"
                }
            }
            Row {
                width: parent.width
                height: 49
                spacing: 4
                Rectangle {
                    width: parent.width
                    height: parent.height
                    radius: 12
                    color: "#a41a2a50"
                    border.color: secret.activeFocus ? "#49b9ff" : "#7282aa"
                    border.width: 1
                    TextField {
                        id: secret
                        anchors.left: parent.left
                        anchors.right: visibilityButton.left
                        anchors.leftMargin: 10
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        height: 42
                        placeholderText: backend.awaitingResponse ?
                                             (backend.challenge.length ? backend.challenge : "Additional authentication") :
                                             "Password"
                        color: "white"
                        font.pixelSize: 16
                        echoMode: root.revealPassword || (backend.awaitingResponse && backend.challengeEcho)
                                  ? TextInput.Normal : TextInput.Password
                        enabled: backend.connected && !backend.busy
                        focus: true
                        Keys.onReturnPressed: root.doLogin()
                        background: Item {}
                    }
                    Button {
                        id: visibilityButton
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 40; height: 38
                        flat: true
                        text: root.revealPassword ? "◉" : "◎"
                        enabled: !backend.busy && !backend.awaitingResponse
                        onClicked: root.revealPassword = !root.revealPassword
                        contentItem: Text {
                            text: visibilityButton.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: "#c9def8"
                            font.pixelSize: 21
                        }
                        background: Rectangle { color: "transparent" }
                    }
                }
            }
            Button {
                id: loginButton
                width: parent.width
                height: 51
                enabled: backend.connected && !backend.busy
                text: backend.busy ? "Authenticating…" : (backend.awaitingResponse ? "Continue" : "→     Log In")
                onClicked: root.doLogin()
                background: Rectangle {
                    radius: 12
                    gradient: Gradient {
                        GradientStop { position: 0; color: loginButton.enabled ? "#279bfa" : "#506881" }
                        GradientStop { position: 1; color: loginButton.enabled ? "#4454ec" : "#36445f" }
                    }
                }
                contentItem: Text {
                    text: loginButton.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    color: "white"
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
            }
            Text {
                width: parent.width
                height: 36
                text: backend.message
                color: backend.awaitingResponse ? "#d3e5fb" : "#ffe0cd"
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }
    Rectangle {
        visible: root.primary
        anchors.top: loginCard.bottom
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(368, loginCard.width - 50)
        height: 44; radius: 12
        color: "#61293c6e"
        border.color: "#5d709b"
        Row {
            anchors.centerIn: parent
            spacing: 12
            Text { text: "▣"; color: "#d3e6ff"; font.pixelSize: 22 }
            Text { text: "Session"; color: "#9db3d4"; font.pixelSize: 12 }
            Text { text: "Plasma (Wayland)"; color: "white"; font.pixelSize: 14 }
        }
    }

    Row {
        id: powerRow
        visible: root.primary
        anchors.right: parent.right
        anchors.rightMargin: root.margin
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.max(22, root.height * .035)
        spacing: 13
        Repeater {
            model: [
                {name: "Sleep", glyph: "☾", action: "suspend"},
                {name: "Restart", glyph: "↻", action: "reboot"},
                {name: "Shut Down", glyph: "⏻", action: "poweroff"}
            ]
            delegate: Column {
                required property var modelData
                width: 83
                spacing: 7
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 53; height: 53
                    text: modelData.glyph
                    onClicked: {
                        powerDialog.action = modelData.action
                        powerDialog.open()
                    }
                    contentItem: Text {
                        text: parent.text
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 28
                    }
                    background: Rectangle {
                        radius: 27
                        color: parent.hovered ? "#7b4c79af" : "#7840527a"
                        border.color: "#6483b6"
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: modelData.name
                    color: "#f2f6ff"
                    font.pixelSize: 12
                }
            }
        }
    }
    Row {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: root.margin
        spacing: 11
        Text { text: "ⓘ"; color: "#f1f7ff"; font.pixelSize: 19 }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "LPM Linux"
            color: "#e7efff"
            font.pixelSize: 15
        }
    }
    Dialog {
        id: powerDialog
        parent: root.contentItem
        property string action: ""
        title: action === "suspend" ? "Suspend computer?" :
               (action === "reboot" ? "Restart computer?" : "Shut down computer?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: backend.power(action)
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        visible: !root.primary
        text: "Plasma 6 · Wayland"
        color: "#c5d6ef"
        font.pixelSize: 14
    }
}
