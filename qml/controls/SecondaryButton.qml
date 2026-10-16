import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    次级按钮（描边 + 透明底）。例：刷新、新建文件夹、测试连接。

    设计规格
      · 边框 1px borderStrong 恒定不变，**交互只改背景**（hover / hoverStrong）——
        这是效率风的核心手法：边框不动，界面不"抖"
      · 禁用态：背景 bg、边框 disabledText 降为 1px 实色（不再靠透明度）
*/
Button {
    id: control

    property string glyph: ""
    property string iconName: ""

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)
    readonly property bool _hasText: text.length > 0
    readonly property color _fg: control.enabled ? Theme.textPrimary : Theme.disabledText

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(Theme.controlHeight * 2.5,
                            contentRow.implicitWidth + leftPadding + rightPadding)
    leftPadding: Theme.spaceL
    rightPadding: Theme.spaceL
    font.pointSize: Theme.fontBody

    contentItem: RowLayout {
        id: contentRow
        spacing: Theme.spaceS

        CvIcon {
            visible: control._hasIcon
            Layout.alignment: Qt.AlignVCenter
            name: control._icon
            size: Theme.iconSize
            color: control.enabled ? Theme.textSecondary : Theme.disabledText
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

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: !control.enabled ? Theme.bg
             : control.down ? Theme.hoverStrong
             : control.hovered ? Theme.hover
             : "transparent"
        border.width: 1
        border.color: control.enabled ? Theme.borderStrong : Theme.disabledText

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
