import QtQuick
import QtQuick.Controls

import "../theme"

/*!
    主操作按钮（实心主色）。对应 UI 方案 §2.2「Primary」——每屏最多 1–2 个。
    例：上传、下载。
    三态：hover / pressed / focus（焦点用 Theme.focusRing 描边）。
*/
Button {
    id: control

    // 可选前置内联字形（文本符号，非位图图标）
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
             : control.down ? Theme.primaryPressed
             : control.hovered ? Theme.primaryHover
             : Theme.primary

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    // 键盘焦点环（键盘可达性）
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
