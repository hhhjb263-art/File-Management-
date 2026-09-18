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
    提供，按「已上传 / 已下载」两个**默认折叠、不持久化**的分组展示；
    分组控件已抽成 `controls/HistoryGroup.qml`，与传输抽屉共用（不再内联复制）。
*/
Item {
    id: page

    readonly property int compactWidth: 520
    readonly property bool compact: width < compactWidth

    // 展开态历史体的最大高度（随窗口高度自适应，避免撑出页面）
    readonly property int historyMaxHeight: Math.max(120, Math.round(page.height * 0.24))

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
                                tone: Theme.toneForState(model.state)
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
            maxBodyHeight: page.historyMaxHeight
        }

        HistoryGroup {
            title: qsTr("已下载")
            upload: false
            count: TransferModel.downloadHistoryCount
            maxBodyHeight: page.historyMaxHeight
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
