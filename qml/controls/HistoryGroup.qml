import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    传输历史分组（可折叠）——「已上传」/「已下载」共用控件。

    从原 TransfersPage.qml 的内联 `component HistoryGroup` 抽出，供**传输整页**与
    **传输抽屉**两处复用（避免复制两份配色 / 交互）。

    语义约定（保持既有，勿改坏）：
      · 默认折叠（expanded: false），且**不持久化**（每次重建都重新折叠）。
      · count === 0 时整组隐藏。
      · 头部整行可点（MouseArea 盖满）+ hover 反馈；右侧「清空记录」按钮（清空的是**全部**历史）。
      · 展开体高度由内容决定、并以 maxBodyHeight 为上限（超出则内部滚动）。

    数据来源：TransferModel.uploadHistoryAt(i) / downloadHistoryAt(i)（函数调用，
    QML 无法自动追踪依赖 → 用内部自增的 historyVersion 作为显式依赖）。
*/
ColumnLayout {
    id: group

    property string title: ""
    property bool   upload: true
    property int    count: 0
      property bool   expanded: false           // 默认折叠；不持久化
      property int    maxBodyHeight: 220         // 展开体高度上限（调用方可覆盖）
      // 空历史时是否仍显示分组头（默认 false = 整组隐藏，保持传输整页既有观感）。
      // 传输抽屉里置 true：需求要求「展开后的传输队列包含已下载/已上传两个栏目」，
      // 若空历史就整组消失，首次使用的用户看不到这两栏的存在。
      property bool   alwaysShowHeader: false

    // 历史变化版本号：historyAt() 是函数调用，QML 无法自动追踪其依赖，
    // 用一个自增版本号作为显式依赖，保证历史增删后行内容被重新求值。
    property int historyVersion: 0

    Connections {
        target: TransferModel
        function onHistoryChanged() { group.historyVersion++ }
    }

    // 取历史条目（upload=true → 已上传；false → 已下载）。越界时返回空对象，避免绑定报错。
    function historyAt(isUpload, index) {
        const m = isUpload ? TransferModel.uploadHistoryAt(index)
                           : TransferModel.downloadHistoryAt(index)
        return (m === undefined || m === null) ? ({}) : m
    }

    Layout.fillWidth: true
    spacing: Theme.spaceXs
    visible: group.count > 0 || group.alwaysShowHeader

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
                        ? Math.min(historyColumn.implicitHeight, group.maxBodyHeight)
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
            ScrollBar.vertical.policy: historyColumn.implicitHeight > group.maxBodyHeight
                                       ? ScrollBar.AsNeeded
                                       : ScrollBar.AlwaysOff

            ColumnLayout {
                id: historyColumn
                width: groupScroll.availableWidth
                spacing: Theme.spaceXs

                // 空历史占位（仅当调用方要求 alwaysShowHeader 时才可能看到）
                Label {
                    Layout.fillWidth: true
                    Layout.margins: Theme.spaceS
                    visible: group.count === 0
                    text: qsTr("暂无记录")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    horizontalAlignment: Text.AlignHCenter
                }

                Repeater {
                    model: group.count

                    delegate: Rectangle {
                        id: historyRow

                        // 显式依赖 historyVersion，确保历史变化后行内容刷新
                        readonly property var info: {
                            group.historyVersion
                            return group.historyAt(group.upload, index)
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
                                    // 进度式大小：已传 / 总量（totalText 由 C++ historyMapFor 提供；
                                    // 未就绪时优雅退化为仅显示已传）。不设 elide，保证「总量」那一半不被吃掉。
                                    text: {
                                        const s = historyRow.info.sizeText !== undefined
                                                  ? String(historyRow.info.sizeText) : ""
                                        const t = historyRow.info.totalText !== undefined
                                                  ? String(historyRow.info.totalText) : ""
                                        return t.length > 0 ? s + " / " + t : s
                                    }
                                    Layout.fillWidth: false
                                    color: Theme.textSecondary
                                    font.pointSize: Theme.fontSecondary
                                }

                                StatusBadge {
                                    tone: Theme.toneForState(historyRow.info.state)
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
