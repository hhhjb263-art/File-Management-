import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

/*!
    空态 / 引导态组件。对应 UI 方案 §2.4「空态引导」——列出提示 + 主操作按钮，
    取代「一片空白」。也复用于加载态与错误态（错误态通过 actions 提供【重试】）。
*/
Item {
    id: root

    property string glyph: "📭"
    property string title: ""
    property string description: ""
    property color glyphColor: Theme.textSecondary

    // 默认内容区：放置引导按钮（自动横向排布）
    default property alias actions: actionRow.data

    implicitWidth: 320
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.centerIn: parent
        width: Math.min(root.width > 0 ? root.width - Theme.spaceXl * 2 : 320, 420)
        spacing: Theme.spaceM

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root.glyph
            font.pointSize: 34
            color: root.glyphColor
            opacity: 0.85
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
        }

        RowLayout {
            id: actionRow
            Layout.alignment: Qt.AlignHCenter
            spacing: Theme.spaceS
        }
    }
}
