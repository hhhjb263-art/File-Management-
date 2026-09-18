import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    证书指纹确认对话框（TOFU，契约 §8.3）。
    · 展示 host:port + 分组指纹(SHA-256) + subject / issuer / 有效期
    · 「信任」→ 记住指纹并放行；「不信任」→ 拒绝本次连接
    · 不提供「忽略并继续」——指纹不一致时客户端不放行（net 层 fail-closed）
*/
Dialog {
    id: dlg

    property string hostPort: ""
    property string fingerprint: ""
    property string subject: ""
    property string issuer: ""
    property string validity: ""
    property bool answered: false

    signal decided(bool accept)

    modal: true
    anchors.centerIn: parent
    width: Math.min(520, (parent ? parent.width - Theme.spaceXl * 2 : 520))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("首次连接该服务器")
    standardButtons: Dialog.NoButton

    function openFor(hp, fp, sub, iss, val) {
        dlg.hostPort = (hp === undefined || hp === null) ? "" : hp
        dlg.fingerprint = (fp === undefined || fp === null) ? "" : fp
        dlg.subject = (sub === undefined || sub === null) ? "" : sub
        dlg.issuer = (iss === undefined || iss === null) ? "" : iss
        dlg.validity = (val === undefined || val === null) ? "" : val
        dlg.answered = false
        dlg.open()
    }

    onClosed: {
        // 未点选即以关闭方式收场 → 按「不信任」处理（fail-closed，未决策即拒绝）
        if (!dlg.answered) {
            dlg.answered = true
            dlg.decided(false)
        }
    }

    function respond(accept) {
        if (dlg.answered)
            return
        dlg.answered = true
        dlg.decided(accept)
        dlg.close()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        Label {
            Layout.fillWidth: true
            text: qsTr("这是首次连接该自签名服务器。请核对下列证书指纹是否与服务器管理员"
                       + "提供的一致，再决定是否信任。")
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.spaceM
            rowSpacing: Theme.spaceXs

            Label { text: qsTr("服务器"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.hostPort
                color: Theme.textPrimary
                font.pointSize: Theme.fontBody
                font.bold: true
                wrapMode: Text.WrapAnywhere
            }

            Label { text: qsTr("使用者"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.subject
                color: Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WrapAnywhere
            }

            Label { text: qsTr("签发者"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.issuer
                color: Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WrapAnywhere
            }

            Label { text: qsTr("有效期"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.validity
                color: Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WrapAnywhere
            }
        }

        Label {
            text: qsTr("证书指纹（SHA-256）")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: fpText.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Theme.bg
            border.width: 1
            border.color: Theme.border

            Label {
                id: fpText
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                text: dlg.fingerprint
                color: Theme.textPrimary
                font.pointSize: Theme.fontMono
                font.family: "monospace"
                wrapMode: Text.WrapAnywhere
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: noteText.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.35)

            Label {
                id: noteText
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                text: qsTr("指纹与本机已固定记录不一致时，客户端不会放行连接——"
                           + "如无法确认来源，请务必选择「不信任」。")
                color: Theme.warning
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("不信任（本次连接失败）")
                onClicked: dlg.respond(false)
            }
            PrimaryButton {
                glyph: "🔒"
                text: qsTr("信任并继续（记住该指纹）")
                onClicked: dlg.respond(true)
            }
        }
    }
}
