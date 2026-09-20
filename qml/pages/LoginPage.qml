import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    登录 / 注册页（覆盖层，由 Main 在「未登录且服务端具备账号体系」时显示）。

    仅当 `Auth.supported && !Auth.loggedIn && !Auth.legacyMode && !Auth.initializing`
    时可见；登录成功后 Auth.loggedIn 变 true，本页自动隐藏。
    错误展示区分：密码错 / 账号锁定（倒计时禁用）/ 被禁用 / 网络不可达 / 输入不合规。
*/
Item {
    id: page

    anchors.fill: parent
    visible: Auth.supported && !Auth.loggedIn && !Auth.legacyMode && !Auth.initializing

    // 半透明遮罩，阻止与下层界面交互（未登录时不应能操作文件等）。
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.82)
    }

    // 账号锁定倒计时（429 的 Retry-After）
    property int lockCountdown: 0
    readonly property bool isRegister: tabBar.currentIndex === 1
    readonly property string errorDisplayText: {
        if (Auth.errorKind === "locked" && lockCountdown > 0)
            return qsTr("尝试次数过多，账号已锁定，请 %1 秒后重试").arg(lockCountdown)
        return Auth.errorText
    }

    Timer {
        id: lockTimer
        interval: 1000
        repeat: true
        running: lockCountdown > 0
        onTriggered: {
            if (lockCountdown > 0)
                lockCountdown--
        }
    }

    Connections {
        target: Auth
        ignoreUnknownSignals: true
        function onErrorChanged() {
            // 进入锁定态：启动倒计时；其余清空倒计时。
            lockCountdown = (Auth.errorKind === "locked" && Auth.retryAfterSeconds > 0)
                               ? Auth.retryAfterSeconds : 0
        }
        function onRegistered() {
            tabBar.currentIndex = 0
            passField.text = ""
            nameField.text = ""
        }
        function onLoggedIn() {
            userField.text = ""
            passField.text = ""
            nameField.text = ""
            lockCountdown = 0
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(380, page.width - Theme.spaceXl * 2)
        spacing: Theme.spaceL

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "☁  " + qsTr("云匣 CloudVault")
            color: Theme.textPrimary
            font.pointSize: Theme.fontTitle + 4
            font.bold: true
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spaceXs
            text: qsTr("登录以访问你的文件")
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
            visible: !isRegister
        }

        // ---- 模式切换：登录 / 注册 ----
        TabBar {
            id: tabBar
            Layout.fillWidth: true
            TabButton { text: qsTr("登录") }
            TabButton { text: qsTr("注册") }
        }

        // ---- 表单卡片 ----
        Rectangle {
            Layout.fillWidth: true
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            implicitHeight: formCol.implicitHeight + Theme.spaceL * 2

            ColumnLayout {
                id: formCol
                anchors.fill: parent
                anchors.margins: Theme.spaceL
                spacing: Theme.spaceM

                Label {
                    text: qsTr("用户名")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                }
                TextField {
                    id: userField
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight
                    font.pointSize: Theme.fontBody
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textSecondary
                    placeholderText: qsTr("用户名")
                    selectByMouse: true
                    text: Auth.userName
                    background: Rectangle {
                        implicitHeight: Theme.controlHeight
                        radius: Theme.radiusControl
                        color: Theme.card
                        border.width: userField.activeFocus ? 2 : 1
                        border.color: userField.activeFocus ? Theme.primary : Theme.border
                    }
                    onAccepted: page.submit()
                }

                Label {
                    visible: isRegister
                    text: qsTr("显示名（可选）")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                }
                TextField {
                    id: nameField
                    visible: isRegister
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight
                    font.pointSize: Theme.fontBody
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textSecondary
                    placeholderText: qsTr("显示名")
                    selectByMouse: true
                    background: Rectangle {
                        implicitHeight: Theme.controlHeight
                        radius: Theme.radiusControl
                        color: Theme.card
                        border.width: nameField.activeFocus ? 2 : 1
                        border.color: nameField.activeFocus ? Theme.primary : Theme.border
                    }
                    onAccepted: page.submit()
                }

                Label {
                    text: qsTr("密码")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    TextField {
                        id: passField
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        font.pointSize: Theme.fontBody
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textSecondary
                        placeholderText: qsTr("密码")
                        echoMode: showPw.checked ? TextInput.Normal : TextInput.Password
                        selectByMouse: true
                        background: Rectangle {
                            implicitHeight: Theme.controlHeight
                            radius: Theme.radiusControl
                            color: Theme.card
                            border.width: passField.activeFocus ? 2 : 1
                            border.color: passField.activeFocus ? Theme.primary : Theme.border
                        }
                        onAccepted: page.submit()
                    }
                    GhostButton {
                        id: showPw
                        checkable: true
                        text: showPw.checked ? qsTr("隐藏") : qsTr("显示")
                        font.pointSize: Theme.fontSecondary
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: errorDisplayText.length > 0
                    text: errorDisplayText
                    color: Theme.danger
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }

                PrimaryButton {
                    Layout.fillWidth: true
                    text: isRegister ? qsTr("注册") : qsTr("登录")
                    enabled: !Auth.busy && lockCountdown === 0
                    onClicked: page.submit()
                }
            }
        }

        // ---- 模式切换提示 ----
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: isRegister ? qsTr("已有账号？去登录") : qsTr("还没有账号？去注册")
            color: Theme.primary
            font.pointSize: Theme.fontSecondary
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: tabBar.currentIndex = isRegister ? 0 : 1
            }
        }
    }

    Component.onCompleted: userField.forceActiveFocus()

    function submit() {
        if (isRegister)
            Auth.registerUser(userField.text, passField.text, nameField.text)
        else
            Auth.login(userField.text, passField.text)
    }
}
