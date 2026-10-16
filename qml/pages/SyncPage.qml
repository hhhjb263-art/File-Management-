import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs          // FolderDialog：挑本地目录
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    文件夹同步页（PRD F3）。

    ⚠️ 页面对用户要交代清楚的四件事（同步是最容易"悄悄做错事"的功能，界面不能含糊）：
      1. **方向**：双向 / 仅上传 / 仅下载 —— 决定"云端改动会不会拉回本地"。
      2. **冲突**：两端都改时**两份都留**，较旧的那份存成「冲突副本」，绝不静默覆盖。
      3. **删除**：云端删除进云端回收站；**本地删除会把文件搬进本机的"同步保险箱"**
         （不是直接抹掉）—— 位置在页面底部明确给出，否则用户根本不知道去哪儿找。
      4. **索引**：只有"两端确认一致"的路径才会被记为已同步；被方向策略跳过的**不记**，
         这样以后切换方向不会把之前的改动当成"已同步"而漏掉。

    ⚠️ 所有 Sync.* / SyncModel 访问都用 typeof 守卫：C++ 未落地时退化为占位态，绝不抛错。
       （SyncController 现在 supported=true；守卫仍保留，便于服务端/装配层回退时不白屏。）
*/
Item {
    id: page

    readonly property bool apiReady: (typeof Sync !== "undefined")
    readonly property bool supported: apiReady
                                      && (Sync.supported === undefined ? true : Sync.supported)
    readonly property bool modelReady: (typeof SyncModel !== "undefined")

    readonly property bool   running:    apiReady && Sync.running === true
    readonly property int    pairCount:  apiReady && Sync.pairCount !== undefined ? Sync.pairCount : 0
    readonly property bool   hasPairs:   pairCount > 0
    readonly property string deviceName: apiReady && Sync.deviceName !== undefined
                                         ? String(Sync.deviceName) : "本机"
    readonly property var    logEntries: apiReady && Sync.logEntries !== undefined
                                         ? Sync.logEntries : []
    readonly property int    scanMin:    apiReady && Sync.scanIntervalMin !== undefined
                                         ? Sync.scanIntervalMin : 10
    readonly property string guardLocation: apiReady && Sync.localTrashLocation !== undefined
                                            ? String(Sync.localTrashLocation()) : ""

    // 页内就近反馈（沿用各页既有通道）
    property string statusText: ""
    property bool   statusOk: true
    property string formError: ""
    property int    newMode: 0          // 0 双向 / 1 仅上传 / 2 仅下载
    // 选择性同步编辑区当前展开的同步对 id（空 = 全部收起）。
    // 用"内联展开"而不是弹窗：少一个文件、少一次 qrc 登记，交互也更直接。
    property string expandedPairId: ""

    function setStatus(text, ok) {
        page.statusText = String(text)
        page.statusOk = (ok !== false)
    }

    function modeName(m) {
        if (typeof Sync !== "undefined" && typeof Sync.modeText === "function")
            return Sync.modeText(m)
        return ["双向同步", "仅上传", "仅下载"][m] || "双向同步"
    }

    function doCreate() {
        page.formError = ""
        if (!apiReady)
            return
        const err = Sync.addPair(localField.text, remoteField.text, page.newMode)
        if (err && String(err).length > 0) {
            page.formError = String(err)
            page.setStatus(String(err), false)
            return
        }
        localField.text = ""
        remoteField.text = ""
        page.setStatus("同步对已创建，已开始对账", true)
    }

    function doRemove(id) {
        if (apiReady)
            Sync.removePair(String(id))
    }

    function doSyncNow(id) {
        if (apiReady)
            Sync.syncNow(id === undefined ? "" : String(id))
    }

    // ==================================================================
    //  占位态：C++ 未就绪 / 服务端回退
    // ==================================================================
    UnsupportedPage {
        anchors.fill: parent
        visible: !page.supported
        featureName: qsTr("文件夹同步")
        iconName: "exchange"
        notice: apiReady && Sync.unsupportedNotice !== undefined
                && String(Sync.unsupportedNotice).length > 0
                ? String(Sync.unsupportedNotice)
                : qsTr("当前构建未启用同步模块。")
    }

    // ==================================================================
    //  正文
    // ==================================================================
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM
        visible: page.supported

        // ---------------- 标题行 ----------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label {
                    text: qsTr("文件夹同步")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontDisplay
                    font.weight: Font.Medium
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("把本机目录与云端目录绑定为一个「同步对」。启动时全量对账一次，"
                               + "之后监听目录变化（静默 %1 秒后合并对账），并每 %2 分钟兜底全量对账一次。")
                          .arg(5).arg(page.scanMin)
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }

            StatusBadge {
                Layout.alignment: Qt.AlignVCenter
                visible: page.apiReady
                text: page.running ? qsTr("同步运行中") : qsTr("已暂停")
                tone: page.running ? "success" : "neutral"
                outlined: true
            }
            GhostButton {
                iconName: "refresh"
                text: qsTr("全部立即同步")
                enabled: page.apiReady && page.hasPairs
                onClicked: {
                    page.doSyncNow("")
                    page.setStatus("已触发全部同步对", true)
                }
            }
            PrimaryButton {
                iconName: page.running ? "pause" : "play"
                text: page.running ? qsTr("暂停") : qsTr("启动")
                onClicked: {
                    if (!page.apiReady)
                        return
                    if (page.running)
                        Sync.stop()
                    else
                        Sync.start()
                }
            }
        }

        // 页内反馈条
        Rectangle {
            Layout.fillWidth: true
            visible: page.statusText.length > 0
            implicitHeight: visible ? feedbackRow.implicitHeight + Theme.spaceS * 2 : 0
            radius: Theme.radiusControl
            color: page.statusOk ? Theme.successSoft : Theme.dangerSoft
            RowLayout {
                id: feedbackRow
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceM
                anchors.rightMargin: Theme.spaceM
                spacing: Theme.spaceS
                CvIcon {
                    Layout.alignment: Qt.AlignVCenter
                    name: page.statusOk ? "check-circle" : "warning"
                    size: Theme.iconSize
                    color: page.statusOk ? Theme.success : Theme.danger
                }
                Label {
                    Layout.fillWidth: true
                    text: page.statusText
                    color: page.statusOk ? Theme.success : Theme.danger
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
                GhostButton {
                    iconName: "close"
                    onClicked: page.statusText = ""
                }
            }
        }

        // ---------------- 新建同步对 ----------------
        SectionCard {
            Layout.fillWidth: true
            title: qsTr("新建同步对")
            subtitle: qsTr("本机目录必须真实存在；云端目录会自动创建")
            iconName: "plus"

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.spaceM
                    rowSpacing: Theme.spaceS

                    Label {
                        text: qsTr("本机目录")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        TextField {
                            id: localField
                            Layout.fillWidth: true
                            implicitHeight: Theme.controlHeight
                            placeholderText: qsTr("例如 D:/文档/工作")
                            selectByMouse: true
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontBody
                            background: Rectangle {
                                radius: Theme.radiusControl
                                color: Theme.surfaceSunken
                                border.width: 1
                                border.color: localField.activeFocus ? Theme.primary
                                                                     : Theme.borderStrong
                            }
                        }
                        SecondaryButton {
                            text: qsTr("浏览…")
                            onClicked: localPicker.open()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("选择一个本机文件夹作为同步目录")
                        }
                    }

                    Label {
                        text: qsTr("云端目录")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                    TextField {
                        id: remoteField
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight
                        placeholderText: qsTr("例如 /备份/工作")
                        selectByMouse: true
                        color: Theme.textPrimary
                        font.pointSize: Theme.fontBody
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: Theme.surfaceSunken
                            border.width: 1
                            border.color: remoteField.activeFocus ? Theme.primary
                                                                  : Theme.borderStrong
                        }
                    }

                    Label {
                        text: qsTr("同步方向")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        Repeater {
                            model: [ qsTr("双向同步"), qsTr("仅上传"), qsTr("仅下载") ]
                            delegate: GhostButton {
                                required property int index
                                required property string modelData
                                text: modelData
                                active: page.newMode === index
                                onClicked: page.newMode = index
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: page.formError.length > 0
                    text: page.formError
                    color: Theme.danger
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("双向：两端互相同步　·　仅上传：本机单向备份到云端　·　仅下载：云端单向发布到本机")
                        color: Theme.textTertiary
                        font.pointSize: Theme.fontMicro
                        wrapMode: Text.WordWrap
                    }
                    PrimaryButton {
                        iconName: "check"
                        text: qsTr("创建同步对")
                        enabled: page.apiReady
                        onClicked: page.doCreate()
                    }
                }
            }
        }

        // ---------------- 同步对列表 ----------------
        SectionCard {
            Layout.fillWidth: true
            // 高度按内容定：有数据时跟着列表长，空态给固定高度。
            // ⚠️ 空态**不能**用 Layout.fillHeight —— 与 ListView 两个 fillHeight 挤在
            //    固定高度的卡片里会互相压缩并溢出卡片边界（实测：文字直接压到下面的卡片上，
            //    ColumnLayout 默认不裁剪）。这里改用 preferredHeight 精确约束。
            Layout.preferredHeight: page.hasPairs
                                    ? Math.max(200, Math.min(pairList.contentHeight + 110, 420))
                                    : 215
            title: qsTr("同步对")
            subtitle: page.hasPairs ? qsTr("共 %1 个").arg(page.pairCount)
                                    : qsTr("尚未配置")
            iconName: "folder"

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS

                // 表头
                Rectangle {
                    Layout.fillWidth: true
                    visible: page.hasPairs
                    implicitHeight: Theme.controlHeightSmall + Theme.spaceXs
                    radius: Theme.radiusControl
                    color: Theme.surfaceSunken
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceS
                        anchors.rightMargin: Theme.spaceS
                        spacing: Theme.spaceS
                        Label { Layout.fillWidth: true
                                text: qsTr("本机目录 / 云端目录"); color: Theme.textSecondary
                                font.pointSize: Theme.fontMicro }
                        Label { Layout.preferredWidth: 74; text: qsTr("方向")
                                color: Theme.textSecondary; font.pointSize: Theme.fontMicro }
                        Label { Layout.preferredWidth: 78; text: qsTr("状态")
                                color: Theme.textSecondary; font.pointSize: Theme.fontMicro }
                        Label { Layout.preferredWidth: 108; text: qsTr("上次同步")
                                color: Theme.textSecondary; font.pointSize: Theme.fontMicro }
                        Label { Layout.preferredWidth: 96; text: qsTr("操作")
                                color: Theme.textSecondary; font.pointSize: Theme.fontMicro }
                    }
                }

                ListView {
                    id: pairList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: Theme.spaceXs
                    model: page.modelReady ? SyncModel : null
                    visible: page.hasPairs

                    delegate: Rectangle {
                        required property var model
                        readonly property bool exclOpen: page.expandedPairId === String(model.id)
                        width: pairList.width
                        // ⚠️ 高度必须把展开区算进去，否则编辑区会溢出到下一行上面。
                        //    这里靠"子项 implicitHeight 之和"驱动，且子项只用 top/left/right 锚定
                        //    （不能用 anchors.fill —— 父高度依赖子 implicitHeight 会成绑定环）。
                        height: rowLayout.implicitHeight + Theme.spaceS * 2
                                + (exclOpen ? exclBox.implicitHeight + Theme.spaceXs : 0)
                        radius: Theme.radiusControl
                        color: rowHover.containsMouse ? Theme.hover : "transparent"
                        border.width: 1
                        border.color: Theme.divider

                        MouseArea {
                            id: rowHover
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: rowLayout.height   // 只覆盖主行，别把展开的编辑区也算进来
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton   // 只做悬停高亮，不抢按钮点击
                        }

                        RowLayout {
                            id: rowLayout
                            // 不能再用 anchors.fill: parent —— 展开编辑区后父高会变大，
                            // fill 会把主行内容拉伸铺满整块（表现为行内容散开）。
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.leftMargin: Theme.spaceS
                            anchors.rightMargin: Theme.spaceS
                            anchors.topMargin: Theme.spaceS
                            spacing: Theme.spaceS

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true
                                    text: model.localPath
                                    color: model.canSync ? Theme.textPrimary : Theme.danger
                                    font.pointSize: Theme.fontBody
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: model.remotePath
                                          + (model.excludeCount > 0
                                             ? qsTr("　·　选择性同步：%1 个云端目录不下载").arg(model.excludeCount)
                                             : "")
                                          + (model.canSync ? "" : qsTr("　·　本机目录不存在，同步不会执行"))
                                    color: model.canSync ? Theme.textTertiary : Theme.danger
                                    font.pointSize: Theme.fontMicro
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: String(model.lastError).length > 0
                                    text: model.lastError
                                    color: Theme.danger
                                    font.pointSize: Theme.fontMicro
                                    elide: Text.ElideRight
                                }
                            }

                            // 同步方向：**可随时切换**，包括"正在同步中"。
                            // 后端逻辑本来就热生效（setPairMode → pushToEngine + 立即重新对账，
                            // 索引记的是真实观察到的两端状态，换方向不会漏改动），此前只是界面没有入口。
                            //
                            // ⚠️ 外面套一层 Item 先把角色值抓出来：ComboBox 自己的 `model` 属性
                            //    会遮蔽 delegate 的 `model`，届时 `model.mode` 会变成 undefined。
                            Item {
                                Layout.preferredWidth: 86
                                Layout.preferredHeight: 24
                                property int    pairMode: model.mode
                                property string pairId:   String(model.id)
                                ComboBox {
                                    anchors.fill: parent
                                    font.pointSize: Theme.fontMicro
                                    model: [qsTr("双向同步"), qsTr("仅上传"), qsTr("仅下载")]
                                    currentIndex: parent.pairMode
                                    // 用 activated（用户主动选择）而不是 currentIndex 变化：
                                    // 后者在模型刷新回填时也会触发，会和 setPairMode 互相改写。
                                    onActivated: {
                                        if (page.apiReady)
                                            Sync.setPairMode(parent.pairId, index)
                                    }
                                }
                            }

                            StatusBadge {
                                Layout.preferredWidth: 78
                                text: model.statusText
                                tone: model.status === 4 ? "danger"
                                      : (model.status === 3 ? "warning"
                                      : (model.status === 2 ? "info"
                                      : (model.status === 1 ? "success" : "neutral")))
                                outlined: true
                            }

                            Label {
                                Layout.preferredWidth: 108
                                text: model.lastSyncText
                                color: Theme.textTertiary
                                font.pointSize: Theme.fontMicro
                            }

                            RowLayout {
                                Layout.preferredWidth: 96
                                spacing: Theme.spaceXs
                                GhostButton {
                                    id: syncOneBtn
                                    iconName: "refresh"
                                    onClicked: page.doSyncNow(model.id)
                                    ToolTip.visible: syncOneBtn.hovered
                                    ToolTip.text: qsTr("立即同步这个同步对")
                                }
                                GhostButton {
                                    id: toggleBtn
                                    iconName: model.enabled ? "pause" : "play"
                                    onClicked: {
                                        if (page.apiReady)
                                            Sync.setPairEnabled(String(model.id), !model.enabled)
                                    }
                                    ToolTip.visible: toggleBtn.hovered
                                    ToolTip.text: model.enabled ? qsTr("停用") : qsTr("启用")
                                }
                                DangerButton {
                                    id: removeBtn
                                    iconName: "trash"
                                    onClicked: page.doRemove(model.id)
                                    ToolTip.visible: removeBtn.hovered
                                    ToolTip.text: qsTr("移除同步对（不会删除任何文件）")
                                }
                                GhostButton {
                                    id: exclBtn
                                    iconName: "list"
                                    active: exclOpen
                                    onClicked: page.expandedPairId = exclOpen ? "" : String(model.id)
                                    ToolTip.visible: exclBtn.hovered
                                    ToolTip.text: qsTr("选择性同步：指定哪些云端目录不下载到本地")
                                }
                            }
                        }

                        // ---- 选择性同步编辑区（FR-3-7）----
                        ColumnLayout {
                            id: exclBox
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: rowLayout.bottom
                            anchors.leftMargin: Theme.spaceS
                            anchors.rightMargin: Theme.spaceS
                            anchors.topMargin: Theme.spaceXs
                            visible: exclOpen
                            spacing: Theme.spaceXs

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("以下**云端**目录只保留在云端、不下载到本地（每行一个，相对 %1）")
                                      .arg(model.remotePath)
                                color: Theme.textTertiary
                                font.pointSize: Theme.fontMicro
                                wrapMode: Text.WordWrap
                            }
                            TextArea {
                                id: exclArea
                                Layout.fillWidth: true
                                Layout.preferredHeight: 58
                                placeholderText: qsTr("例如：\n照片/原图\n视频素材")
                                color: Theme.textPrimary
                                font.pointSize: Theme.fontSecondary
                                selectByMouse: true
                                wrapMode: TextArea.Wrap
                                background: Rectangle {
                                    radius: Theme.radiusControl
                                    color: Theme.surfaceSunken
                                    border.width: 1
                                    border.color: exclArea.activeFocus ? Theme.primary
                                                                       : Theme.borderStrong
                                }
                                // 展开时从模型回填（用户编辑会打断绑定，所以显式赋值）
                                onVisibleChanged: if (visible) text = String(model.excludeText)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceXs
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("被排除的目录不会被下载，也不会因为本地缺失而删除云端内容；上传统不受影响。")
                                    color: Theme.textTertiary
                                    font.pointSize: Theme.fontMicro
                                    wrapMode: Text.WordWrap
                                }
                                GhostButton {
                                    text: qsTr("取消")
                                    onClicked: page.expandedPairId = ""
                                }
                                PrimaryButton {
                                    iconName: "check"
                                    text: qsTr("保存")
                                    onClicked: {
                                        if (page.apiReady)
                                            Sync.setPairExcludes(String(model.id), exclArea.text)
                                        page.expandedPairId = ""
                                        page.setStatus(qsTr("选择性同步已保存"), true)
                                    }
                                }
                            }
                        }
                    }
                }

                // ⚠️ 空态放在**卡片层**（ListView 的兄弟节点），不能塞进 ListView ——
                //    非委托子项会被挪进 contentItem 参与滚动，居中语义不可靠。
                EmptyState {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    visible: !page.hasPairs
                    iconName: "folder"
                    title: qsTr("还没有同步对")
                    description: page.apiReady ? String(Sync.emptyText) : ""
                }
            }
        }

        // ---------------- 同步日志 ----------------
        SectionCard {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            title: qsTr("同步日志")
            subtitle: qsTr("最近约 400 条；每次对账都会列出每个动作")
            iconName: "list"

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.logEntries
                    spacing: 1
                    delegate: Label {
                        required property string modelData
                        width: parent.width
                        text: modelData
                        color: modelData.indexOf("✗") >= 0 ? Theme.danger : Theme.textSecondary
                        font.pointSize: Theme.fontMicro
                        wrapMode: Text.WrapAnywhere
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: page.logEntries.length === 0
                    text: qsTr("还没有同步记录。启动同步或点「立即同步」后，这里会逐条列出动作。")
                    color: Theme.textTertiary
                    font.pointSize: Theme.fontMicro
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("本机设备名：%1").arg(page.deviceName)
                        color: Theme.textTertiary
                        font.pointSize: Theme.fontMicro
                    }
                    GhostButton {
                        iconName: "close"
                        text: qsTr("清空日志")
                        enabled: page.logEntries.length > 0
                        onClicked: page.apiReady && Sync.clearLog()
                    }
                }
            }
        }

        // ---------------- 安全说明（本地删除去哪儿找）----------------
        Label {
            Layout.fillWidth: true
            text: qsTr("本地删除不会直接抹掉文件：会先搬进本机的同步保险箱 —— %1\n"
                       + "云端删除进的是云端回收站，可随时恢复。冲突时两份都保留，较旧的那份存为「冲突副本」。")
                  .arg(page.guardLocation)
            color: Theme.textTertiary
            font.pointSize: Theme.fontMicro
            wrapMode: Text.WordWrap
        }
    }

    // file:// URL → 本地路径（Windows: file:///C:/x → C:/x；POSIX: /home/x 原样）
    function toLocalPath(u) {
        let t = String(u)
        if (t.indexOf("file://") === 0) {
            t = t.substring(7)
            if (t.length > 2 && t.charAt(2) === ":")
                t = t.substring(1)
            return decodeURIComponent(t)
        }
        return decodeURIComponent(t)
    }

    // 系统目录选择对话框（避免让用户手打路径 —— 手打的路径最容易因为斜杠/大小写而同步不上）
    FolderDialog {
        id: localPicker
        title: qsTr("选择本机同步目录")
        onAccepted: localField.text = page.toLocalPath(selectedFolder)
    }
}
