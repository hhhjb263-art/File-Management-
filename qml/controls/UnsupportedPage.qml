import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

/*!
    未支持功能的置灰占位页。契约 §3「未支持功能的呈现规则」：
    页面形状保留（产品蓝图完整），内容明确说明「服务端暂不支持此功能」。
    无任何可点击即失败的动作。
*/
Item {
    id: page

    property string glyph: "🚧"
    property string featureName: ""
    property string notice: ""

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.spaceXl * 2, 460)
        spacing: Theme.spaceM

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: page.glyph
            font.pointSize: 38
            opacity: 0.7
        }

        Label {
            Layout.fillWidth: true
            text: page.featureName.length > 0
                  ? page.featureName + qsTr(" · 服务端暂不支持此功能")
                  : qsTr("服务端暂不支持此功能")
            color: Theme.textPrimary
            font.pointSize: Theme.fontTitle
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        StatusBadge {
            Layout.alignment: Qt.AlignHCenter
            tone: "neutral"
            text: qsTr("入口保留 · 暂未开放")
        }

        Label {
            Layout.fillWidth: true
            text: page.notice
            visible: page.notice.length > 0
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
