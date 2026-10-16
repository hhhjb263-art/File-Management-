import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    空态 / 引导态组件 —— 提示 + 主操作，取代「一片空白」。
    也复用于加载态与错误态（错误态通过 actions 提供【重试】）。

    设计规格
      · 图标装在 64×64 的圆形"井"里（surfaceSunken 底 + 极细描边），
        比裸放一个 40pt 的 emoji 更有结构感，也让空态在深色主题下不"发飘"
      · 图标颜色由 glyphColor 控制；圆形底色自动取该色的 8% 叠加 ——
        于是错误态（glyphColor: Theme.danger）会自动得到淡红圆底，无需额外配置
      · 标题 16 加粗、描述 13 次要色，正文宽度上限 420 便于阅读

    ⚠️ 兼容性：glyph / title / description / glyphColor / actions 均为既有属性，全部保留。
*/
Item {
    id: root

    property string glyph: "📭"
    property string iconName: ""
    property string title: ""
    property string description: ""
    property color glyphColor: Theme.textTertiary

    // 默认内容区：放置引导按钮（自动横向排布）
    default property alias actions: actionRow.data

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)

    implicitWidth: 320
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.centerIn: parent
        width: Math.min(root.width > 0 ? root.width - Theme.spaceXxl * 2 : 320, 420)
        spacing: Theme.spaceM

        // 圆形图标井
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            visible: root._hasIcon
            implicitWidth: 64
            implicitHeight: 64
            radius: width / 2
            color: Theme.softOf(root.glyphColor, 0.08)
            border.width: 1
            border.color: Theme.softOf(root.glyphColor, 0.16)

            CvIcon {
                anchors.centerIn: parent
                name: root._icon
                size: 26
                color: root.glyphColor
                strokeWidth: 1.6
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.title.length > 0
            text: root.title
            color: Theme.textPrimary
            font.pointSize: Theme.fontTitle
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            visible: root.description.length > 0
            text: root.description
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.5
        }

        RowLayout {
            id: actionRow
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceS
        }
    }
}
