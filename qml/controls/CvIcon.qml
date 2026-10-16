import QtQuick
import QtQuick.Shapes

import "../theme"

/*!
    矢量图标（线性描边，24×24 画布，等比缩放到 size）。

    用法：
        CvIcon { name: "upload"; size: Theme.iconSize; color: Theme.textPrimary }
        CvIcon { glyph: "⬆" }            // 旧 emoji 也会被自动翻译（见 Icons 单例）

    设计要点：
      · 颜色**永远由调用方给出**（默认 Theme.textSecondary），因此天然跟随主题，
        不需要为深浅两套主题各准备一套图标资源。
      · 未登记的图标名 → 不绘制任何东西（path 为空串），**不显示方框、不报错**。
      · 用 QtQuick.Shapes 渲染，无位图资源、无 QtSvg 依赖，任意 DPI 下都锐利。
      · 描边宽度在 24 画布内固定 1.75，随 item 缩放而等比变化 —— 16px 下约 1.17px，
        视觉重量与正文一致，不会"糊成一坨"。
*/
Item {
    id: root

    // 图标名（优先）。为空时回落到 glyph 翻译。
    property string name: ""
    // 旧 emoji 字形（兼容旧调用方；会被自动翻译成图标名）
    property string glyph: ""
    property int size: Theme.iconSize
    property color color: Theme.textSecondary
    // 描边粗细（24 画布内）；填充类图标忽略此值
    property real strokeWidth: 1.75

    // 实际使用的图标名：name 优先，否则翻译 glyph
    readonly property string resolvedName: name.length > 0 ? name : Icons.nameForGlyph(glyph)
    readonly property bool _filled: Icons.isFilled(resolvedName)
    readonly property string _path: Icons.pathFor(resolvedName)
    readonly property real _scale: size / 24

    implicitWidth: size
    implicitHeight: size
    visible: _path.length > 0

    Shape {
        // 画布固定 24×24，再整体缩放 —— 描边随之一并等比缩放
        width: 24
        height: 24
        transformOrigin: Item.TopLeft
        scale: root._scale
        antialiasing: true
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: root._filled ? "transparent" : root.color
            strokeWidth: root._filled ? 0 : root.strokeWidth
            fillColor: root._filled ? root.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root._path }
        }
    }
}
