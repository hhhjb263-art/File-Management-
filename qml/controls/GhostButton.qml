import QtQuick
import QtQuick.Controls

import "../theme"

/*!
    辅助 / 图标按钮（无边框，hover 浅底）。对应 UI 方案 §2.2「Ghost/Icon」。
    例：健康检查、存储空间、证书…、清空日志。
    设置 active=true 可呈现「选中态」（用于导航 / 视图切换）。
*/
Button {
    id: control

    property string glyph: ""
    property bool active: false // 选中态（导航 / 切换器使用）

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(Theme.controlHeight,
                            contentText.implicitWidth + leftPadding + rightPadding)
    leftPadding: glyph.length > 0 && text.length > 0 ? Theme.spaceM : Theme.spaceS
    rightPadding: glyph.length > 0 && text.length > 0 ? Theme.spaceM : Theme.spaceS
    font.pointSize: Theme.fontBody

    contentItem: Text {
        id: contentText
        text: control.glyph.length > 0 && control.text.length > 0
              ? control.glyph + "  " + control.text
              : (control.text.length > 0 ? control.text : control.glyph)
        font: control.font
        color: !control.enabled ? Theme.disabledText
             : control.active ? Theme.primary
             : Theme.textPrimary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: control.active ? Theme.selected
             : (control.down || control.hovered) ? Theme.hover
             : "transparent"

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }
}
