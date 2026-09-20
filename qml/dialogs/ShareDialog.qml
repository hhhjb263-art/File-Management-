import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    新建分享链接对话框。

    调用方（文件页右键「分享…」）先赋值 `fileId` / `fileName`，再 `openFor(id, name)`。
    两个阶段：
      · 表单态 —— 提取码（可空，4~6 位字母数字；可随机生成）+ 有效期 + 下载次数
      · 结果态 —— 链接 / 提取码（仅创建时返回一次）+ 三个复制按钮 + 醒目提示

    ⚠️ 依赖的 `Shares` 冻结 API（create/copyLink/busy/onCreated/onErrorOccurred）是**运行时**
       解析的；C++ 未落地前用 typeof 守卫 + `target` 只在弹出时挂接，绝不抛错/刷屏。
*/
Dialog {
    id: dlg

    // ---- 由调用方注入 ----
    property string fileId: ""
    property string fileName: ""

    // ---- 结果态 ----
    property bool   showResult: false
    property string createdUrl: ""
    property string createdCode: ""   // 明文提取码，仅创建时返回一次
    property string createError: ""   // 服务端返回的创建失败信息 / 未就绪提示
    property string resultStatus: ""  // 结果态提示（如「打开链接」失败）

    // ---- 冻结 API 守卫 ----
    readonly property bool apiReady: (typeof Shares !== "undefined")
                                     && (typeof Shares.create === "function")
    readonly property bool sharesBusy: (typeof Shares !== "undefined" && Shares.busy !== undefined)
                                       ? Shares.busy : false

    // 提取码校验：空合法；非空须 4~6 位且只含字母数字
    readonly property bool codeValid: {
        const c = codeField.text
        if (c.length === 0)
            return true
        return c.length >= 4 && c.length <= 6 && /^[A-Za-z0-9]+$/.test(c)
    }

    // ---- 有效期 / 下载次数选项（→ 冻结的 expireDays / maxDownloads 语义）----
    readonly property var expireOptions: [
        { label: qsTr("1 天"),  days: 1 },
        { label: qsTr("7 天"),  days: 7 },
        { label: qsTr("30 天"), days: 30 },
        { label: qsTr("永久"),  days: 0 }
    ]
    readonly property var countOptions: [
        { label: qsTr("不限"),  n: 0 },
        { label: qsTr("1 次"),  n: 1 },
        { label: qsTr("5 次"),  n: 5 },
        { label: qsTr("10 次"), n: 10 }
    ]

    modal: true
    anchors.centerIn: parent
    width: Math.min(460, (parent ? parent.width - Theme.spaceXl * 2 : 460))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: dlg.showResult ? qsTr("分享已创建") : qsTr("创建分享链接")
    standardButtons: Dialog.NoButton

    // 打开并复位（默认 7 天 / 不限次数）
    function openFor(id, name) {
        dlg.fileId = String(id === undefined || id === null ? "" : id)
        dlg.fileName = String(name === undefined || name === null ? "" : name)
        codeField.text = ""
        expireBox.currentIndex = 1   // 默认 7 天
        countBox.currentIndex = 0    // 默认不限
        dlg.showResult = false
        dlg.createdUrl = ""
        dlg.createdCode = ""
        dlg.createError = ""
        dlg.resultStatus = ""
        dlg.open()
    }

    // 随机生成 4 位提取码（剔除易混淆字符 0/O/1/I）
    function generateCode() {
        const set = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
        let s = ""
        for (let i = 0; i < 4; ++i)
            s += set.charAt(Math.floor(Math.random() * set.length))
        codeField.text = s
    }

    // 复制到系统剪贴板（走冻结 API；未落地则静默忽略）
    function copyText(t) {
        if (typeof Shares !== "undefined" && typeof Shares.copyLink === "function")
            Shares.copyLink(String(t))
    }

    // 用系统默认浏览器打开刚创建的链接（QtQml 内置，无需 C++）。返回 false → 就地提示。
    function openResultLink() {
        if (dlg.createdUrl.length === 0)
            return
        dlg.resultStatus = ""
        if (!Qt.openUrlExternally(dlg.createdUrl))
            dlg.resultStatus = qsTr("无法打开浏览器，请手动复制链接后在浏览器中打开")
    }

    // 结果态「复制链接和提取码」的拼接文本
    function combinedText() {
        if (dlg.createdCode.length > 0)
            return qsTr("链接：%1\n提取码：%2").arg(dlg.createdUrl).arg(dlg.createdCode)
        return dlg.createdUrl
    }

    function tryCreate() {
        if (dlg.sharesBusy)
            return
        if (!dlg.codeValid)
            return
        dlg.createError = ""
        if (!dlg.apiReady) {
            dlg.createError = qsTr("分享功能尚未就绪，请稍后再试。")
            return
        }
        const days = dlg.expireOptions[expireBox.currentIndex].days
        const maxN = dlg.countOptions[countBox.currentIndex].n
        Shares.create(dlg.fileId, codeField.text, days, maxN)
    }

    // 仅在弹出期间挂接（未弹出时不监听，避免与其它页面争抢同一信号）
    Connections {
        target: dlg.opened ? Shares : null
        function onCreated(url, code) {
            dlg.createdUrl = String(url)
            dlg.createdCode = (code === undefined || code === null) ? "" : String(code)
            dlg.createError = ""
            dlg.showResult = true
        }
        function onErrorOccurred(message) {
            dlg.createError = String(message)
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        // ==========================================================
        //  表单态
        // ==========================================================
        ColumnLayout {
            Layout.fillWidth: true
            visible: !dlg.showResult
            spacing: Theme.spaceS

            // 文件名（只读展示）
            Label {
                text: qsTr("分享文件")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.controlHeight
                radius: Theme.radiusControl
                color: Theme.bg
                border.width: 1
                border.color: Theme.border
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceM
                    anchors.rightMargin: Theme.spaceM
                    text: dlg.fileName.length > 0 ? dlg.fileName : qsTr("（未选择文件）")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontBody
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }

            // 提取码 + 随机生成
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceS

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs

                    Label {
                        text: qsTr("提取码（可留空；4~6 位字母或数字）")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                    TextField {
                        id: codeField
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        font.pointSize: Theme.fontBody
                        maximumLength: 6
                        selectByMouse: true
                        placeholderText: qsTr("留空则无需提取码")
                        // 显式配色：系统控件跟随平台调色板，深色下会「白字白底」。
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textSecondary
                        background: Rectangle {
                            implicitHeight: Theme.controlHeight
                            radius: Theme.radiusControl
                            color: Theme.card
                            border.width: codeField.activeFocus ? 2 : 1
                            border.color: !dlg.codeValid ? Theme.danger
                                        : codeField.activeFocus ? Theme.primary : Theme.border
                        }
                        onTextChanged: dlg.createError = ""
                    }
                }

                SecondaryButton {
                    glyph: "🎲"
                    text: qsTr("随机生成")
                    Layout.alignment: Qt.AlignBottom
                    onClicked: dlg.generateCode()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: codeField.text.length > 0 && !dlg.codeValid
                text: qsTr("提取码需为 4~6 位，且仅含字母或数字")
                color: Theme.danger
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WordWrap
            }

            // 有效期 / 下载次数
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                columns: 2
                columnSpacing: Theme.spaceM
                rowSpacing: Theme.spaceS

                Label {
                    text: qsTr("有效期")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                }
                ComboBox {
                    id: expireBox
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight
                    implicitHeight: Theme.controlHeight
                    font.pointSize: Theme.fontBody
                    model: dlg.expireOptions
                    textRole: "label"
                }

                Label {
                    text: qsTr("下载次数")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontBody
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                }
                ComboBox {
                    id: countBox
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight
                    implicitHeight: Theme.controlHeight
                    font.pointSize: Theme.fontBody
                    model: dlg.countOptions
                    textRole: "label"
                }
            }

            // 创建失败 / 未就绪提示
            Label {
                Layout.fillWidth: true
                visible: dlg.createError.length > 0
                text: dlg.createError
                color: Theme.danger
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceS
                Item { Layout.fillWidth: true }
                SecondaryButton {
                    text: qsTr("取消")
                    onClicked: dlg.close()
                }
                PrimaryButton {
                    glyph: "🔗"
                    text: dlg.sharesBusy ? qsTr("创建中…") : qsTr("创建")
                    enabled: !dlg.sharesBusy && dlg.codeValid
                    onClicked: dlg.tryCreate()
                }
            }
        }

        // ==========================================================
        //  结果态
        // ==========================================================
        ColumnLayout {
            Layout.fillWidth: true
            visible: dlg.showResult
            spacing: Theme.spaceS

            // 醒目提示
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: tipLabel.implicitHeight + Theme.spaceM * 2
                radius: Theme.radiusControl
                color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.35)
                Label {
                    id: tipLabel
                    // ⚠️ 不要用 anchors.fill：父矩形的 implicitHeight 依赖 tipLabel.implicitHeight，
                    //    而 fill 会把 label 的上下边也锚到父级 ⇒ 高度绑定环，QML 掐断循环后
                    //    盒子高度/宽度计算错乱（实测：提示框溢出对话框卡片，即“UI 错位”）。
                    //    只锚左/右/上：宽度被约束（自动换行），高度由内容自然撑开。
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spaceM
                    text: qsTr("⚠ 提取码仅在创建时显示一次，请及时保存（列表页不再显示明文）。")
                          + "\n" + qsTr("链接可在浏览器直接打开；若设了提取码，打开后需输入提取码才能下载。")
                    color: Theme.warning
                    font.pointSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
            }

            Label {
                Layout.topMargin: Theme.spaceXs
                text: qsTr("分享链接")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
            TextField {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.controlHeight
                readOnly: true
                selectByMouse: true
                text: dlg.createdUrl
                font.pointSize: Theme.fontBody
                color: Theme.textPrimary
                background: Rectangle {
                    implicitHeight: Theme.controlHeight
                    radius: Theme.radiusControl
                    color: Theme.bg
                    border.width: 1
                    border.color: Theme.border
                }
            }

            Label {
                text: qsTr("提取码")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
            TextField {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.controlHeight + Theme.spaceS
                readOnly: true
                selectByMouse: true
                horizontalAlignment: TextInput.AlignHCenter
                text: dlg.createdCode.length > 0 ? dlg.createdCode : qsTr("无提取码")
                color: dlg.createdCode.length > 0 ? Theme.primary : Theme.textSecondary
                font.pointSize: dlg.createdCode.length > 0 ? Theme.fontTitle + 3 : Theme.fontBody
                font.bold: dlg.createdCode.length > 0
                font.family: dlg.createdCode.length > 0 ? "monospace" : ""
                background: Rectangle {
                    implicitHeight: Theme.controlHeight + Theme.spaceS
                    radius: Theme.radiusControl
                    color: Theme.bg
                    border.width: 1
                    border.color: Theme.border
                }
            }

            // 三个复制按钮
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceS
                GhostButton {
                    glyph: "⧉"
                    text: qsTr("复制链接")
                    enabled: dlg.createdUrl.length > 0
                    onClicked: dlg.copyText(dlg.createdUrl)
                }
                GhostButton {
                    glyph: "🔑"
                    text: qsTr("复制提取码")
                    enabled: dlg.createdCode.length > 0
                    onClicked: dlg.copyText(dlg.createdCode)
                }
                GhostButton {
                    glyph: "⎘"
                    text: qsTr("复制链接和提取码")
                    enabled: dlg.createdUrl.length > 0
                    onClicked: dlg.copyText(dlg.combinedText())
                }
            }

            // 打开链接（系统默认浏览器）+ 打开失败提示
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceS
                GhostButton {
                    glyph: "🌐"
                    text: qsTr("打开链接")
                    enabled: dlg.createdUrl.length > 0
                    onClicked: dlg.openResultLink()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("在系统默认浏览器中打开该链接")
                }
                Label {
                    Layout.fillWidth: true
                    visible: dlg.resultStatus.length > 0
                    text: dlg.resultStatus
                    color: Theme.danger
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
                Item {
                    Layout.fillWidth: true
                    visible: dlg.resultStatus.length === 0
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceS
                Item { Layout.fillWidth: true }
                PrimaryButton {
                    text: qsTr("完成")
                    onClicked: dlg.close()
                }
            }
        }
    }
}
