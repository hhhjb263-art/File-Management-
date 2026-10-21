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
    // 纯图标（无文字）：收紧内边距，让图标在方按钮里居中
    readonly property bool _iconOnly: _hasIcon && !_hasText
    readonly property color _fg: control.enabled ? Theme.textOnPrimary : Theme.disabledText

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(Theme.controlHeight * 2.5,
                            contentRow.implicitWidth + leftPadding + rightPadding)
    // 16px 的左右内边距会把 30px 宽的纯图标按钮内容宽度压成 0，
    // 图标被顶到右边缘（看起来"贴着右边框"）；纯图标时按 GhostButton 的做法收紧为 8，
    // 内容宽度正好 30-8*2=14=iconSize，图标居中。带文字时保持 16 不变。
    leftPadding: _iconOnly ? Theme.spaceS : Theme.spaceL
    rightPadding: _iconOnly ? Theme.spaceS : Theme.spaceL
    font.pointSize: Theme.fontBody
    font.bold: true

    // ⚠️ 外面包一层 Item 再 anchors.centerIn：
    //    RowLayout 作为 contentItem 时是**左对齐**的 —— 按钮比内容宽时（比如纯图标按钮
    //    宽度 = controlHeight*2.5），图标会被顶到左边、右边空一大块。
    //    包一层 Item + centerIn 后，图标/图文整体在按钮里居中。
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
