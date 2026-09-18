import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    传输队列页：进度（用 progressRatio，0..1）、状态、取消、打开下载目录。
    同名冲突（409）由 Main 的「覆盖询问」对话框统一处理：
    TransferController.conflictDetected → OverwriteDialog → resolveConflict(id, overwrite)。

    语义：TransferModel 现在**只保留未完成任务**（C++ 侧一旦 completed/failed/canceled
    即自动移出活动队列）。历史（已完成/失败/取消）通过 uploadHistoryAt/downloadHistoryAt
    提供，按「已上传 / 已下载」两个**默认折叠、不持久化**的分组展示。
*/
Item {
    id: page

    readonly property int compactWidth: 520
    readonly property bool compact: width < compactWidth

    // 历史变化版本号：historyAt() 是函数调用，QML 无法自动追踪其依赖，
    // 用一个自增版本号作为显式依赖，保证历史增删后行内容被重新求值。
    property int historyVersion: 0

    Connections {
        target: TransferModel
        function onHistoryChanged() { page.historyVersion++ }
    }

    // 展开态历史体的最大高度（随窗口高度自适应，避免撑出页面）
    readonly property int historyMaxHeight: Math.max(120, Math.round(page.height * 0.24))

    function stateTone(token) {
        switch (token) {
        case "completed": return "success"
        case "failed":    return "danger"
        case "canceled":  return "neutral"
        case "paused":    return "warning"
        case "running":   return "info"
        case "hashing":   return "info"
        default:          return "neutral"
        }
    }

    // 取历史条目（upload=true → 已上传；false → 已下载）。越界时返回空对象，避免绑定报错。
    function historyAt(upload, index) {
        const m = upload ? TransferModel.uploadHistoryAt(index)
                         : TransferModel.downloadHistoryAt(index)
        return (m === undefined || m === null) ? ({}) : m
    }

    /*!
        历史分组（内联组件，两处复用）。
        · 默认折叠（expanded: false），且**不持久化** —— 每次进页面都重新折叠。
        · count === 0 时整组隐藏。
        · 头部整行可点（MouseArea 盖满）+ hover 反馈；右侧「清空记录」按钮。
        · 展开体高度由内容决定、并以 historyMaxHeight 为上限（超出则内部滚动），
          保证不压叠、不撑出卡片（参考 SectionCard 的高度由内容决定的约定）。
    */
    component HistoryGroup: ColumnLayout {
        id: group

        property string title: ""
        property bool upload: true
        property int count: 0
        property bool expanded: false // 默认折叠；不持久化（每次进页面都折叠）

        Layout.fillWidth: true
        spacing: Theme.spaceXs
        visible: group.count > 0

        // ---- 头部行 ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: groupHeader.implicitHeight + Theme.spaceS * 2
            radius: Theme.radiusControl
            color: groupMouse.containsMouse ? Theme.hover : Theme.card
            border.width: 1
            border.color: Theme.border
            Behavior on color { ColorAnimation { duration: Theme.animFast } }

            // MouseArea 先声明（位于下层）；按钮在上层 RowLayout 内仍可正常点击，
            // 其余整行区域由本 MouseArea 接收点击 → 展开 / 折叠。
            MouseArea {
                id: groupMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: group.expanded = !group.expanded
            }

            RowLayout {
                id: groupHeader
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceM
                anchors.rightMargin: Theme.spaceS
                spacing: Theme.spaceS

                Text {
                    text: group.expanded ? "▾" : "▸"
                    font.pointSize: Theme.fontBody
                    color: Theme.textSecondary
                }

                Label {
                    Layout.fillWidth: true
                    text: group.title + " (" + group.count + ")"
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontBody
                    font.bold: true
                    elide: Text.ElideRight
                }

                // 注意：clearHistory() 清空的是**全部**历史（含另一组），故文案写「清空记录」。
                GhostButton {
                    glyph: "🗑"
                    text: qsTr("清空记录")
                    onClicked: TransferModel.clearHistory()
                }
            }
        }

        // ---- 展开体 ----
        Rectangle {
            Layout.fillWidth: true
            visible: group.expanded
            implicitHeight: group.expanded
                            ? Math.min(historyColumn.implicitHeight, page.historyMaxHeight)
                              + Theme.spaceS * 2
                            : 0
            radius: Theme.radiusControl
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true

            ScrollView {
                id: groupScroll
                anchors.fill: parent
                anchors.margins: Theme.spaceS
                clip: true
                ScrollBar.vertical.policy: historyColumn.implicitHeight > page.historyMaxHeight
                                           ? ScrollBar.AsNeeded
                                           : ScrollBar.AlwaysOff

                ColumnLayout {
                    id: historyColumn
                    width: groupScroll.availableWidth
                    spacing: Theme.spaceXs

                    Repeater {
                        model: group.count

                        delegate: Rectangle {
                            id: historyRow

                            // 显式依赖 historyVersion，确保历史变化后行内容刷新
                            readonly property var info: {
                                page.historyVersion
                                return page.historyAt(group.upload, index)
                            }

                            Layout.fillWidth: true
                            implicitHeight: rowColumn.implicitHeight + Theme.spaceS * 2
                            radius: Theme.radiusControl
                            color: Theme.bg
                            border.width: 1
                            border.color: Theme.border

                            ColumnLayout {
                                id: rowColumn
                                anchors.fill: parent
                                anchors.margins: Theme.spaceS
                                spacing: Theme.spaceXs

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceS

                                    Text {
                                        text: historyRow.info.kind === "download" ? "⬇" : "⬆"
                                        font.pointSize: Theme.fontBody
                                        color: historyRow.info.kind === "download"
                                               ? Theme.info : Theme.primary
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: historyRow.info.fileName !== undefined
                                              ? String(historyRow.info.fileName) : ""
                                        color: Theme.textPrimary
                                        font.pointSize: Theme.fontBody
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        text: historyRow.info.sizeText !== undefined
                                              ? String(historyRow.info.sizeText) : ""
                                        color: Theme.textSecondary
                                        font.pointSize: Theme.fontSecondary
                                    }

                                    StatusBadge {
                                        tone: page.stateTone(historyRow.info.state)
                                        text: historyRow.info.stateText !== undefined
                                              ? String(historyRow.info.stateText) : ""
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: historyRow.info.displayPath !== undefined
                                          ? String(historyRow.info.displayPath) : ""
                                    color: Theme.textSecondary
                                    font.pointSize: Theme.fontSecondary
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: historyRow.info.state === "failed"
                                             && String(historyRow.info.message).length > 0
                                    text: historyRow.info.message !== undefined
                                          ? String(historyRow.info.message) : ""
                                    color: Theme.danger
                                    font.pointSize: Theme.fontSecondary
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        // ---- 工具条 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Label {
                text: Transfers.statusText
                color: Theme.textPrimary
                font.pointSize: Theme.fontBody
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            StatusBadge {
                visible: Transfers.activeCount > 0
                tone: "info"
                text: qsTr("活动 %1").arg(Transfers.activeCount)
            }

            GhostButton {
                glyph: "📂"
                text: qsTr("打开下载目录")
                onClicked: Transfers.openLocalFolder()
            }

            DangerButton {
                glyph: "✕"
                text: qsTr("取消当前")
                enabled: Transfers.busy
                onClicked: Transfers.cancel()
            }
        }

        // ---- 活动队列卡片（只含未完成任务）----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true

            ListView {
                id: queueView
                anchors.fill: parent
                anchors.margins: Theme.spaceS
                clip: true
                model: TransferModel
                spacing: Theme.spaceS
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }

                delegate: Rectangle {
                    id: itemRow

                    width: ListView.view ? ListView.view.width : 0
                    implicitHeight: contentColumn.implicitHeight + Theme.spaceM * 2
                    radius: Theme.radiusControl
                    color: Theme.bg
                    border.width: 1
                    border.color: Theme.border

                    ColumnLayout {
                        id: contentColumn
                        anchors.fill: parent
                        anchors.margins: Theme.spaceM
                        spacing: Theme.spaceXs

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceS

                            Text {
                                text: model.kind === "download" ? "⬇" : "⬆"
                                font.pointSize: Theme.fontBody
                                color: model.kind === "download" ? Theme.info : Theme.primary
                            }

                            Label {
                                Layout.fillWidth: true
                                text: model.fileName
                                color: Theme.textPrimary
                                font.pointSize: Theme.fontBody
                                elide: Text.ElideRight
                            }

                            StatusBadge {
                                tone: page.stateTone(model.state)
                                text: model.stateText
                            }

                            Label {
                                visible: !page.compact
                                text: model.progress + "%"
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            // 契约 §4.1：ProgressBar.value 用 progressRatio（0.0..1.0）
                            value: model.progressRatio
                            from: 0
                            to: 1
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: String(model.message).length > 0
                            text: model.message
                            color: model.state === "failed" ? Theme.danger : Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                EmptyState {
                    anchors.centerIn: parent
                    width: Math.min(queueView.width, 420)
                    visible: queueView.count === 0
                    glyph: "⇅"
                    title: qsTr("当前没有进行中的传输")
                    description: qsTr("已完成的任务在下方「已上传 / 已下载」中查看。")

                    GhostButton {
                        text: qsTr("打开下载目录")
                        onClicked: Transfers.openLocalFolder()
                    }
                }
            }
        }

        // ---- 历史分组：已上传 / 已下载（默认折叠；count===0 时整组不显示）----
        HistoryGroup {
            title: qsTr("已上传")
            upload: true
            count: TransferModel.uploadHistoryCount
        }

        HistoryGroup {
            title: qsTr("已下载")
            upload: false
            count: TransferModel.downloadHistoryCount
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("提示：同名冲突会弹窗询问「覆盖 / 跳过」；覆盖为服务端原子替换，"
                       + "不会先删后传。失败任务可重新发起。")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }
    }
}
