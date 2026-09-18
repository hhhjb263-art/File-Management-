import QtQuick
import QtQuick.Controls

import "../theme"

/*!
    搜索输入框（带清空按钮）。对应 UI 方案 §2.4 交互目标 6：Ctrl+F 过滤。
    过滤逻辑由使用方负责（FilesPage 会据 textChanged 重建过滤视图）。
*/
TextField {
    id: control

    property string glyph: "🔍"

    implicitHeight: Theme.controlHeight
    leftPadding: glyph.length > 0 ? Theme.spaceM + 18 : Theme.spaceM
    rightPadding: clearButton.visible ? Theme.spaceM + 22 : Theme.spaceM
    font.pointSize: Theme.fontBody
    color: Theme.textPrimary
    placeholderTextColor: Theme.textSecondary
    selectByMouse: true

    Text {
        visible: control.glyph.length > 0
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceM
        anchors.verticalCenter: parent.verticalCenter
        text: control.glyph
        font.pointSize: Theme.fontSecondary
        color: Theme.textSecondary
    }

    ToolButton {
        id: clearButton
        visible: control.text.length > 0
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceS
        anchors.verticalCenter: parent.verticalCenter
        text: "✕"
        font.pointSize: Theme.fontSecondary
        implicitWidth: Theme.spaceL
        implicitHeight: Theme.spaceL
        background: null
        contentItem: Text {
            text: clearButton.text
            font: clearButton.font
            color: Theme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        onClicked: {
            control.text = ""
            control.accepted()
        }
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: Theme.card
        border.width: 1
        border.color: control.activeFocus ? Theme.primary : Theme.border
    }
}
