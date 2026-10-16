import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    目录选择器：「移动到…」的目标选择（目录树 CRUD）。

    ⚠️ 目录 id 一律用 Files.allFolders() 返回的 id，界面**不做** path↔id 换算。
       两个数据源对"目录 id"的口径本来就不同（HttpBackend 是 <前缀>+<路径>，
       MockBackend 是内部生成 id），换算只能各自在自己的 Backend 里做；
       界面越俎代庖的话，必然只对其中一个后端成立。
*/
Dialog {
    id: dlg

    property string sourceId: ""
    property string sourceName: ""

    signal submit(string id, string targetId)

    modal: true
    anchors.centerIn: parent
    width: Math.min(480, (parent ? parent.width - Theme.spaceXl * 2 : 480))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("移动到…")
    standardButtons: Dialog.NoButton

    function buildAndFilter() {
        folderModel.clear()
        if (typeof Files === "undefined" || typeof Files.allFolders !== "function")
            return
        const f = filterField.text.trim().toLowerCase()
        const list = Files.allFolders()
        for (let i = 0; i < list.length; ++i) {
            const it = list[i]
            const lab = String(it.label)
            if (f === "" || lab.toLowerCase().indexOf(f) >= 0)
                folderModel.append({
                    fid: String(it.id),
                    flabel: lab,
                    fdepth: Number(it.depth)
                })
        }
    }

    function openFor(id, name) {
        dlg.sourceId = String(id)
        dlg.sourceName = (name === undefined || name === null) ? "" : String(name)
        filterField.text = ""
        buildAndFilter()
        dlg.open()
        filterField.forceActiveFocus()
    }

    ListModel { id: folderModel }

    contentItem: ColumnLayout {
        spacing: Theme.spaceS

        Label {
            Layout.fillWidth: true
            text: dlg.sourceName.length > 0
                  ? qsTr("把「%1」移动到：").arg(dlg.sourceName)
                  : qsTr("选择目标文件夹：")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            elide: Text.ElideRight
        }

        TextField {
            id: filterField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            placeholderText: qsTr("过滤文件夹…")
            selectByMouse: true
            color: Theme.textPrimary
            font.pointSize: Theme.fontBody
            onTextChanged: dlg.buildAndFilter()
            background: Rectangle {
                radius: Theme.radiusControl
                color: Theme.surfaceSunken
                border.width: 1
                border.color: filterField.activeFocus ? Theme.primary : Theme.borderStrong
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 240
            radius: Theme.radiusControl
            color: Theme.surfaceSunken
            border.width: 1
            border.color: Theme.border

            ListView {
                id: folderList
                anchors.fill: parent
                anchors.margins: Theme.spaceXs
                clip: true
                model: folderModel
                spacing: 1

                delegate: Rectangle {
                    required property var model
                    width: folderList.width
                    height: Theme.rowHeight
                    radius: Theme.radiusControl
                    color: rowHover.containsMouse ? Theme.hover : "transparent"

                    // ⚠️ 悬停区必须声明在按钮**之前**且不接受点击，否则会吃掉按钮的事件
                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceS + model.fdepth * 12
                        anchors.rightMargin: Theme.spaceS
                        spacing: Theme.spaceS

                        CvIcon {
                            Layout.alignment: Qt.AlignVCenter
                            name: "folder"
                            size: Theme.iconSize
                            color: Theme.textTertiary
                        }
                        Label {
                            Layout.fillWidth: true
                            text: model.flabel
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontBody
                            elide: Text.ElideMiddle
                        }
                        GhostButton {
                            text: qsTr("移到这里")
                            enabled: model.fid.length > 0
                            onClicked: {
                                dlg.submit(dlg.sourceId, model.fid)
                                dlg.close()
                            }
                        }
                    }
                }
            }

            // 空态放在**卡片层**（ListView 的兄弟节点）：塞进 ListView 会被挪进 contentItem 参与滚动
            Label {
                anchors.centerIn: parent
                visible: folderModel.count === 0
                text: qsTr("没有匹配的文件夹")
                color: Theme.textTertiary
                font.pointSize: Theme.fontSecondary
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("取消")
                onClicked: dlg.close()
            }
        }
    }
}
