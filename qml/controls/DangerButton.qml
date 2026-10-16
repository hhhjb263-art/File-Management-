import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    危险操作按钮（实心危险色）。例：删除、取消上传。
    仅出现在确认流程或溢出菜单中 —— 不进常规工具栏。

    设计规格
      · 背景三态：danger → dangerHover → dangerPressed
      · 文字/图标用 textOnPrimary（浅色主题为白、深色主题为近黑）——
        深色主题下 danger 是浅红，压白字会低于 AA，压近黑字才达标。
      · 禁用态与主按钮一致（surfaceSunken + disabledText）
*/
Button {
    id: control

    property string glyph: ""
    property string iconName: ""

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)
    readonly property bool _hasText: text.length > 0
    readonly property color _fg: control.enabled ? Theme.textOnPrimary : Theme.disabledText

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(Theme.controlHeight * 2.5,
                            contentRow.implicitWidth + leftPadding + rightPadding)
    leftPadding: Theme.spaceL
    rightPadding: Theme.spaceL
    font.pointSize: Theme.fontBody
    font.bold: true

    contentItem: RowLayout {
        id: contentRow
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

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: !control.enabled ? Theme.surfaceSunken
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
