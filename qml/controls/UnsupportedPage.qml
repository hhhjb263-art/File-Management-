import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "."

/*!
    未支持功能的置灰占位页。契约 §3「未支持功能的呈现规则」：
    页面形状保留（产品蓝图完整），内容明确说明「服务端暂不支持此功能」。
    无任何可点击即失败的动作。

    设计规格
      · 与 EmptyState 同构（圆形图标井 + 标题 + 徽标 + 说明），保证"空态家族"观感一致
      · 图标井用中性色，不报警 —— 这是"尚未开放"，不是"出错了"

    ⚠️ 兼容性：glyph / featureName / notice 均为既有属性，全部保留。
*/
Item {
    id: page

    property string glyph: "🚧"
    property string iconName: ""
    property string featureName: ""
    property string notice: ""

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.spaceXl * 2, 460)
        spacing: Theme.spaceM

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 64
            implicitHeight: 64
            radius: width / 2
            color: Theme.surfaceSunken
            border.width: 1
            border.color: Theme.border

            CvIcon {
                anchors.centerIn: parent
                name: page._icon.length > 0 ? page._icon : "warning"
                size: 26
                color: Theme.textTertiary
                strokeWidth: 1.6
            }
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
            lineHeight: 1.5
        }
    }
}
