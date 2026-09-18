import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    关于对话框：产品名、版本、能力与限制说明。
    版本取 Qt.application.version（main.cpp 已 setApplicationVersion(CV_APP_VERSION)）。
*/
Dialog {
    id: dlg

    modal: true
    anchors.centerIn: parent
    width: Math.min(460, (parent ? parent.width - Theme.spaceXl * 2 : 460))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("关于云匣")
    standardButtons: Dialog.NoButton

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceM

            Text {
                text: "☁"
                font.pointSize: 34
                color: Theme.primary
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label {
                    text: qsTr("云匣 CloudVault")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontTitle
                    font.bold: true
                }
                Label {
                    text: qsTr("个人私有云网盘客户端 · 版本 %1").arg(Qt.application.version)
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("支持分块上传（含秒传与断点续传）、Range 并发下载、同名原子覆盖、"
                       + "自签名证书 TOFU 指纹固定。")
            color: Theme.textSecondary
            font.pointSize: Theme.fontBody
            wrapMode: Text.WordWrap
        }

        SectionCard {
            Layout.fillWidth: true
            title: qsTr("服务端暂不支持")
            Label {
                Layout.fillWidth: true
                text: App.unsupportedNotice
                color: Theme.textSecondary
                font.pointSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            PrimaryButton {
                text: qsTr("关闭")
                onClicked: dlg.close()
            }
        }
    }
}
