import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"
import "../dialogs"

/*!
    文件版本页：查看 / 回滚某个文件的历史内容。

    核心语义（决定了界面必须怎么长，来自服务端契约）：
      · 列表第一条是**合成的「当前版本」**（服务端在 file_node 上现取，不是快照行，
        current=true、id="0"）—— 它的「回滚 / 删除」没有意义，必须禁用，
        否则用户会点到一个永远失败（或更糟：把自己备份成自己）的按钮。
      · 每次**覆盖上传**新内容时，服务端会自动把旧内容存成一条快照 → 所以"历史版本"
        是自动积累的，用户不需要每次手动存档。
      · **回滚不会弄丢当下这版**：服务端在回滚前先自动备份（note = "恢复前备份"）。
        这句话必须在界面上明写 —— 否则没人敢点回滚。
      · 回滚是零拷贝：内容按分块寻址，历史版本与当前版本共享同一批分块，
        只有引用计数在动。所以"占多少空间"是按版本大小显示的**逻辑大小**，
        不是额外开销。别在界面上把各版本大小加起来吓唬用户。

    ⚠️ 数据源：VersionModel（角色 id/version/size/sizeText/time/device/note/current）。
    ⚠️ 所有 Versions.* 访问都用 typeof 守卫 —— C++ 未落地时本页退化为占位态，绝不抛错。
*/
Item {
    id: page

    // ---- 冻结 API 守卫 ----
    readonly property bool apiReady: (typeof Versions !== "undefined")
    readonly property bool supported: apiReady
                                      && (Versions.supported === undefined ? true : Versions.supported)
    readonly property bool verReady: apiReady && (Versions.supported === true)
    readonly property bool modelReady: (typeof VersionModel !== "undefined")

    readonly property bool verBusy: verReady && Versions.busy !== undefined ? Versions.busy : false
    readonly property int  verCount: verReady && Versions.count !== undefined ? Versions.count : 0
    readonly property bool hasTarget: verReady && Versions.hasTarget === true
    readonly property string targetName: verReady && Versions.fileName !== undefined
                                         ? String(Versions.fileName) : ""
    readonly property int currentVersion: verReady && Versions.currentVersion !== undefined
                                          ? Versions.currentVersion : 1
    readonly property string emptyText: verReady && Versions.emptyText !== undefined
                                        ? String(Versions.emptyText) : ""

    // 页内就近反馈
    property string statusText: ""
    property bool   statusOk: true

    // 待确认回滚 / 删除的目标
    property string pendingVersionId: ""
    property int    pendingVersionNo: 0
    property bool   pendingIsRestore: true

    function setStatus(text, ok) {
        page.statusText = String(text)
        page.statusOk = (ok !== false)
    }

    function doRefresh() {
        if (page.hasTarget && typeof Versions.refresh === "function")
            Versions.refresh()
    }

    function doSnapshot() {
        if (page.hasTarget && typeof Versions.snapshot === "function")
            Versions.snapshot()
    }

    function askRestore(versionId, versionNo) {
        page.pendingVersionId = String(versionId)
        page.pendingVersionNo = versionNo
        page.pendingIsRestore = true
        confirmDialog.openAs(String(versionId), qsTr("版本 v%1").arg(versionNo), {
            danger: false,
            titleText: qsTr("回滚到此版本"),
            confirmText: qsTr("回滚"),
            noticeText: qsTr("文件的当前内容会被替换为 v%1 的内容。")
                        .arg(versionNo)
                        + qsTr("放心：回滚前，现在这一版会自动存成一条新的历史版本，"
                               + "随时可以再滚回来。")
        })
    }

    function askDelete(versionId, versionNo) {
        page.pendingVersionId = String(versionId)
        page.pendingVersionNo = versionNo
        page.pendingIsRestore = false
        confirmDialog.openAs(String(versionId), qsTr("版本 v%1").arg(versionNo), {
            danger: true,
            titleText: qsTr("删除历史版本"),
            confirmText: qsTr("删除版本"),
            noticeText: qsTr("将删除 v%1 这条历史快照，之后无法再回滚到该版本，"
                             + "并释放它独占的分块空间。此操作不可恢复。").arg(versionNo)
        })
    }

    // 进页面时刷新一次（可能刚从文件页带着新目标跳过来）
    onVisibleChanged: {
        if (page.visible && page.hasTarget)
            page.doRefresh()
    }

    Connections {
        target: page.verReady ? Versions : null
        function onVersionsChanged() { /* count/currentVersion 属性已自带通知 */ }
        function onErrorOccurred(message) { page.setStatus(String(message), false) }
        function onStatusMessage(message, ok) {
            if (String(message).length > 0)
                page.setStatus(String(message), ok !== false)
        }
        // 目标文件被切换（例如文件页又选了别的文件）→ 清掉上一份提示，避免张冠李戴
        function onTargetChanged() { page.statusText = "" }
    }

    // ==================================================================
    //  未就绪兜底：原有占位页（保留既有，勿删）
    // ==================================================================
    UnsupportedPage {
        anchors.fill: parent
        visible: !page.supported
        featureName: qsTr("文件版本")
        glyph: "🕘"
        notice: (typeof Versions !== "undefined" && Versions.unsupportedNotice !== undefined
                 && String(Versions.unsupportedNotice).length > 0)
                ? Versions.unsupportedNotice
                : qsTr("当前服务端不支持文件历史版本。")
    }

    // ==================================================================
    //  真实页面
    // ==================================================================
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM
        visible: page.supported

        // ---- 头部 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label {
                    Layout.fillWidth: true
                    text: page.hasTarget
                          ? qsTr("版本历史 · %1").arg(page.targetName)
                          : qsTr("文件版本")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontTitle + 3
                    font.bold: true
                    elide: Text.ElideMiddle
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("覆盖上传新内容时，旧内容会自动存为一条历史版本，可随时回滚。")
                          + "\n" + qsTr("回滚前会自动备份当前版本，因此回滚不会丢失现有内容。")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }

            GhostButton {
                glyph: "⟳"
                text: qsTr("刷新")
                enabled: page.hasTarget && !page.verBusy
                onClicked: page.doRefresh()
            }

            SecondaryButton {
                iconName: "history"
                text: qsTr("存为历史版本")
                enabled: page.hasTarget && !page.verBusy
                onClicked: page.doSnapshot()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("把当前内容立即存成一个快照（一般不需要手动做）")
            }
        }

        // ---- 就近状态 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            visible: page.statusText.length > 0

            StatusBadge {
                text: page.statusText
                tone: page.statusOk ? "success" : "danger"
            }
            Item { Layout.fillWidth: true }
        }

        // ---- 无目标：引导去文件页选一个文件 ----
        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !page.hasTarget
            iconName: "history"
            title: qsTr("未选择文件")
            description: page.emptyText.length > 0
                         ? page.emptyText
                         : qsTr("先在「文件」页选中一个文件，再回到这里查看它的历史版本。")
            glyphColor: Theme.textTertiary
        }

        // ---- 版本列表 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.hasTarget
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // 表头
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Theme.tableHeaderHeight
                    color: Theme.tableHeader

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.border
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceM
                        anchors.rightMargin: Theme.spaceM
                        spacing: Theme.spaceS

                        Repeater {
                            model: [
                                { label: qsTr("版本"),     width: 150 },
                                { label: qsTr("大小"),     width: 90 },
                                { label: qsTr("时间"),     width: 150 },
                                { label: qsTr("说明"),     width: -1 },
                                { label: qsTr("操作"),     width: 132 }
                            ]
                            delegate: Item {
                                readonly property var cell: modelData
                                Layout.fillWidth: cell.width < 0
                                Layout.preferredWidth: cell.width < 0 ? 0 : cell.width
                                Layout.fillHeight: true

                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.label
                                    color: Theme.textTertiary
                                    font.pointSize: Theme.fontMicro
                                }
                            }
                        }
                    }
                }

                ListView {
                    id: versionList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.modelReady ? VersionModel : null
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }

                    delegate: Rectangle {
                        id: verRow

                        // current 是服务端的合成行（id="0"）：回滚/删除对它都没有意义
                        readonly property bool isCurrent: model.current === true

                        width: ListView.view ? ListView.view.width : 0
                        height: Theme.rowHeight
                        color: verMouse.containsMouse ? Theme.hover : "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.divider
                            visible: !verMouse.containsMouse
                        }

                        MouseArea {
                            id: verMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceM
                            anchors.rightMargin: Theme.spaceM
                            spacing: Theme.spaceS

                            RowLayout {
                                Layout.preferredWidth: 150
                                spacing: Theme.spaceXs

                                Label {
                                    text: qsTr("v%1").arg(model.version)
                                    color: Theme.textPrimary
                                    font.pointSize: Theme.fontBody
                                    font.bold: verRow.isCurrent
                                    verticalAlignment: Text.AlignVCenter
                                }
                                StatusBadge {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: verRow.isCurrent
                                    text: qsTr("当前")
                                    tone: "success"
                                }
                            }
                            Label {
                                Layout.preferredWidth: 90
                                text: model.sizeText !== undefined ? String(model.sizeText) : "-"
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                horizontalAlignment: Text.AlignRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.preferredWidth: 150
                                text: model.time !== undefined ? String(model.time) : "-"
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.fillWidth: true
                                // 说明列 = 来源设备 + 备注（服务端 note 取值：
                                // 上传新版本 / 恢复前备份 / 覆盖前备份 / 手动快照 / 当前版本）
                                text: {
                                    const parts = []
                                    if (model.note !== undefined && String(model.note).length > 0)
                                        parts.push(String(model.note))
                                    if (model.device !== undefined && String(model.device).length > 0)
                                        parts.push(String(model.device))
                                    return parts.join(" · ")
                                }
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            RowLayout {
                                Layout.preferredWidth: 132
                                spacing: Theme.spaceXs

                                SecondaryButton {
                                    text: qsTr("回滚")
                                    // 当前版本没有"回滚到自己"的意义 → 禁用
                                    enabled: !verRow.isCurrent && !page.verBusy
                                    onClicked: page.askRestore(model.id, model.version)
                                    ToolTip.visible: hovered && verRow.isCurrent
                                    ToolTip.text: qsTr("这已经是当前版本")
                                }
                                GhostButton {
                                    iconName: "close"
                                    enabled: !verRow.isCurrent && !page.verBusy
                                    onClicked: page.askDelete(model.id, model.version)
                                    ToolTip.visible: hovered
                                    ToolTip.text: verRow.isCurrent
                                                  ? qsTr("当前版本不能删除")
                                                  : qsTr("删除这个历史版本")
                                }
                            }
                        }
                    }
                }
            }

            // 空态放在卡片层（ListView 的兄弟节点），不塞进 ListView 内部 ——
            // ListView 会把非委托子项挪进 contentItem 参与滚动，定位语义不可靠。
            // 只剩合成的那一条「当前版本」时（count<=1）等于"还没有历史"，必须解释原因。
            Label {
                anchors.centerIn: parent
                width: parent.width - Theme.spaceL * 2
                visible: page.verCount <= 1
                text: qsTr("这个文件还没有历史版本。"
                           + "每次覆盖上传新内容、或执行回滚之前，"
                           + "系统都会自动把当时的内容存成一条历史版本。")
                color: Theme.textTertiary
                font.pointSize: Theme.fontSecondary
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    /*!
        确认框（复用 DeleteConfirmDialog）：
        · pendingIsRestore=true  → 回滚（可逆：回滚前会自动备份）
        · pendingIsRestore=false → 删除历史版本（不可逆，红色语气）
        用同一个对话框承载，靠 pendingIsRestore 决定动作。
    */
    DeleteConfirmDialog {
        id: confirmDialog
        onConfirmed: (id) => {
            if (!page.verReady)
                return
            if (page.pendingIsRestore) {
                if (typeof Versions.restore === "function")
                    Versions.restore(String(id))
            } else {
                if (typeof Versions.removeVersion === "function")
                    Versions.removeVersion(String(id))
            }
        }
    }
}
