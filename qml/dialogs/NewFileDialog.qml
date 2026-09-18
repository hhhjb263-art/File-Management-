import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    新建对话框（文件 / 文件夹共用；kind = "file" | "folder"）。
    服务端无目录 id 体系，createFolder 接收相对路径（支持 "a/b" 形式）。
*/
Dialog {
    id: dlg

    property string kind: "file"
    signal submit(string name, string kind)

    modal: true
    anchors.centerIn: parent
    width: Math.min(420, (parent ? parent.width - Theme.spaceXl * 2 : 420))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: dlg.kind === "folder" ? qsTr("新建文件夹") : qsTr("新建文件")
    standardButtons: Dialog.NoButton

    function openFor(k, initial) {
        dlg.kind = (k === "folder") ? "folder" : "file"
        nameField.text = (initial === undefined || initial === null) ? "" : initial
        dlg.open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }

    function trySubmit() {
        const n = nameField.text.trim()
        if (n.length === 0)
            return
        dlg.submit(n, dlg.kind)
        dlg.close()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceS

        Label {
            Layout.fillWidth: true
            text: dlg.kind === "folder"
                  ? qsTr("文件夹名称（支持 a/b 形式的多级路径）")
                  : qsTr("文件名（例如 note.txt）")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }

        TextField {
            id: nameField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            font.pointSize: Theme.fontBody
            placeholderText: dlg.kind === "folder" ? qsTr("新建文件夹") : qsTr("新建文件.txt")
            onAccepted: dlg.trySubmit()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("取消")
                onClicked: dlg.close()
            }
            PrimaryButton {
                text: qsTr("确定")
                enabled: nameField.text.trim().length > 0
                onClicked: dlg.trySubmit()
            }
        }
    }
}
