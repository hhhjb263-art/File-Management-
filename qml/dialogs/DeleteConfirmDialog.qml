import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    删除确认对话框（危险/重要操作二次确认）。

    ⚠️ 语义已随回收站落地而变更（勿沿用旧文案）：
       此前服务端没有回收站，「删除」= 永久删除，本对话框写着"删除后不可恢复"。
       现在「删除」的默认语义 = **移入回收站**（可恢复），永久删除只从回收站页发起。
       因此本对话框做成可配置的通用确认框，两种用法共用一套外观：
         · 文件页「移入回收站」→ 用默认值（danger=false，按钮为常规主操作）
         · 回收站页「彻底删除 / 清空回收站」→ 传 danger=true + 不可恢复的文案

    可配置项都有合理默认值：调用方只传需要覆盖的部分。
*/
Dialog {
    id: dlg

    property string fileId: ""
    property string fileName: ""
    // ---- 可配置文案与语气 ----
    property string titleText: qsTr("移入回收站")
    // ⚠️ 措辞刻意区分「已用空间」与「磁盘」两件事：服务端 used_bytes 按 deleted=0 统计
    //    （见 server/src/meta/schema.h 顶部说明），进站后用量会立刻下降，但 blob 还在磁盘上。
    //    只说"仍占用空间"会和设置页的用量数字当场矛盾。
    property string noticeText: qsTr("文件会先进入回收站，可以随时恢复到原位置。"
                                     + "注意：回收站中的内容不再计入「已用空间」，"
                                     + "但磁盘上尚未释放 —— 只有「彻底删除」或「清空回收站」"
                                     + "才会真正回收磁盘空间。")
    property string confirmText: qsTr("移入回收站")
    // danger=true → 红色强调 + 警告图标（用于不可恢复的彻底删除）
    property bool danger: false

    signal confirmed(string id)

    modal: true
    anchors.centerIn: parent
    width: Math.min(440, (parent ? parent.width - Theme.spaceXl * 2 : 440))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: dlg.titleText
    standardButtons: Dialog.NoButton

    function openFor(id, name) {
        dlg.fileId = id
        dlg.fileName = (name === undefined || name === null) ? "" : name
        dlg.open()
    }

    /*!
        一次性配置 + 打开，供"同一个对话框承担多种确认"的调用方使用
        （避免调用方分三段先赋值再 open，容易漏字段）。
        opts: { danger, titleText, noticeText, confirmText }
    */
    function openAs(id, name, opts) {
        const o = opts || ({})
        dlg.danger = (o.danger === true)
        dlg.titleText = o.titleText !== undefined ? String(o.titleText) : qsTr("移入回收站")
        dlg.noticeText = o.noticeText !== undefined
                        ? String(o.noticeText)
                        : qsTr("文件会先进入回收站，可以随时恢复到原位置。"
                               + "注意：回收站中的内容不再计入「已用空间」，"
                               + "但磁盘上尚未释放 —— 只有「彻底删除」或「清空回收站」"
                               + "才会真正回收磁盘空间。")
        dlg.confirmText = o.confirmText !== undefined ? String(o.confirmText) : qsTr("移入回收站")
        dlg.openFor(id, name)
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            CvIcon {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 2
                name: dlg.danger ? "warning" : "info"
                size: 22
                color: dlg.danger ? Theme.danger : Theme.info
                strokeWidth: 1.8
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("确定要删除「%1」吗？").arg(dlg.fileName)
                color: Theme.textPrimary
                font.pointSize: Theme.fontBody
                font.bold: true
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: warnLabel.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            // 语气色随 danger 切换：常态信息蓝、危险红（soft 令牌已按主题调过，
            // 深色底上不会发光）。
            color: dlg.danger ? Theme.dangerSoft : Theme.infoSoft
            border.width: 1
            border.color: dlg.danger
                          ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.35)
                          : Qt.rgba(Theme.info.r, Theme.info.g, Theme.info.b, 0.30)

            Label {
                id: warnLabel
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                text: dlg.noticeText
                color: dlg.danger ? Theme.danger : Theme.info
                font.pointSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("取消")
                onClicked: dlg.close()
            }
            // 两种语气用不同控件：危险态必须是有独立配色的 DangerButton，
            // 否则"彻底删除"与"移入回收站"在视觉上长得一样，用户不会多看一眼。
            DangerButton {
                visible: dlg.danger
                glyph: "🗑"
                text: dlg.confirmText
                onClicked: {
                    dlg.confirmed(dlg.fileId)
                    dlg.close()
                }
            }
            PrimaryButton {
                visible: !dlg.danger
                iconName: "trash"
                text: dlg.confirmText
                onClicked: {
                    dlg.confirmed(dlg.fileId)
                    dlg.close()
                }
            }
        }
    }
}
