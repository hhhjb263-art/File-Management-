import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    传输抽屉内容（默认隐藏，点击工具栏「传输队列」展开）。

    内容 = 进行中队列 + 「已上传」+「已下载」两栏（与传输整页共用 HistoryGroup 控件）。
    宽屏与紧凑模式共用同一个抽屉（Main.qml 中唯一）。

    与 Main 的耦合通过信号解耦（本面板不引用 window / navIndex）：
      · closeRequested()          关闭抽屉
      · openFullPageRequested()   跳转到「传输」整页
*/
Rectangle {
    id: root

    color: Theme.card

    signal closeRequested()
    signal openFullPageRequested()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceM
        spacing: Theme.spaceS

        // ---- 头部 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Label {
                Layout.fillWidth: true
                text: qsTr("传输队列")
                color: Theme.textPrimary
                font.pointSize: Theme.fontTitle
                font.bold: true
                elide: Text.ElideRight
            }
            GhostButton {
                text: qsTr("查看全部")
                onClicked: root.openFullPageRequested()
            }
            GhostButton {
                glyph: "✕"
                onClicked: root.closeRequested()
            }
        }

        Label {
            Layout.fillWidth: true
            text: Transfers.statusText
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            elide: Text.ElideRight
        }

        // ---- 进行中队列（只含未完成任务）----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            radius: Theme.radiusControl
            color: Theme.bg
            border.width: 1
            border.color: Theme.border
            clip: true

            ListView {
                id: activeList
                anchors.fill: parent
                anchors.margins: Theme.spaceS
                clip: true
                model: TransferModel
                spacing: Theme.spaceS
                boundsBehavior: Flickable.StopAtBounds
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
                    visible: activeList.count === 0
                    text: qsTr("暂无传输任务")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                }
            }
        }

        // ---- 历史两栏（默认折叠；抽屉内**空历史也保留栏目头**，见 alwaysShowHeader）----
        HistoryGroup {
            title: qsTr("已上传")
            upload: true
            count: TransferModel.uploadHistoryCount
            maxBodyHeight: 150
            alwaysShowHeader: true
        }
        HistoryGroup {
            title: qsTr("已下载")
            upload: false
            count: TransferModel.downloadHistoryCount
            maxBodyHeight: 150
            alwaysShowHeader: true
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
