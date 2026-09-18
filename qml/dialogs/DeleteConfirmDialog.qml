import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    删除确认对话框（危险操作二次确认）。
    契约 §8.1：服务端无回收站，删除 = 真实永久删除，文案必须明确「删除后不可恢复」。
*/
Dialog {
    id: dlg

    property string fileId: ""
    property string fileName: ""
    signal confirmed(string id)

    modal: true
    anchors.centerIn: parent
    width: Math.min(440, (parent ? parent.width - Theme.spaceXl * 2 : 440))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("删除文件")
    standardButtons: Dialog.NoButton

    function openFor(id, name) {
        dlg.fileId = id
        dlg.fileName = (name === undefined || name === null) ? "" : name
        dlg.open()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            Text {
                text: "⚠️"
                font.pointSize: 22
                color: Theme.danger
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("确定要删除「%1」吗？").arg(dlg.fileName)
                color: Theme.textPrimary
                font.pointSize: Theme.fontBody
                font.bold: true
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: warnLabel.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.35)

            Label {
                id: warnLabel
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                text: qsTr("此操作将从服务器真实永久删除该文件，删除后不可恢复"
                           + "（服务端不提供回收站）。")
                color: Theme.danger
                font.pointSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("取消")
                onClicked: dlg.close()
            }
            DangerButton {
                glyph: "🗑"
                text: qsTr("永久删除")
                onClicked: {
                    dlg.confirmed(dlg.fileId)
                    dlg.close()
                }
            }
        }
    }
}
