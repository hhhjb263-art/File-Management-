import QtQuick
import QtQuick.Layouts

import "../theme"
import "."

/*!
    状态徽标（成功 / 警告 / 危险 / 信息 / 中性）。用于「结果就近反馈」。

    设计规格
      · 药丸形（radius = height/2），高度 = 文字高 + 6，横向内边距 10
      · 底色取**语义色的 soft 令牌**（successSoft / warningSoft / dangerSoft /
        infoSoft），不再现场算透明度 —— 深浅两套主题的 soft 值是各自调过的，
        在深色底上不会出现"发光感"；中性色用 surfaceSunken
      · 文字用语义色本体（浅色主题均为 AA 达标值）
      · outlined = true 时改为透明底 + 语义色描边（用于强调单个徽标）
      · 可选前置小图标（glyph / iconName），颜色与文字一致
*/
Rectangle {
    id: badge

    property string text: ""
    property string tone: "info" // success | warning | danger | info | neutral
    property bool outlined: false
    property string glyph: ""
    property string iconName: ""

    readonly property string _icon: iconName.length > 0 ? iconName : Icons.nameForGlyph(glyph)
    readonly property bool _hasIcon: _icon.length > 0 && Icons.has(_icon)

    readonly property color toneColor: {
        switch (tone) {
        case "success": return Theme.success
        case "warning": return Theme.warning
        case "danger":  return Theme.danger
        case "info":    return Theme.info
        default:        return Theme.textSecondary
        }
    }

    readonly property color toneSoft: {
        switch (tone) {
        case "success": return Theme.successSoft
        case "warning": return Theme.warningSoft
        case "danger":  return Theme.dangerSoft
        case "info":    return Theme.infoSoft
        default:        return Theme.surfaceSunken
        }
    }

    implicitWidth: contentRow.implicitWidth + Theme.spaceS * 2 + Theme.spaceXs / 2
    implicitHeight: contentRow.implicitHeight + Theme.spaceXs + 2
    radius: height / 2
    color: outlined ? "transparent" : toneSoft
    border.width: 1
    border.color: outlined ? Theme.softOf(toneColor, 0.55)
                           : Theme.softOf(toneColor, 0.22)

    RowLayout {
        id: contentRow
        anchors.centerIn: parent
        spacing: Theme.spaceXs

        CvIcon {
            visible: badge._hasIcon
            Layout.alignment: Qt.AlignVCenter
            name: badge._icon
            size: Theme.iconSizeSmall
            color: badge.toneColor
        }

        Text {
            id: label
            text: badge.text
            font.pointSize: Theme.fontSecondary
            color: badge.toneColor
            verticalAlignment: Text.AlignVCenter
        }
    }
}
