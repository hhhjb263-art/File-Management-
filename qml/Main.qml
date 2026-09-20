import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "theme"
import "controls"
import "pages"
import "panels"
import "dialogs"

/*!
    云匣 CloudVault 主窗口。
    · header：品牌 + 全局动作（健康检查 / 传输侧栏 / 溢出菜单）
    · 内容区：左侧导航栏（非 compact）+ 页面栈 + 右侧传输侧栏（wide）
    · footer：状态条（非 compact）/ 底部标签栏（compact）

    响应式三档（断点集中在 Theme）：
      ≥1280           三栏（列表 + 内容 + 传输侧栏）
      768–1280        侧栏改为可折叠抽屉（Drawer）
      <768            单栏 + 底部标签页 + 溢出菜单收纳次要动作
*/
ApplicationWindow {
    id: window

      width: 1280
      height: 800
      minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: qsTr("云匣 CloudVault")
    color: Theme.bg

    // ---- 断点 ----
    readonly property bool compact: width < Theme.breakpointNarrow
    readonly property bool wide: width >= Theme.breakpointWide

    // ---- 页面索引 ----
    readonly property int pageFiles: 0
    readonly property int pageTransfers: 1
    readonly property int pageShares: 2
    readonly property int pageTags: 3
    readonly property int pageTrash: 4
    readonly property int pageVersions: 5
    readonly property int pageSettings: 6

    property int navIndex: pageFiles

    // 证书指纹不匹配去抖：同一 hostPort 只弹一次（并发请求可能多次触发）。
    // 用户「清除记录并重新信任」后对该 host 复位，便于后续新的证书变更再次提示。
    property var certMismatchSeen: ({})

    // ---- 顶部通知条 ----
    property string toastText: ""
    property bool toastOk: true
    property bool toastVisible: false

    function showToast(message, ok) {
        if (message === undefined || message === null || String(message).length === 0)
            return
        toastText = String(message)
        toastOk = (ok !== false)
        toastVisible = true
        toastTimer.restart()
    }

    Timer {
        id: toastTimer
        interval: 4500
        onTriggered: window.toastVisible = false
    }

    // 主导航项（supported=false → 置灰 + tooltip；页面形状仍保留）
    readonly property var navItems: [
        { label: qsTr("文件"),   glyph: "🗂", supported: true },
        { label: qsTr("传输"),   glyph: "⇅",  supported: true },
        { label: qsTr("分享"),   glyph: "🔗", supported: true },
        { label: qsTr("标签"),   glyph: "🏷", supported: false },
        { label: qsTr("回收站"), glyph: "🗑", supported: false },
        { label: qsTr("版本"),   glyph: "🕘", supported: false },
        { label: qsTr("设置"),   glyph: "⚙",  supported: true }
    ]

    readonly property var compactNav: [
        { label: qsTr("文件"), glyph: "🗂", index: 0 },
        { label: qsTr("传输"), glyph: "⇅",  index: 1 },
        { label: qsTr("设置"), glyph: "⚙",  index: 6 }
    ]

    // ==================================================================
    //  header
    // ==================================================================
    header: ToolBar {
        id: headerBar
        implicitHeight: Theme.toolbarHeight

        background: Rectangle {
            color: Theme.card
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceM
            anchors.rightMargin: Theme.spaceM
            spacing: Theme.spaceS

            Text {
                text: "☁"
                font.pointSize: 20
                color: Theme.primary
            }
            Label {
                visible: !window.compact
                text: qsTr("云匣 CloudVault")
                color: Theme.textPrimary
                font.pointSize: Theme.fontTitle
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            GhostButton {
                visible: !window.compact
                glyph: "⇄"
                text: qsTr("健康检查")
                enabled: !App.busy
                onClicked: App.healthCheck()
            }

            // 「传输队列」抽屉开关：宽屏与紧凑**共用同一个抽屉**，默认隐藏；
            // 带进行中数量徽标（activeCount>0 时显示计数）。
            GhostButton {
                glyph: "⇅"
                text: Transfers.activeCount > 0
                      ? qsTr("传输队列 %1").arg(Transfers.activeCount)
                      : qsTr("传输队列")
                onClicked: transfersDrawer.opened ? transfersDrawer.close()
                                                  : transfersDrawer.open()
            }

            ToolButton {
                id: overflowBtn
                text: "⋮"
                font.pointSize: 18
                onClicked: overflowMenu.open()
            }
        }
    }

    // ==================================================================
    //  footer：状态条 / 底部标签栏
    // ==================================================================
    footer: ToolBar {
        id: footerBar
        implicitHeight: window.compact ? 58 : 34

        background: Rectangle {
            color: Theme.card
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.border
            }
        }

        // ---- 状态条（非 compact）----
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceM
            anchors.rightMargin: Theme.spaceM
            spacing: Theme.spaceM
            visible: !window.compact

            Label {
                Layout.fillWidth: true
                text: App.statusText
                color: App.statusText.indexOf("✗") === 0 ? Theme.danger : Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                elide: Text.ElideRight
            }
            Label {
                text: Stats.freeText.length > 0 ? qsTr("空间 %1").arg(Stats.freeText) : ""
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
            Label {
                text: Transfers.statusText
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
            ProgressBar {
                Layout.preferredWidth: 96
                visible: Transfers.busy
                indeterminate: true
            }
        }

        // ---- 底部标签栏（compact）----
        RowLayout {
            anchors.fill: parent
            spacing: 0
            visible: window.compact

            Repeater {
                model: window.compactNav
                delegate: Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Rectangle {
                        anchors.fill: parent
                        color: tabMouse.containsMouse ? Theme.hover : "transparent"
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 0
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: modelData.glyph
                            font.pointSize: 15
                            color: window.navIndex === modelData.index ? Theme.primary : Theme.textPrimary
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: modelData.label
                            font.pointSize: Theme.fontSecondary
                            color: window.navIndex === modelData.index ? Theme.primary : Theme.textSecondary
                        }
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: window.navIndex = modelData.index
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Rectangle {
                    anchors.fill: parent
                    color: moreMouse.containsMouse ? Theme.hover : "transparent"
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 0
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "⋮"
                        font.pointSize: 15
                        color: Theme.textPrimary
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("更多")
                        font.pointSize: Theme.fontSecondary
                        color: Theme.textSecondary
                    }
                }
                MouseArea {
                    id: moreMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: compactMoreMenu.open()
                }
            }
        }
    }

    // ==================================================================
    //  内容区
    // ==================================================================
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- 左侧导航栏 ----
        Rectangle {
            visible: !window.compact
            Layout.preferredWidth: Theme.navRailWidth
            Layout.fillHeight: true
            color: Theme.card

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceS
                spacing: Theme.spaceXs

                Repeater {
                    model: window.navItems
                    delegate: Item {
                        Layout.fillWidth: true
                        implicitHeight: navCol.implicitHeight + Theme.spaceM

                        // 选中态：药丸形高亮
                        Rectangle {
                            anchors.fill: parent
                            radius: height / 2
                            color: window.navIndex === index ? Theme.selected
                                 : (navMouse.containsMouse ? Theme.hover : "transparent")

                            Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        }
                        ColumnLayout {
                            id: navCol
                            anchors.centerIn: parent
                            spacing: 2
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: modelData.glyph
                                font.pointSize: 16
                                color: modelData.supported
                                       ? (window.navIndex === index ? Theme.primary : Theme.textPrimary)
                                       : Theme.disabledText
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: modelData.label
                                font.pointSize: Theme.fontSecondary
                                color: modelData.supported
                                       ? (window.navIndex === index ? Theme.primary : Theme.textSecondary)
                                       : Theme.disabledText
                            }
                        }
                        MouseArea {
                            id: navMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: window.navIndex = index
                        }
                        ToolTip.visible: navMouse.containsMouse && !modelData.supported
                        ToolTip.text: App.unsupportedNotice
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // ---- 页面栈 ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 顶部通知条（结果就近反馈；401 提供「前往设置」）
            Rectangle {
                Layout.fillWidth: true
                visible: window.toastVisible
                implicitHeight: window.toastVisible ? bannerRow.implicitHeight + Theme.spaceM : 0
                color: window.toastOk
                       ? Qt.rgba(Theme.success.r, Theme.success.g, Theme.success.b, 0.12)
                       : Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.12)
                clip: true

                RowLayout {
                    id: bannerRow
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceL
                    anchors.rightMargin: Theme.spaceL
                    spacing: Theme.spaceS

                    Text {
                        text: window.toastOk ? "✅" : "⚠️"
                        font.pointSize: Theme.fontBody
                    }
                    Label {
                        Layout.fillWidth: true
                        text: window.toastText
                        color: window.toastOk ? Theme.success : Theme.danger
                        font.pointSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    GhostButton {
                        visible: window.toastText.indexOf(qsTr("令牌无效")) >= 0
                        glyph: "⚙"
                        text: qsTr("前往设置")
                        onClicked: {
                            window.navIndex = window.pageSettings
                            window.toastVisible = false
                        }
                    }
                    GhostButton {
                        glyph: "✕"
                        onClicked: window.toastVisible = false
                    }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: window.navIndex

                FilesPage {
                    id: filesPage
                    onRequestSettings: window.navIndex = window.pageSettings
                }
                TransfersPage { id: transfersPage }
                SharesPage { id: sharesPage }
                TagsPage { id: tagsPage }
                TrashPage { id: trashPage }
                VersionsPage { id: versionsPage }
                SettingsPage { id: settingsPage }
            }
        }

        // ---- 右侧预览面板（仅 wide **且** 在文件页）----
        // 预览跟随文件页的选中项；切到其它页面时整个面板隐藏，
        // 既避免"非文件页显示一个空预览壳"，也给页面腾出宽度。
        // （FilesPage 在不可见时已 clearPreview()，这里只是把面板本身也收掉，二者一致。）
        PreviewPanel {
            id: previewPanel
            visible: window.wide && window.navIndex === window.pageFiles
            Layout.preferredWidth: Theme.sidePanelWidth
            Layout.fillHeight: true
        }
    }

    // ==================================================================
    //  传输抽屉（宽屏与紧凑**共用同一个**；默认隐藏，由工具栏「传输队列」按钮切换）
    //  内容 = 进行中队列 + 「已上传」+「已下载」两栏（HistoryGroup 与传输整页共用）
    // ==================================================================
    Drawer {
        id: transfersDrawer
        edge: Qt.RightEdge
        width: Math.min(Theme.sidePanelWidth, window.width * 0.85)
        height: window.height
        modal: false
        interactive: true
        // ⚠️ 需求「默认隐藏」：Drawer 的 opened 是**只读**属性（写它会导致整个
        //    QML 组件加载失败：Invalid property assignment）。Drawer 默认就是关闭态
        //    （position=0），所以不写即可；这里显式 close() 只是把意图钉死。
        Component.onCompleted: close()

        TransferDrawer {
            anchors.fill: parent
            onCloseRequested: transfersDrawer.close()
            onOpenFullPageRequested: {
                transfersDrawer.close()
                window.navIndex = window.pageTransfers
            }
        }
    }

    // ==================================================================
    //  菜单
    // ==================================================================
    // 顶部溢出菜单：分组（诊断 / 安全 / 关于）+ 图标
    Menu {
        id: overflowMenu
        width: 220
        // ---- 诊断 ----
        MenuItem { text: "⇄  " + qsTr("健康检查"); onTriggered: App.healthCheck() }
        MenuItem { text: "⟳  " + qsTr("刷新文件列表"); onTriggered: filesPage.refreshNow() }
        MenuItem { text: "💽  " + qsTr("刷新存储空间"); onTriggered: Stats.refresh() }
        MenuSeparator { }
        // ---- 安全 ----
        MenuItem {
            text: "🔒  " + qsTr("证书与信任…")
            onTriggered: window.navIndex = window.pageSettings
        }
        MenuSeparator { }
        // ---- 关于 ----
        MenuItem { text: "ℹ  " + qsTr("关于云匣"); onTriggered: aboutDialog.open() }
    }

    Menu {
        id: compactMoreMenu
        width: 220
        MenuItem { text: "🔗  " + qsTr("分享"); onTriggered: window.navIndex = window.pageShares }
        MenuItem { text: "🏷  " + qsTr("标签（暂不支持）"); enabled: false }
        MenuItem { text: "🗑  " + qsTr("回收站（暂不支持）"); enabled: false }
        MenuItem { text: "🕘  " + qsTr("版本（暂不支持）"); enabled: false }
        MenuSeparator { }
        MenuItem { text: "⚙  " + qsTr("连接设置"); onTriggered: window.navIndex = window.pageSettings }
        MenuItem { text: "ℹ  " + qsTr("关于云匣"); onTriggered: aboutDialog.open() }
    }

    // ==================================================================
    //  对话框
    // ==================================================================
    CertPinDialog {
        id: certPinDialog
        onDecided: (accept) => {
            // 单一信任桥：装配层现在只由 AppController 注入信任回调，
            // 因此统一通过 App 回答即可，避免多余（且已不存在的）第二通道。
            App.answerTrustPrompt(accept)
        }
    }

    OverwriteDialog {
        id: overwriteDialog
        onDecided: (taskId, overwrite) => Transfers.resolveConflict(taskId, overwrite)
    }

    AboutDialog { id: aboutDialog }

    // 证书指纹不匹配（连接已被拒绝）→ 明确的提示与恢复路径。
    // 恢复动作由 Main 代为实现（对话框不直接引用 filesPage / 导航）。
    CertMismatchDialog {
        id: certMismatchDialog
        onClearAndRetryRequested: {
            const hp = certMismatchDialog.hostPort
            if (hp.length > 0)
                window.certMismatchSeen[hp] = false   // 允许后续新的证书变更再次提示
            window.showToast(qsTr("已清除该主机指纹记录，正在重新连接…"), true)
            filesPage.refreshNow()
        }
        onOpenSettingsRequested: window.navIndex = window.pageSettings
    }

    // ==================================================================
    //  全局信号接线
    // ==================================================================
    Connections {
        target: App
        function onTrustPromptRequested(hostPort, fingerprint, subject, issuer, validity) {
            certPinDialog.openFor(hostPort, fingerprint, subject, issuer, validity)
        }
    }

    // 证书指纹不匹配：连接已被拒绝、不会自动重试；给出新旧指纹对照与恢复路径。
    // ⚠️ `App.certPinMismatch` 可能尚未落地（C++ 运行时解析）→ ignoreUnknownSignals
    //    静默跳过未知信号，加载期不刷错误；落地后自动接线。
    // 去抖：同一 hostPort 仅弹一次，避免并发请求造成弹窗风暴。
    Connections {
        target: (typeof App !== "undefined") ? App : null
        ignoreUnknownSignals: true
        function onCertPinMismatch(hostPort, expectedFingerprint, actualFingerprint,
                                   subject, issuer, validity) {
            const hp = String(hostPort)
            if (window.certMismatchSeen[hp] === true)
                return
            window.certMismatchSeen[hp] = true
            certMismatchDialog.openFor(hp, expectedFingerprint, actualFingerprint,
                                       subject, issuer, validity)
        }
    }

    Connections {
        target: Transfers
        function onConflictDetected(taskId, dir, name) {
            overwriteDialog.openFor(taskId, dir, name)
        }
        function onTaskFinished(taskId, ok, message) {
            if (String(message).length > 0)
                window.showToast(message, ok)
        }
        function onErrorOccurred(message) {
            window.showToast(message, false)
        }
    }

    Connections {
        target: Files
        function onStatusMessage(message, ok) {
            window.showToast(message, ok)
        }
        function onErrorOccurred(message) {
            window.showToast(message, false)
        }
    }

    // 文件页自动刷新「已暂停」提示 —— 复用同一个 window.showToast（不新造第二套 toast）。
    Connections {
        target: filesPage
        function onNotify(message, ok) {
            window.showToast(message, ok)
        }
    }

    // ==================================================================
    //  快捷键
    // ==================================================================
    Shortcut { sequence: "Ctrl+U"; onActivated: Files.uploadHere() }
    Shortcut { sequence: "Ctrl+D"; onActivated: Files.downloadSelected() }
    Shortcut { sequence: "Ctrl+L"; onActivated: window.navIndex = window.pageSettings }
    Shortcut {
        sequences: ["F5", "Ctrl+R"]
        onActivated: {
            // 走 FilesPage.refreshNow()：立即刷新并解除「已暂停」，
            // 与右键菜单「刷新」行为一致（F5 为全局快捷键，已经在 Main 注册，未重复添加）。
            filesPage.refreshNow()
            Stats.refresh()
        }
    }

    Component.onCompleted: {
        // 自检钩子：CV_START_PAGE 指定初始页（未设置时保持默认页，无副作用）
        if (typeof CvStartPage !== "undefined" && String(CvStartPage).length > 0) {
            const map = { "files": 0, "transfers": 1, "shares": 2, "tags": 3,
                          "trash": 4, "versions": 5, "settings": 6 }
            const v = map[String(CvStartPage).toLowerCase()]
            if (v !== undefined)
                window.navIndex = v
        }
        Files.refresh()
        Stats.refresh()
    }
}
