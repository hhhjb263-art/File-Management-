import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    右侧常驻「预览」面板（仅 wide 档显示，宽 Theme.sidePanelWidth）。

    按 `Files.previewState` 分派内容：
      idle        未选中 → 引导文案
      loading     取预览中 → 忙碌指示
      text        文本预览（等宽、只读、可滚动、超长换行不撑破）
      image       图片预览（等比缩放，最大高度受面板约束）
      unsupported 此类型暂不支持预览
      error       以危险色显示 Files.previewError

    ⚠️ 所有 Files.preview* / previewMeta 访问都用 typeof 守卫 —— QML 对 C++ 方法/属性
       是**运行时**解析，C++ 未落地时该面板退化为 idle 空态，绝不抛错/刷屏。
    任何状态都显示元信息区（名称 / 大小 / 时间 / 路径 / 哈希 / 类型）。
*/
Rectangle {
    id: panel

    color: Theme.card

    // 左边界线（与左侧导航栏 / 内容区分隔线呼应；由本组件自绘，调用方只给宽度）
    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: Theme.border
    }

    // ---- 冻结 API 的 typeof 守卫 ----
    readonly property bool apiReady: (typeof Files !== "undefined") && (Files.previewState !== undefined)
    readonly property string pState: apiReady ? String(Files.previewState) : "idle"
    readonly property var pMeta: {
        if (!apiReady)
            return ({})
        const m = Files.previewMeta
        return (m === undefined || m === null) ? ({}) : m
    }
    readonly property string pText: apiReady && Files.previewText !== undefined
                                    ? String(Files.previewText) : ""
    readonly property string pImage: apiReady && Files.previewImageUrl !== undefined
                                     ? String(Files.previewImageUrl) : ""
    readonly property string pError: apiReady && Files.previewError !== undefined
                                     ? String(Files.previewError) : ""

    function metaVal(key, fallback) {
        const v = pMeta[key]
        return (v === undefined || v === null) ? fallback : String(v)
    }

    // 元信息一行：键 + 值（值可换行）
    component MetaRow: RowLayout {
        id: mr

        property string label: ""
        property string value: ""

        Layout.fillWidth: true
        spacing: Theme.spaceS

        Label {
            text: mr.label
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            Layout.preferredWidth: 44
            Layout.alignment: Qt.AlignTop
        }
        Label {
            Layout.fillWidth: true
            text: mr.value.length > 0 ? mr.value : "—"
            color: Theme.textPrimary
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WrapAnywhere
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceM
        spacing: Theme.spaceS

        // ---- 标题 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Label {
                Layout.fillWidth: true
                text: qsTr("预览")
                color: Theme.textPrimary
                font.pointSize: Theme.fontTitle
                font.bold: true
            }
            StatusBadge {
                visible: panel.pState === "loading"
                tone: "info"
                text: qsTr("加载中")
            }
        }

        // ---- 元信息区（任何状态都显示）----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: metaCol.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Theme.bg
            border.width: 1
            border.color: Theme.border

            ColumnLayout {
                id: metaCol
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                spacing: Theme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: panel.pMeta.name !== undefined
                          ? String(panel.pMeta.name)
                          : qsTr("（未选择文件）")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontBody
                    font.bold: true
                    wrapMode: Text.WrapAnywhere
                }

                MetaRow {
                    label: qsTr("大小")
                    value: panel.pMeta.isDir === true
                           ? qsTr("目录")
                           : panel.metaVal("sizeText", "")
                }
                MetaRow {
                    label: qsTr("时间")
                    value: panel.metaVal("timeText", "")
                }
                MetaRow {
                    label: qsTr("路径")
                    value: panel.metaVal("path", "")
                }
                MetaRow {
                    label: qsTr("类型")
                    value: panel.metaVal("kind", "")
                }
                MetaRow {
                    label: qsTr("哈希")
                    visible: panel.metaVal("hash", "").length > 0
                    value: panel.metaVal("hash", "")
                }
            }
        }

        // ---- 内容区（按状态分派）----
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // idle
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width, 260)
                spacing: Theme.spaceS
                visible: panel.pState === "idle"

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "🗂"
                    font.pointSize: 34
                    color: Theme.textSecondary
                    opacity: 0.8
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("在左侧选择一个文件以预览")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }

            // loading
            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.spaceS
                visible: panel.pState === "loading"

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: panel.pState === "loading"
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("正在加载预览…")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                }
            }

            // text
            ScrollView {
                anchors.fill: parent
                visible: panel.pState === "text"
                clip: true

                TextArea {
                    readOnly: true
                    text: panel.pText
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: "monospace"
                    font.pointSize: Theme.fontMono
                    color: Theme.textPrimary
                    selectByMouse: true
                    background: Rectangle {
                        color: Theme.bg
                        border.width: 1
                        border.color: Theme.border
                        radius: Theme.radiusControl
                    }
                }
            }

            // image
            Image {
                anchors.fill: parent
                anchors.bottomMargin: 1
                visible: panel.pState === "image"
                source: panel.pImage
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                sourceSize.width: width
                sourceSize.height: height
            }

            // unsupported
            Label {
                anchors.centerIn: parent
                width: Math.min(parent.width, 260)
                visible: panel.pState === "unsupported"
                text: qsTr("此类型暂不支持预览")
                color: Theme.textSecondary
                font.pointSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            // error
            Label {
                anchors.centerIn: parent
                width: Math.min(parent.width, 260)
                visible: panel.pState === "error"
                text: panel.pError.length > 0 ? panel.pError : qsTr("预览失败")
                color: Theme.danger
                font.pointSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }
}
