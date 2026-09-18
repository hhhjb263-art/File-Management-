import QtQuick
import QtQuick.Controls

import "../theme"

/*!
    次级按钮（描边 / 浅底）。对应 UI 方案 §2.2「Secondary」。
    例：刷新、新建文件夹。
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

    contentItem: Text {
        id: contentText
        text: control.glyph.length > 0 ? control.glyph + "  " + control.text : control.text
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.disabledText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: !control.enabled ? Theme.bg
             : control.down ? Theme.hover
             : control.hovered ? Theme.hover
             : "transparent"
        border.width: 1
        border.color: control.enabled ? Theme.border : Theme.disabledText

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }
}
