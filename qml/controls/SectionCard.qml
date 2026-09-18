import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

/*!
    分区卡片（圆角 + 边框 + 标题）。用于设置页 / 关于页 / 详情区的内容分组。
    子项默认进入内部列表布局，直接书写即可（如 SectionCard { Label {} }）。
*/
Rectangle {
    id: card

    property string title: ""
    property string subtitle: ""
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

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceM
        }
    }
}
