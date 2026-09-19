import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    服务器证书指纹「不匹配」对话框（连接已被拒绝后的恢复路径）。

    触发：`App.certPinMismatch(hostPort, expected, actual, subject, issuer, validity)`
      expected = 已固定记录（旧），actual = 本次收到的（新）

    背景（契约 §8.3 / net 层 fail-closed）：
      指纹与已固定记录不一致时，`HttpBackend` **不放行**（不 ignoreSslErrors）且直接返回，
      且该 return 在弹确认框之前 ⇒ 不会再走 TOFU 确认框，重连多少次都一样、界面此前毫无提示。
      用户此前**唯一**的恢复路径是「设置页 → 清除该主机信任」，但失败那一刻无人告知。

    本对话框给出明确的**判断依据（新旧指纹对照）**与**恢复路径**，并且：
      · 绝不自动信任；清除指纹**只能**由用户显式点击触发。
      · 「取消（保持拒绝）」为推荐的安全动作。
      · 状态反馈复用本对话框内的就地提示（不新造第二套 toast），清除成功后由 Main 走既有 toast 通道。
*/
Dialog {
    id: dlg

    // ---- 由 Main 注入 ----
    property string hostPort: ""
    property string expectedFingerprint: ""   // 已固定（旧）
    property string actualFingerprint: ""     // 本次收到（新）
    property string subject: ""
    property string issuer: ""
    property string validity: ""

    // 就地状态反馈
    property string statusText: ""
    property bool   statusOk: true

    // 由 Main 代为接线（本对话框不直接引用 filesPage / 导航）
    signal clearAndRetryRequested()   // 「清除记录并重新信任」：Main 负责 toast 提示 + 触发重试
    signal openSettingsRequested()    // 「去设置页查看」：Main 负责跳转

    modal: true
    anchors.centerIn: parent
    width: Math.min(560, (parent ? parent.width - Theme.spaceXl * 2 : 560))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("服务器证书已变更")
    standardButtons: Dialog.NoButton

    function openFor(hp, expected, actual, sub, iss, val) {
        dlg.hostPort = String(hp === undefined || hp === null ? "" : hp)
        dlg.expectedFingerprint = String(expected === undefined || expected === null ? "" : expected)
        dlg.actualFingerprint = String(actual === undefined || actual === null ? "" : actual)
        dlg.subject = String(sub === undefined || sub === null ? "" : sub)
        dlg.issuer = String(iss === undefined || iss === null ? "" : iss)
        dlg.validity = String(val === undefined || val === null ? "" : val)
        dlg.statusText = ""
        dlg.statusOk = true
        dlg.open()
    }

    // 复制某个 TextArea 的全部内容到系统剪贴板（纯 QML：selectAll + copy + deselect）
    function copyField(field) {
        if (!field)
            return
        field.selectAll()
        field.copy()
        field.deselect()
        dlg.statusOk = true
        dlg.statusText = qsTr("指纹已复制到剪贴板。")
    }

    // 「清除记录并重新信任」：显式、用户触发的恢复动作；绝不自动放行。
    function clearAndRetrust() {
        if (typeof App === "undefined" || typeof App.clearPinnedFingerprint !== "function") {
            dlg.statusOk = false
            dlg.statusText = qsTr("当前版本不支持一键清除，请到「设置 → 自签名证书与信任」手动清除指纹后重试。")
            return
        }
        App.clearPinnedFingerprint()
        dlg.close()
        dlg.clearAndRetryRequested()
    }

    // 可复用的「指纹对照块」：标题 + 复制按钮 + 可换行完整显示的等宽文本
    component FingerprintBlock: ColumnLayout {
        id: fpb

        property string heading: ""
        property string fp: ""
        property bool   alert: false   // 新版用危险色描边强调
        signal copyAsked(var field)

        Layout.fillWidth: true
        spacing: Theme.spaceXs

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            Label {
                Layout.fillWidth: true
                text: fpb.heading
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
                font.bold: true
            }
            GhostButton {
                glyph: "⧉"
                text: qsTr("复制")
                onClicked: fpb.copyAsked(fpbArea)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(Theme.controlHeight, fpbArea.implicitHeight) + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Theme.bg
            border.width: 1
            border.color: fpb.alert
                          ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.55)
                          : Theme.border

            TextArea {
                id: fpbArea
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                text: fpb.fp.length > 0 ? fpb.fp : qsTr("（无）")
                color: Theme.textPrimary
                font.pointSize: Theme.fontMono
                font.family: "monospace"
                background: null
                topPadding: 0
                bottomPadding: 0
                leftPadding: 0
                rightPadding: 0
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        // ---- 主提示：连接已被拒绝 ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headLabel.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.35)

            Label {
                id: headLabel
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                text: qsTr("已拒绝连接：%1 返回的证书指纹与本机已固定的记录不一致。"
                           + "为防止中间人攻击，本次连接已被拒绝，且不会自动重试。")
                      .arg(dlg.hostPort)
                color: Theme.danger
                font.pointSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("请核对下列两个指纹：")
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
        }

        // ---- 新旧指纹对照 ----
        FingerprintBlock {
            heading: qsTr("已固定（旧）")
            fp: dlg.expectedFingerprint
            onCopyAsked: (field) => dlg.copyField(field)
        }
        FingerprintBlock {
            heading: qsTr("本次收到（新）")
            fp: dlg.actualFingerprint
            alert: true
            onCopyAsked: (field) => dlg.copyField(field)
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("可与服务端证书比对（SHA-256）：")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
        }
        Label {
            Layout.fillWidth: true
            text: "openssl x509 -in <证书> -noout -fingerprint -sha256"
            color: Theme.textPrimary
            font.pointSize: Theme.fontMono
            font.family: "monospace"
            wrapMode: Text.WrapAnywhere
        }

        // ---- 证书信息 ----
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.spaceM
            rowSpacing: Theme.spaceXs

            Label { text: qsTr("使用者"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.subject.length > 0 ? dlg.subject : "—"
                color: Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WrapAnywhere
            }
            Label { text: qsTr("签发者"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.issuer.length > 0 ? dlg.issuer : "—"
                color: Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WrapAnywhere
            }
            Label { text: qsTr("有效期"); color: Theme.textSecondary; font.pointSize: Theme.fontBody }
            Label {
                Layout.fillWidth: true
                text: dlg.validity.length > 0 ? dlg.validity : "—"
                color: Theme.textPrimary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WrapAnywhere
            }
        }

        // ---- 安全说明（不淡化）----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: secLabel.implicitHeight + Theme.spaceM * 2
            radius: Theme.radiusControl
            color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.35)

            Label {
                id: secLabel
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                text: qsTr("如果你最近更换过服务端证书，这属于正常情况，可在确认新指纹无误后清除记录并重新信任。\n"
                           + "否则，这可能是一次中间人攻击——请勿在不可信网络中信任该证书。不确定时请选择「取消（保持拒绝）」。")
                color: Theme.warning
                font.pointSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
        }

        // ---- 就地状态反馈 ----
        Label {
            Layout.fillWidth: true
            visible: dlg.statusText.length > 0
            text: dlg.statusText
            color: dlg.statusOk ? Theme.success : Theme.danger
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }

        // ---- 动作 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            GhostButton {
                glyph: "⚙"
                text: qsTr("去设置页查看指纹")
                onClicked: {
                    dlg.close()
                    dlg.openSettingsRequested()
                }
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            // 视觉权重刻意如此：**安全动作（取消=保持拒绝）用填充主按钮**，
            // **危险动作（清除记录并重新信任）用幽灵按钮**。
            // 本对话框的全部意义就是阻止用户在没搞清楚状况时盲目信任，
            // 若把"重新信任"做成最醒目/红色填充的按钮，等于在引导他点它。
            // （Escape 键也走取消；未设 DefaultButton 角色，Enter 不会落到"重新信任"。）
            PrimaryButton {
                text: qsTr("取消（保持拒绝）")
                onClicked: dlg.close()
            }
            GhostButton {
                glyph: "🗝"
                text: qsTr("清除记录并重新信任")
                onClicked: dlg.clearAndRetrust()
            }
        }
    }
}
