import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "theme"
import "controls"
import "pages"
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
        { label: qsTr("分享"),   glyph: "🔗", supported: false },
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

            GhostButton {
                visible: !window.compact && !window.wide
                glyph: "⇅"
                text: qsTr("传输侧栏")
                onClicked: transfersDrawer.open()
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
                        implicitHeight: navCol.implicitHeight + Theme.spaceS

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusControl
                            color: window.navIndex === index ? Theme.selected
                                 : (navMouse.containsMouse ? Theme.hover : "transparent")
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

        // ---- 右侧传输侧栏（仅 wide）----
        Rectangle {
            visible: window.wide
            Layout.preferredWidth: Theme.sidePanelWidth
            Layout.fillHeight: true
            color: Theme.card

            Rectangle {
                anchors.left: parent.left
                width: 1
                height: parent.height
                color: Theme.border
            }

            Loader {
                anchors.fill: parent
                sourceComponent: transfersSummaryComponent
            }
        }
    }

    // ==================================================================
    //  传输侧栏摘要（wide 侧栏 / 中档抽屉共用同一组件）
    // ==================================================================
    Component {
        id: transfersSummaryComponent

        Rectangle {
            color: Theme.card

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                spacing: Theme.spaceS

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("传输队列")
                        color: Theme.textPrimary
                        font.pointSize: Theme.fontTitle
                        font.bold: true
                    }
                    GhostButton {
                        text: qsTr("查看全部")
                        onClicked: {
                            window.navIndex = window.pageTransfers
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: Transfers.statusText
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    elide: Text.ElideRight
                }

                ListView {
                    id: summaryList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: TransferModel
                    spacing: Theme.spaceS
                    ScrollBar.vertical: ScrollBar { }

                    delegate: ColumnLayout {
                        width: ListView.view ? ListView.view.width : 0
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: (model.kind === "download" ? "⬇ " : "⬆ ") + model.fileName
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontSecondary
                            elide: Text.ElideRight
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: model.progressRatio
                        }
                        Label {
                            text: model.stateText + "  " + model.progress + "%"
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: summaryList.count === 0
                        text: qsTr("暂无传输任务")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                }

                DangerButton {
                    Layout.fillWidth: true
                    glyph: "✕"
                    text: qsTr("取消当前")
                    enabled: Transfers.busy
                    onClicked: Transfers.cancel()
                }
            }
        }
    }

    // ==================================================================
    //  中档：传输侧栏抽屉
    // ==================================================================
    Drawer {
        id: transfersDrawer
        edge: Qt.RightEdge
        width: Math.min(Theme.sidePanelWidth, window.width * 0.85)
        height: window.height
        modal: false
        interactive: true

        Loader {
            anchors.fill: parent
            sourceComponent: transfersSummaryComponent
        }
    }

    // ==================================================================
    //  菜单
    // ==================================================================
    Menu {
        id: overflowMenu
        MenuItem { text: qsTr("健康检查"); onTriggered: App.healthCheck() }
        MenuItem { text: qsTr("刷新文件列表"); onTriggered: Files.refresh() }
        MenuItem { text: qsTr("刷新存储空间"); onTriggered: Stats.refresh() }
        MenuSeparator { }
        MenuItem { text: qsTr("关于云匣"); onTriggered: aboutDialog.open() }
    }

    Menu {
        id: compactMoreMenu
        MenuItem { text: qsTr("分享（暂不支持）"); enabled: false }
        MenuItem { text: qsTr("标签（暂不支持）"); enabled: false }
        MenuItem { text: qsTr("回收站（暂不支持）"); enabled: false }
        MenuItem { text: qsTr("版本（暂不支持）"); enabled: false }
        MenuSeparator { }
        MenuItem { text: qsTr("关于云匣"); onTriggered: aboutDialog.open() }
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

    // ==================================================================
    //  全局信号接线
    // ==================================================================
    Connections {
        target: App
        function onTrustPromptRequested(hostPort, fingerprint, subject, issuer, validity) {
            certPinDialog.openFor(hostPort, fingerprint, subject, issuer, validity)
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

    // ==================================================================
    //  快捷键
    // ==================================================================
    Shortcut { sequence: "Ctrl+U"; onActivated: Files.uploadHere() }
    Shortcut { sequence: "Ctrl+D"; onActivated: Files.downloadSelected() }
    Shortcut { sequence: "Ctrl+L"; onActivated: window.navIndex = window.pageSettings }
    Shortcut {
        sequences: ["F5", "Ctrl+R"]
        onActivated: {
            Files.refresh()
            Stats.refresh()
        }
    }

    Component.onCompleted: {
        Files.refresh()
        Stats.refresh()
    }
}
