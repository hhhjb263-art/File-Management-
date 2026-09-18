import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    重命名对话框。文件 id 由右键菜单传入。
*/
Dialog {
    id: dlg

    property string fileId: ""
    signal submit(string id, string name)

    modal: true
    anchors.centerIn: parent
    width: Math.min(420, (parent ? parent.width - Theme.spaceXl * 2 : 420))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("重命名")
    standardButtons: Dialog.NoButton

    function openFor(id, currentName) {
        dlg.fileId = id
        nameField.text = (currentName === undefined || currentName === null) ? "" : currentName
        dlg.open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }

    function trySubmit() {
        const n = nameField.text.trim()
        if (n.length === 0)
            return
        dlg.submit(dlg.fileId, n)
        dlg.close()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceS

        Label {
            Layout.fillWidth: true
            text: qsTr("输入新的文件名称（不能包含 '/' 或 '\\'）")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }

        TextField {
            id: nameField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            font.pointSize: Theme.fontBody
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
                text: qsTr("重命名")
                enabled: nameField.text.trim().length > 0
                onClicked: dlg.trySubmit()
            }
        }
    }
}
