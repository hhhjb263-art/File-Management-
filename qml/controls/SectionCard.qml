import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

/*!
    分区卡片（圆角 12 + 边框 + 标题层级 + 卡片头下细分割线）。
    用于设置页 / 关于页 / 详情区的内容分组。
    子项默认进入内部列表布局，直接书写即可（如 SectionCard { Label {} }）。
    排版约定：卡片内边距 16（spaceL）、头/体间距 12（spaceM）、体项间距 12。
*/
Rectangle {
    id: card

    property string title: ""
    property string subtitle: ""
    property bool showHeaderDivider: true
    default property alias contentData: body.data

    implicitWidth: 260
    radius: Theme.radiusCard
    color: Theme.card
    border.width: 1
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs
            visible: card.title.length > 0 || card.subtitle.length > 0

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

        // 卡片头下细分割线（仅当有标题/描述时出现）
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: -Theme.spaceXs
            implicitHeight: 1
            color: Theme.border
            visible: card.showHeaderDivider
                     && (card.title.length > 0 || card.subtitle.length > 0)
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceM
        }
    }
}
