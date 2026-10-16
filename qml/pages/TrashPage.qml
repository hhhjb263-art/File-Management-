import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"
import "../dialogs"

/*!
    回收站页：列出已删除的顶层条目（文件 + 文件夹），逐项恢复 / 彻底删除，或整体清空。

    ⚠️ 三条必须先讲清楚的语义（服务端契约，界面不能含糊）：
      1. 「删除」= 移入回收站（软删除），内容与分块引用计数原样保留 —— 所以**恢复是廉价的**。
      2. 口径要分清（`sumSizeOfOwner` 带 `WHERE deleted = 0`）：
         **磁盘**上内容一个字节都没少（blob 未回收），但**「已用空间」里已经扣掉了** ——
         所以进站后设置页的用量数字会当场下降，磁盘占用却纹丝不动。
         页面上必须把这两件事分开说，否则"用量降了但磁盘没释放"会被当成 bug。
      3. 只有「彻底删除 / 清空回收站」才真正回收磁盘，且**不可恢复** —— 一律二次确认。

    ⚠️ 数据源是 TrashModel（FileListModel 的独立实例，角色仍是 id/name/size/time/status/dir），
       与文件页的 FileModel **不是同一个对象**：复用会让两个页面互相冲掉对方的数据。
       其中 dir 角色放「原位置」、status 角色放「文件 / 文件夹」、time 角色放「删除时间」。

    ⚠️ 所有 Trash.* 访问都用 typeof 守卫 —— C++ 未落地时本页退化为占位态，绝不抛错。
*/
Item {
    id: page

    // ---- 冻结 API 守卫 ----
    readonly property bool apiReady: (typeof Trash !== "undefined")
    readonly property bool supported: apiReady
                                      && (Trash.supported === undefined ? true : Trash.supported)
    readonly property bool trashReady: apiReady && (Trash.supported === true)
    readonly property bool modelReady: (typeof TrashModel !== "undefined")

    readonly property bool trashBusy: trashReady && Trash.busy !== undefined ? Trash.busy : false
    readonly property int  trashCount: trashReady && Trash.count !== undefined ? Trash.count : 0
    readonly property string trashUsed: trashReady && Trash.usedText !== undefined
                                        ? String(Trash.usedText) : "-"
    readonly property string trashEmpty: trashReady && Trash.emptyText !== undefined
                                         ? String(Trash.emptyText) : ""

    // 数据变更版本号：列表实体走模型，这里只在需要重算派生量时自增。
    property int trashVersion: 0

    // 页内就近反馈（沿用 SharesPage 的 statusText/statusOk 通道，不新造第二套 toast）
    property string statusText: ""
    property bool   statusOk: true

    function setStatus(text, ok) {
        page.statusText = String(text)
        page.statusOk = (ok !== false)
    }

    function doRefresh() {
        if (page.trashReady && typeof Trash.refresh === "function")
            Trash.refresh()
    }

    function doRestore(id) {
        if (page.trashReady && typeof Trash.restore === "function")
            Trash.restore(String(id))
    }

    // 彻底删除必须带确认 —— 这是本页唯一不可逆的动作
    function askPurge(id, name, isDir) {
        purgeDialog.openAs(String(id), String(name), {
            danger: true,
            titleText: qsTr("彻底删除"),
            confirmText: qsTr("永久删除"),
            noticeText: isDir
                ? qsTr("将永久删除该文件夹及其中的全部文件，同时清掉它们的标签关联与历史版本。"
                       + "此操作不可恢复，空间会立即释放。")
                : qsTr("将永久删除该文件，同时清掉它的标签关联与历史版本。"
                       + "此操作不可恢复，空间会立即释放。")
        })
    }

    function askEmpty() {
        purgeDialog.openAs("", qsTr("回收站中的全部内容"), {
            danger: true,
            titleText: qsTr("清空回收站"),
            confirmText: qsTr("清空回收站"),
            noticeText: qsTr("将永久删除回收站中的全部文件与文件夹（含其中的所有文件），"
                             + "并释放相应空间。此操作不可恢复。")
        })
    }

    // 进页面时刷新一次（软删除发生在文件页，回到本页必须能看到新条目）
    onVisibleChanged: {
        if (page.visible)
            page.doRefresh()
    }

    // 仅在「已就绪」时挂接（未就绪 target=null → 不产生任何信号连接/告警）
    Connections {
        target: page.trashReady ? Trash : null
        function onTrashChanged() { page.trashVersion++ }
        function onErrorOccurred(message) {
            page.setStatus(String(message), false)
        }
        function onStatusMessage(message, ok) {
            if (String(message).length > 0)
                page.setStatus(String(message), ok !== false)
        }
    }

    // ==================================================================
    //  未就绪兜底：原有占位页（保留既有，勿删）
    // ==================================================================
    UnsupportedPage {
        anchors.fill: parent
        visible: !page.supported
        featureName: qsTr("回收站")
        glyph: "🗑"
        notice: (typeof Trash !== "undefined" && Trash.unsupportedNotice !== undefined
                 && String(Trash.unsupportedNotice).length > 0)
                ? Trash.unsupportedNotice
                : qsTr("当前服务端不支持回收站。")
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
                    text: qsTr("回收站")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontTitle + 3
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("删除的文件与文件夹会先来到这里，可以随时恢复到原位置。")
                          + "\n" + qsTr("注意：回收站中的内容不再计入「已用空间」，"
                                        + "但磁盘上尚未释放；"
                                        + "只有「彻底删除」或「清空回收站」才真正回收磁盘空间。")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }

            GhostButton {
                glyph: "⟳"
                text: qsTr("刷新")
                enabled: !page.trashBusy
                onClicked: page.doRefresh()
            }

            DangerButton {
                glyph: "🗑"
                text: qsTr("清空回收站")
                // 空回收站没什么可清：按钮置灰，避免"点了没反应"的困惑
                enabled: page.trashCount > 0 && !page.trashBusy
                onClicked: page.askEmpty()
            }
        }

        // ---- 就近状态（成功 / 失败）----
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

        // ---- 统计条：条目数 + 占用（把"还在占空间"摆在最显眼处）----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: statRow.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusCard
            color: Theme.surfaceSunken
            border.width: 1
            border.color: Theme.border
            visible: page.trashCount > 0

            RowLayout {
                id: statRow
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceL
                anchors.rightMargin: Theme.spaceL
                spacing: Theme.spaceL

                CvIcon {
                    Layout.alignment: Qt.AlignVCenter
                    name: "trash"
                    size: Theme.iconSizeLarge
                    color: Theme.warning
                }
                Label {
                    Layout.fillWidth: true
                    // 这个数字是"回收站条目的逻辑大小之和"，**不是**磁盘实际占用
                    // （分块去重后真实磁盘增量通常小于它）。措辞上不写"占用空间"，
                    // 免得与设置页的用量数字打架。
                    text: qsTr("共 %1 项，合计 %2（未计入「已用空间」）")
                          .arg(page.trashCount).arg(page.trashUsed)
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
                StatusBadge {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("磁盘未释放")
                    tone: "warning"
                    outlined: true
                }
            }
        }

        // ---- 列表面板 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true
            visible: page.trashCount > 0

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // 表头（回收站不提供排序：按服务端返回的删除时间倒序即可，
                // 少一个交互维度，也少一处与文件页不一致的地方）
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
                            // width < 0 表示占满剩余宽度（名称列）
                            model: [
                                { label: qsTr("名称"),     width: -1 },
                                { label: qsTr("原位置"),   width: 160 },
                                { label: qsTr("删除时间"), width: 150 },
                                { label: qsTr("大小"),     width: 90 },
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
                    id: listView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.modelReady ? TrashModel : null
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }

                    delegate: Rectangle {
                        id: rowDelegate

                        // dir 角色在此页语义是「原位置」；status 角色是「文件 / 文件夹」
                        readonly property bool isDir: String(model.status) === qsTr("文件夹")

                        width: ListView.view ? ListView.view.width : 0
                        height: Theme.rowHeight
                        color: rowMouse.containsMouse ? Theme.hover : "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.divider
                            visible: !rowMouse.containsMouse
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            // 行本身不可点：操作只走右侧按钮，避免"点一下就把东西恢复了"这种误触
                            acceptedButtons: Qt.NoButton
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceM
                            anchors.rightMargin: Theme.spaceM
                            spacing: Theme.spaceS

                            CvIcon {
                                Layout.alignment: Qt.AlignVCenter
                                name: rowDelegate.isDir ? "folder" : "file"
                                size: Theme.iconSize
                                color: rowDelegate.isDir ? Theme.warning : Theme.textSecondary
                            }
                            Label {
                                Layout.fillWidth: true
                                text: model.name
                                color: Theme.textPrimary
                                font.pointSize: Theme.fontBody
                                elide: Text.ElideMiddle
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.preferredWidth: 160
                                // 原位置：空串 = 根目录，界面上必须显示成 "/" 而不是空白
                                text: String(model.dir).length > 0 ? "/" + model.dir : "/"
                                color: Theme.textTertiary
                                font.pointSize: Theme.fontSecondary
                                elide: Text.ElideLeft
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.preferredWidth: 150
                                text: model.time
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.preferredWidth: 90
                                text: rowDelegate.isDir ? "-" : model.size
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                horizontalAlignment: Text.AlignRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            RowLayout {
                                Layout.preferredWidth: 132
                                spacing: Theme.spaceXs

                                SecondaryButton {
                                    text: qsTr("恢复")
                                    enabled: !page.trashBusy
                                    onClicked: page.doRestore(model.id)
                                }
                                GhostButton {
                                    iconName: "close"
                                    enabled: !page.trashBusy
                                    onClicked: page.askPurge(model.id, model.name, rowDelegate.isDir)
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("彻底删除（不可恢复）")
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- 空态 ----
        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.trashCount === 0
            iconName: "trash"
            title: qsTr("回收站是空的")
            description: page.trashEmpty.length > 0
                         ? page.trashEmpty
                         : qsTr("删除的文件会先来到这里，可以随时恢复。")
            glyphColor: Theme.textTertiary
        }
    }

    /*!
        确认框（复用 DeleteConfirmDialog 的 danger 语气）。
        空 id = 「清空回收站」；否则 = 「彻底删除某一条」—— 由 onConfirmed 的入参区分。
    */
    DeleteConfirmDialog {
        id: purgeDialog
        onConfirmed: (id) => {
            if (String(id).length === 0) {
                if (page.trashReady && typeof Trash.emptyTrash === "function")
                    Trash.emptyTrash()
            } else {
                if (page.trashReady && typeof Trash.purge === "function")
                    Trash.purge(String(id))
            }
        }
    }
}
