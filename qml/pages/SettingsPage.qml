import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    连接设置页：服务器地址 / 访问令牌 / 自签证书开关 / 证书指纹查看·清除 / 下载目录。
    只使用 AppController（App）暴露的属性与方法；不直接发 HTTP、不碰 net/。
*/
Item {
    id: page

    property string fingerprint: ""

    readonly property int contentMargin: Theme.spaceXl
    readonly property bool twoColumn: width >= 760

    function refreshFingerprint() {
        fingerprint = App.pinnedFingerprint()
    }

    Component.onCompleted: refreshFingerprint()

    Connections {
        target: App
        function onServerUrlChanged() { page.refreshFingerprint() }
    }

    ScrollView {
        id: settingsScroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: settingsScroll.availableWidth
            spacing: Theme.spaceL

            Item { Layout.preferredHeight: Theme.spaceXl }

            Label {
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                text: qsTr("连接设置")
                color: Theme.textPrimary
                font.pointSize: Theme.fontTitle + 2
                font.bold: true
            }

            // ---- 连接 ----
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("服务器连接")
                subtitle: qsTr("修改后会立即生效并自动保存。")

                GridLayout {
                    Layout.fillWidth: true
                    columns: page.twoColumn ? 2 : 1
                    columnSpacing: Theme.spaceM
                    rowSpacing: Theme.spaceS

                    Label {
                        text: qsTr("服务器地址")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontBody
                        Layout.preferredWidth: 90
                    }
                    TextField {
                        id: urlField
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        font.pointSize: Theme.fontBody
                        placeholderText: qsTr("https://主机:端口")
                        text: App.serverUrl
                        onEditingFinished: {
                            App.serverUrl = text
                            App.saveSettings()
                            page.refreshFingerprint()
                        }
                    }

                    Label {
                        text: qsTr("访问令牌")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontBody
                        Layout.preferredWidth: 90
                    }
                    TextField {
                        id: tokenField
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        font.pointSize: Theme.fontBody
                        echoMode: TextInput.Password
                        placeholderText: qsTr("Bearer 令牌")
                        text: App.accessToken
                        onEditingFinished: {
                            App.accessToken = text
                            App.saveSettings()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS

                    PrimaryButton {
                        glyph: "💾"
                        text: qsTr("保存设置")
                        onClicked: {
                            App.serverUrl = urlField.text
                            App.accessToken = tokenField.text
                            App.saveSettings()
                        }
                    }
                    SecondaryButton {
                        glyph: "⇄"
                        text: qsTr("健康检查")
                        enabled: !App.busy
                        onClicked: App.healthCheck()
                    }
                    GhostButton {
                        glyph: "💽"
                        text: qsTr("刷新存储空间")
                        onClicked: Stats.refresh()
                    }
                    Item { Layout.fillWidth: true }
                }

                StatusBadge {
                    Layout.fillWidth: true
                    visible: App.statusText.length > 0
                    tone: App.statusText.indexOf("✗") === 0 ? "danger"
                        : (App.statusText.indexOf("⚠") === 0 ? "warning" : "success")
                    outlined: true
                    text: App.statusText
                }
            }

            // ---- 自签证书 / TOFU ----
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("自签名证书与信任")
                subtitle: qsTr("首次连接自签名服务器需人工核对证书指纹（TOFU）。"
                               + "未确认即拒绝连接，指纹不一致时不会放行。")

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceM

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        Label {
                            text: qsTr("允许使用自签名证书")
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontBody
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("关闭后交给系统证书链校验；开启时首次连接仍需确认指纹。")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                    }

                    Switch {
                        id: trustSwitch
                        onToggled: {
                            App.trustSelfSigned = checked
                            App.saveSettings()
                        }
                    }
                }

                Connections {
                    target: App
                    function onTrustSelfSignedChanged() {
                        trustSwitch.checked = App.trustSelfSigned
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                Label {
                    text: qsTr("当前主机的已固定证书指纹（SHA-256）")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: fpLabel.implicitHeight + Theme.spaceM * 2
                    radius: Theme.radiusControl
                    color: Theme.bg
                    border.width: 1
                    border.color: Theme.border

                    Label {
                        id: fpLabel
                        anchors.fill: parent
                        anchors.margins: Theme.spaceM
                        text: page.fingerprint.length > 0
                              ? page.fingerprint
                              : qsTr("（尚未固定指纹 — 首次成功信任该主机后会写入）")
                        color: page.fingerprint.length > 0 ? Theme.textPrimary : Theme.textSecondary
                        font.pointSize: Theme.fontMono
                        font.family: "monospace"
                        wrapMode: Text.WrapAnywhere
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS

                    SecondaryButton {
                        glyph: "👁"
                        text: qsTr("查看 / 刷新指纹")
                        onClicked: page.refreshFingerprint()
                    }
                    DangerButton {
                        glyph: "🗑"
                        text: qsTr("清除该主机信任")
                        onClicked: {
                            App.clearPinnedFingerprint()
                            page.refreshFingerprint()
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            // ---- 下载目录 ----
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("下载目录")
                subtitle: qsTr("下载文件默认保存到客户端配置的本地下载目录"
                               + "（未设置时使用系统「下载」目录）。")

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    SecondaryButton {
                        glyph: "📂"
                        text: qsTr("打开下载目录")
                        onClicked: Transfers.openLocalFolder()
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            // ---- 未支持功能说明 ----
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("能力说明")
                Label {
                    Layout.fillWidth: true
                    text: App.unsupportedNotice
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
            }

            Item { Layout.preferredHeight: Theme.spaceXl }
        }
    }
}
