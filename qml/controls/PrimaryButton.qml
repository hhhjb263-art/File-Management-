import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    主操作按钮（实心强调色）。每屏最多 1–2 个。
    例：上传、下载。

    设计规格（现代桌面效率风）
      · 高度 32、圆角 6、水平内边距 16
      · 背景三态：默认 primary → hover primaryHover → pressed primaryPressed
      · 禁用态：背景退到 surfaceSunken、文字 disabledText（**不用半透明**，
        半透明在深色底上会糊；实色下沉更干净）
      · 图标与文字间距 8；图标颜色跟随文字色，保证同对比度

    ⚠️ 兼容性：`glyph` 属性保留（旧页面写 emoji），内部由 Icons 单例翻译成矢量图标；
       新代码请优先用 `iconName`。
*/
Button {
    id: control

    // 旧字形（emoji，兼容保留）与新图标名（推荐）。iconName 优先。
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
             : control.down ? Theme.primaryPressed
             : control.hovered ? Theme.primaryHover
             : Theme.primary

        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    // 键盘焦点环（仅在键盘导航时出现 —— 鼠标点击不显示，界面更安静）
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
