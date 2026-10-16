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
    // 默认规则：未登录且服务端**具备**账号体系、且用户没有主动跳过时挡在前面。
    // 例外（`forced`）：服务端还没有任何账号（/me 返回 501 → legacy 模式）时本页默认不显示，
    // 但顶栏「登录 / 注册」可强制打开它 —— 否则永远注册不出第一个账号，账号体系永远启用不了。
    //
    // ⚠️ loginSkipped：用户点过「跳过登录」后不再自动弹出。
    //    没有这一条时本页会把整个界面永久挡住：登录动作依赖后端（不可达/未配置时必然失败），
    //    而此前**没有任何关闭入口**（跳过按钮只在 forced 时可见）→ 用户被彻底锁死。
    visible: Auth.supported && !Auth.loggedIn && !Auth.initializing
             && ((!Auth.legacyMode && !loginSkipped) || forced)

    // 由 Main 置真以强制显示（legacy 模式下用户主动点「登录 / 注册」）
    property bool forced: false
    signal closed()

    // 用户是否已主动跳过登录（typeof 守卫：C++ 侧未落地时按"未跳过"处理，行为与改动前一致）
    readonly property bool loginSkipped:
        (typeof Auth !== "undefined" && Auth.loginSkipped !== undefined)
        ? Auth.loginSkipped : false

    // 半透明遮罩，阻止与下层界面交互（未登录时不应能操作文件等）。
    Rectangle {
        anchors.fill: parent
        color: Theme.overlay
    }

    // ---- 常驻关闭入口（右上角 ✕）----
    // 无论本页是自动弹出还是被强制打开，用户都必须能关掉它 ——
    // 这是"跳过登录、继续用访问令牌"的唯一出口。
    GhostButton {
        iconName: "close"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.spaceM
        ToolTip.visible: hovered
        ToolTip.text: qsTr("关闭（继续使用访问令牌）")
        onClicked: page.closed()
    }

    // ---- 底部「稍后」入口（文案更明确，与右上角 ✕ 同义）----
    GhostButton {
        iconName: "logout"
        text: qsTr("暂不登录，继续使用访问令牌")
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceXl
        onClicked: page.closed()
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
        function onLoggedOut() {
            // 登出后重新按「记住密码」预填，方便再次登录
            if (Auth.rememberPassword && Auth.savedUserName.length > 0)
                userField.text = Auth.savedUserName
            if (Auth.rememberPassword && Auth.savedPassword.length > 0)
                passField.text = Auth.savedPassword
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(380, page.width - Theme.spaceXl * 2)
        spacing: Theme.spaceL

        // 品牌区：应用标记 + 名称（与主窗口 header 同一形态）
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: Theme.spaceS

            Rectangle {
                implicitWidth: 34
                implicitHeight: 34
                radius: 9
                color: Theme.primary

                CvIcon {
                    anchors.centerIn: parent
                    name: "cloud"
                    size: 20
                    color: Theme.textOnPrimary
                }
            }

            Label {
                text: qsTr("云匣 CloudVault")
                color: Theme.textPrimary
                font.pointSize: Theme.fontTitle + 4
                font.bold: true
            }
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
                        // 纯图标（不用文字）：👁=显示中 → 点它变 🙈 隐藏
                        text: showPw.checked ? "🙈" : "👁"
                        font.pointSize: Theme.fontTitle
                        implicitWidth: Theme.controlHeight
                        ToolTip.visible: hovered
                        ToolTip.text: showPw.checked ? qsTr("隐藏密码") : qsTr("显示密码")
                    }
                }

                // 密码格式提示（注册模式）：与服务端规则一致（≥8 位）
                Label {
                    Layout.fillWidth: true
                    visible: isRegister
                    text: qsTr("密码至少 8 位，建议包含字母与数字")
                    color: passField.text.length > 0 && passField.text.length < 8
                               ? Theme.danger : Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }

                // 记住密码（仅登录模式；注册成功后仍需登录）
                CheckBox {
                    id: rememberBox
                    visible: !isRegister
                    Layout.fillWidth: true
                    text: qsTr("记住密码")
                    checked: Auth.rememberPassword
                    onToggled: Auth.rememberPassword = checked
                    font.pointSize: Theme.fontSecondary
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

    Component.onCompleted: {
        // 预填：「记住密码」开启时带出用户名+密码；否则只带出上次用户名
        if (Auth.savedUserName.length > 0)
            userField.text = Auth.savedUserName
        else if (Auth.userName.length > 0)
            userField.text = Auth.userName
        if (Auth.rememberPassword && Auth.savedPassword.length > 0)
            passField.text = Auth.savedPassword
        userField.forceActiveFocus()
    }

    function submit() {
        if (isRegister)
            Auth.registerUser(userField.text, passField.text, nameField.text)
        else
            Auth.login(userField.text, passField.text)
    }
}
