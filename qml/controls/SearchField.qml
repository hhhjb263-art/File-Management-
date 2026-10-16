import QtQuick
import QtQuick.Controls

import "../theme"
import "."

/*!
    搜索输入框（左侧图标 + 圆角 + 聚焦描边 + 清空按钮）。

    设计规格
      · 常态：surfaceSunken 底 + 1px border —— 读作"可输入的井"，与卡片区分
      · 聚焦：底色升到 card、边框变 primary（**唯一允许改边框的控件**，
        因为输入聚焦必须有明确的空间反馈）
      · 外加 3px 焦点环，供键盘导航识别
      · 过滤逻辑由使用方负责（FilesPage 会据 textChanged 重建过滤视图）

    ⚠️ 兼容性：glyph 属性保留；新增 iconName。
*/
TextField {
    id: control

    property string glyph: "🔍"
    property string iconName: ""

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)

    implicitHeight: Theme.controlHeight
    leftPadding: _hasIcon ? Theme.spaceM + 16 : Theme.spaceM
    rightPadding: clearButton.visible ? Theme.spaceM + 20 : Theme.spaceM
    font.pointSize: Theme.fontBody
    color: Theme.textPrimary
    placeholderTextColor: Theme.textTertiary
    selectByMouse: true
    selectionColor: Theme.primarySoft
    selectedTextColor: Theme.textPrimary

    CvIcon {
        visible: control._hasIcon
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceM
        anchors.verticalCenter: parent.verticalCenter
        name: control._icon
        size: Theme.iconSizeSmall
        color: control.activeFocus ? Theme.primary : Theme.textTertiary

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    ToolButton {
        id: clearButton
        visible: control.text.length > 0
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceS
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: Theme.spaceL
        implicitHeight: Theme.spaceL
        background: null
        contentItem: CvIcon {
            name: "close"
            size: Theme.iconSizeSmall
            color: clearButton.hovered ? Theme.textPrimary : Theme.textTertiary
            anchors.centerIn: parent
        }
        onClicked: {
            control.text = ""
            control.accepted()
        }
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusControl
        color: control.activeFocus ? Theme.card : Theme.surfaceSunken
        border.width: 1
        border.color: control.activeFocus ? Theme.primary : Theme.border

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.animFast } }
    }

    // 聚焦时的外发光焦点环
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.radiusControl + 3
        color: "transparent"
        border.width: 2
        border.color: Theme.focusRing
        visible: control.activeFocus
    }
}
