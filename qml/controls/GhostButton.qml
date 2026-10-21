import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    辅助 / 图标按钮（无边框，hover 浅底）。例：健康检查、存储空间、证书…。

    active = true 呈现「选中态」（导航 / 视图切换）：浅强调底 + 强调色文字。

    设计规格
      · 无边框；交互靠背景：hover / hoverStrong / active(primarySoft)
      · 内边距随「是否有图标+文字」自适应：纯图标时收紧为 8，避免"胖图标按钮"
      · 图标颜色跟随文字色（选中态变强调色，视觉上"亮起来"）
*/
Button {
    id: control

    property string glyph: ""
    property string iconName: ""
    property bool active: false // 选中态（导航 / 切换器使用）

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)
    readonly property bool _hasText: text.length > 0
    readonly property color _fg: !control.enabled ? Theme.disabledText
                               : control.active ? Theme.primary
                               : Theme.textPrimary

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(Theme.controlHeight,
                            contentRow.implicitWidth + leftPadding + rightPadding)
    leftPadding: _hasIcon && _hasText ? Theme.spaceM : Theme.spaceS
    rightPadding: _hasIcon && _hasText ? Theme.spaceM : Theme.spaceS
    font.pointSize: Theme.fontBody

    // 与 DangerButton 一致：包一层 Item 让内容**居中**。
    // （RowLayout 直接作 contentItem 时是左对齐的，纯图标按钮会被顶到左边、右边空一块。）
    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        RowLayout {
            id: contentRow
            anchors.centerIn: parent
            spacing: Theme.spaceS

            CvIcon {
                visible: control._hasIcon
                Layout.alignment: Qt.AlignVCenter
                name: control._icon
                size: Theme.iconSize
                color: control._fg
            }

            Label {
                visible: control._hasText
                text: control.text
                font: control.font
                color: control._fg
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: control.active ? Theme.primarySoft
             : control.down ? Theme.hoverStrong
             : control.hovered ? Theme.hover
             : "transparent"

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
