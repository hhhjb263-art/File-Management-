import QtQuick

import "../theme"

/*!
    状态徽标（成功 / 警告 / 危险 / 信息 / 中性）。对应 UI 方案 §2.4「结果就近反馈」。
    底色与描边由 tone 派生（基于 Theme 语义色做透明叠加），不引入硬编码色值。
*/
Rectangle {
    id: badge

    property string text: ""
    property string tone: "info" // success | warning | danger | info | neutral
    property bool outlined: false

    readonly property color toneColor: {
        switch (tone) {
        case "success": return Theme.success
        case "warning": return Theme.warning
        case "danger":  return Theme.danger
        case "info":    return Theme.info
        default:        return Theme.textSecondary
        }
    }

    implicitWidth: label.implicitWidth + Theme.spaceM
    implicitHeight: label.implicitHeight + Theme.spaceXs
    radius: Theme.radiusControl
    color: outlined ? "transparent" : Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.14)
    border.width: 1
    border.color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, outlined ? 0.55 : 0.30)

    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        font.pointSize: Theme.fontSecondary
        color: badge.toneColor
    }
}
