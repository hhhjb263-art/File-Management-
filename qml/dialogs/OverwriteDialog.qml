import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    覆盖询问对话框（上传同名冲突 409）。
    契约 §8.2 / §8.4：覆盖为服务端原子替换，绝不「先删后传」。
    关闭（Esc / 点 X）按「跳过」处理（安全默认）。
*/
Dialog {
    id: dlg

    property string taskId: ""
    property string targetDir: ""
    property string targetName: ""
    property bool answered: false
    signal decided(string taskId, bool overwrite)

    modal: true
    anchors.centerIn: parent
    width: Math.min(440, (parent ? parent.width - Theme.spaceXl * 2 : 440))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("同名文件冲突")
    standardButtons: Dialog.NoButton

    function openFor(id, dir, name) {
        dlg.taskId = id
        dlg.targetDir = (dir === undefined || dir === null) ? "" : dir
        dlg.targetName = (name === undefined || name === null) ? "" : name
        dlg.answered = false
        dlg.open()
    }

    function respond(overwrite) {
        if (dlg.answered)
            return
        dlg.answered = true
        dlg.decided(dlg.taskId, overwrite)
        dlg.close()
    }

    onClosed: {
        if (!dlg.answered)
            dlg.respond(false) // 未明确选择 → 跳过（不改动服务器文件）
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        Label {
            Layout.fillWidth: true
            text: qsTr("目标目录下已存在同名文件：")
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
        }

        Label {
            Layout.fillWidth: true
            text: (dlg.targetDir.length > 0 ? dlg.targetDir + "/" : qsTr("根目录 / "))
                  + dlg.targetName
            color: Theme.textPrimary
            font.pointSize: Theme.fontBody
            font.bold: true
            wrapMode: Text.WrapAnywhere
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("「覆盖」将由服务器原子替换该文件内容（失败时原文件保持不变）；"
                       + "「跳过」则不上传此文件。")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("跳过")
                onClicked: dlg.respond(false)
            }
            DangerButton {
                glyph: "⟳"
                text: qsTr("覆盖")
                onClicked: dlg.respond(true)
            }
        }
    }
}
