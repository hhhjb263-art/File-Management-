import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    分区卡片（圆角 10 + 1px 极细描边 + 标题层级 + 头下细分割线）。
    用于设置页 / 关于页 / 详情区的内容分组。

    子项默认进入内部列表布局，直接书写即可（如 SectionCard { Label {} }）。

    设计规格
      · 内边距 16（spaceL）、头/体间距 12（spaceM）、体项间距 12
      · 头下分割线用 Theme.divider（比边框更淡）—— 卡片内部分隔不应抢视觉
      · 可选头部图标（glyph / iconName），置于标题左侧，颜色次要

    ⚠️ 兼容性：`title / subtitle / showHeaderDivider / contentData` 均为既有属性，
       一个都没改。仅**新增** glyph / iconName。
    ⚠️ 高度必须由内容决定：若只给 implicitWidth 而不给 implicitHeight，
       内部 anchors.fill 的布局会被压进不确定高度 —— 表现就是卡片内各选项重叠
       （此前的真实缺陷根因，勿删这两行）。
*/
Rectangle {
    id: card

    property string title: ""
    property string subtitle: ""
    property bool showHeaderDivider: true
    property string glyph: ""
    property string iconName: ""
    default property alias contentData: body.data

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)
    readonly property bool _hasHeader: card.title.length > 0 || card.subtitle.length > 0

    implicitWidth: rootLayout.implicitWidth + Theme.spaceL * 2
    implicitHeight: rootLayout.implicitHeight + Theme.spaceL * 2
    radius: Theme.radiusCard
    color: Theme.card
    border.width: 1
    border.color: Theme.border

    ColumnLayout {
        id: rootLayout
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            visible: card._hasHeader

            CvIcon {
                visible: card._hasIcon
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 2
                name: card._icon
                size: Theme.iconSize
                color: Theme.textSecondary
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs

                Label {
                    Layout.fillWidth: true
                    visible: card.title.length > 0
                    text: card.title
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontTitle
                    font.bold: true
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: card.subtitle.length > 0
                    text: card.subtitle
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }
        }

        // 卡片头下细分割线（仅当有标题/描述时出现）
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: -Theme.spaceXs
            implicitHeight: 1
            color: Theme.divider
            visible: card.showHeaderDivider && card._hasHeader
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: Theme.spaceM
        }
    }
}
