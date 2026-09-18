import QtQuick
import QtQuick.Controls

import "../theme"

/*!
    危险操作按钮（危险色）。对应 UI 方案 §2.2「Danger」。
    例：删除、取消上传。仅出现在确认流程或溢出菜单中。
    三态：hover / pressed / focus（焦点用 Theme.focusRing 描边）。
*/
Button {
    id: control

    property string glyph: ""

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(Theme.controlHeight * 2.5,
                            contentText.implicitWidth + leftPadding + rightPadding)
    leftPadding: Theme.spaceL
    rightPadding: Theme.spaceL
    font.pointSize: Theme.fontBody
    font.bold: true

    contentItem: Text {
        id: contentText
        text: control.glyph.length > 0 ? control.glyph + "  " + control.text : control.text
        font: control.font
        color: control.enabled ? Theme.textOnPrimary : Theme.disabledText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: !control.enabled ? Theme.border
             : control.down ? Theme.dangerPressed
             : control.hovered ? Theme.dangerHover
             : Theme.danger

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.radiusControl + 3
        color: "transparent"
        border.width: 2
        border.color: Theme.focusRing
        visible: control.visualFocus
    }
}
