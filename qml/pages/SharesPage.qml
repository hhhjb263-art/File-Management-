import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    分享链接页：列出/管理已有分享链接（新建入口在文件页右键「分享…」）。

    · `supported === false`（服务端/C++ 未就绪）时**回落**为原有的 `UnsupportedPage` 占位。
    · 列表数据经 `Shares.count` + `Shares.at(i)` 提供（`at()` 是函数调用，QML 无法自动追踪
      依赖 → 用自增 `sharesVersion` 作显式依赖，重取值）。
    · `busy` 时禁用「刷新 / 复制 / 撤销」，避免连点。

    ⚠️ 所有 `Shares.*` 访问都用 typeof 守卫 —— C++ 未落地时本页退化为占位态，绝不抛错/刷屏。
*/
Item {
    id: page

    // ---- 冻结 API 守卫 ----
    readonly property bool apiReady: (typeof Shares !== "undefined")
    readonly property bool supported: apiReady
                                      && (Shares.supported === undefined ? true : Shares.supported)
    readonly property bool sharesReady: apiReady && (Shares.supported === true)

    readonly property bool sharesBusy: sharesReady && Shares.busy !== undefined ? Shares.busy : false
    readonly property int  sharesCount: sharesReady && Shares.count !== undefined ? Shares.count : 0
    // 两栏切换：进行中（state === "active"）/ 已失效（其余：revoked/expired/exhausted）
    property bool showInvalid: false
    readonly property int activeCount:  sharesReady && Shares.activeCount  !== undefined ? Shares.activeCount  : 0
    readonly property int invalidCount: sharesReady && Shares.invalidCount !== undefined ? Shares.invalidCount : 0
    function rowState(info) { return page.sval(info, "state", "active"); }
    function rowMatches(info) {
        return page.showInvalid ? (page.rowState(info) !== "active")
                                : (page.rowState(info) === "active")
    }

    // 数据变更版本号（at() 是函数调用，需显式依赖才重取值）
    property int sharesVersion: 0

    // 页内就近反馈
    property string statusText: ""
    property bool   statusOk: true

    // 取某个分享项（越界/未就绪 → 空 map，避免绑定报错）
    function shareAt(i) {
        if (!sharesReady || typeof Shares.at !== "function")
            return ({})
        const m = Shares.at(i)
        return (m === undefined || m === null) ? ({}) : m
    }

    // 安全取值：键缺失/空 → fallback 字符串
    function sval(info, key, fallback) {
        const v = info[key]
        return (v === undefined || v === null) ? fallback : String(v)
    }

    function copyLink(text) {
        if (sharesReady && typeof Shares.copyLink === "function")
            Shares.copyLink(String(text))
    }

    function doRefresh() {
        if (sharesReady && typeof Shares.refresh === "function")
            Shares.refresh()
    }

    function doRevoke(id) {
        if (sharesReady && typeof Shares.revoke === "function")
            Shares.revoke(String(id))
    }

    // 页内提示（沿用既有 statusText/statusOk 通道，不新造第二套 toast）
    function setStatus(text, ok) {
        page.statusText = String(text)
        page.statusOk = (ok !== false)
    }

    // 用系统默认浏览器打开链接（QtQml 内置 Qt.openUrlExternally，无需 C++）。
    // 返回 false（无可用浏览器/打开被拒）→ 走页内提示通道。
    function openExternal(url) {
        const u = String(url)
        if (u.length === 0)
            return
        if (!Qt.openUrlExternally(u))
            page.setStatus(qsTr("无法打开浏览器，请手动复制链接后在浏览器中打开"), false)
    }

    // 进页面时刷新一次
    onVisibleChanged: {
        if (page.visible)
            page.doRefresh()
    }

    // 仅在「已就绪」时挂接（未就绪 target=null → 不产生任何信号连接/告警）
    Connections {
        target: page.sharesReady ? Shares : null
        function onSharesChanged() { page.sharesVersion++ }
        function onErrorOccurred(message) {
            page.statusText = String(message)
            page.statusOk = false
        }
        function onStatusMessage(message, ok) {
            if (String(message).length > 0) {
                page.statusText = String(message)
                page.statusOk = (ok !== false)
            }
        }
    }

    // ==================================================================
    //  未就绪兜底：原有占位页（保留既有，勿删）
    // ==================================================================
    UnsupportedPage {
        anchors.fill: parent
        visible: !page.supported
        featureName: qsTr("分享链接")
        glyph: "🔗"
        notice: (typeof Shares !== "undefined" && Shares.unsupportedNotice !== undefined
                 && String(Shares.unsupportedNotice).length > 0)
                ? Shares.unsupportedNotice
                : qsTr("服务端暂不支持分享链接。")
    }

    // ==================================================================
    //  真实页面
    // ==================================================================
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM
        visible: page.supported

        // ---- 头部 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label {
                    Layout.fillWidth: true
                    text: qsTr("分享链接")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontTitle + 3
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("此处管理已有的分享链接。新建分享请到「文件」页右键文件 →「分享…」。")
                          + "\n" + qsTr("分享链接可直接在浏览器打开；若已设提取码，打开后需输入提取码才能下载。")
                          + "\n" + qsTr("提取码仅在创建时显示一次，之后无法查看，请妥善保存。")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }

            GhostButton {
                glyph: "⟳"
                text: qsTr("刷新")
                enabled: !page.sharesBusy
                onClicked: page.doRefresh()
            }
        }

        // ---- 就近状态（成功 / 失败）----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            visible: page.statusText.length > 0

            StatusBadge {
                tone: page.statusOk ? "success" : "danger"
                text: page.statusOk ? qsTr("提示") : qsTr("错误")
            }
            Label {
                Layout.fillWidth: true
                text: page.statusText
                color: page.statusOk ? Theme.textPrimary : Theme.danger
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WordWrap
            }
        }

        // ---- 两栏切换：进行中 / 已失效 + 清除无效分享 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            GhostButton {
                checkable: true
                checked: !page.showInvalid
                text: qsTr("进行中 (%1)").arg(page.activeCount)
                font.pointSize: Theme.fontBody
                onClicked: page.showInvalid = false
            }
            GhostButton {
                checkable: true
                checked: page.showInvalid
                text: qsTr("已失效 (%1)").arg(page.invalidCount)
                font.pointSize: Theme.fontBody
                onClicked: page.showInvalid = true
            }
            Item { Layout.fillWidth: true }
            DangerButton {
                visible: page.showInvalid && page.invalidCount > 0
                glyph: "🧹"
                text: qsTr("🧹 清理")
                enabled: !page.sharesBusy
                onClicked: cleanupConfirm.open()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("永久删除已撤销 / 已过期 / 次数用尽的分享记录，不可恢复")
            }
        }

        // ---- 列表卡片 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true

            ListView {
                id: shareList
                anchors.fill: parent
                anchors.margins: Theme.spaceS
                clip: true
                spacing: Theme.spaceS
                model: page.sharesCount
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }

                delegate: Rectangle {
                    id: shareRow

                    // 显式依赖 sharesVersion，保证「下载次数变化 / 撤销」后行内容刷新
                    readonly property var info: {
                        page.sharesVersion
                        return page.shareAt(index)
                    }
                    readonly property bool isExpired: info.expired === true || info.usable === false
                    readonly property string url: page.sval(info, "url", "")

                    width: shareList.width
                    implicitHeight: rowCol.implicitHeight + Theme.spaceM * 2
                    radius: Theme.radiusControl
                    color: Theme.bg
                    border.width: 1
                    border.color: Theme.border
                    // 失效/已撤销项弱化视觉（撤销优先级更高）
                    opacity: shareRow.info.revoked === true ? 0.45
                                        : (isExpired ? 0.62 : 1.0)

                    ColumnLayout {
                        id: rowCol
                        anchors.fill: parent
                        anchors.margins: Theme.spaceM
                        spacing: Theme.spaceXs

                        // 第 1 行：图标 / 名称 / 大小 / 徽标
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceS

                            Text {
                                text: shareRow.info.isDir === true ? "📁" : "📄"
                                font.pointSize: Theme.fontBody
                                color: Theme.textSecondary
                            }
                            Label {
                                Layout.fillWidth: true
                                text: page.sval(shareRow.info, "fileName", qsTr("（未知文件）"))
                                color: Theme.textPrimary
                                font.pointSize: Theme.fontBody
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Label {
                                text: page.sval(shareRow.info, "sizeText", "")
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                            StatusBadge {
                                visible: shareRow.info.needCode === true
                                tone: "info"
                                text: qsTr("已设提取码")
                            }
                            StatusBadge {
                                visible: shareRow.isExpired
                                tone: "neutral"
                                text: qsTr("已失效")
                            }
                            StatusBadge {
                                visible: shareRow.info.revoked === true
                                tone: "danger"
                                text: qsTr("已撤销")
                            }
                        }

                        // 第 2 行：有效期 / 剩余次数 / 已下载
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceM

                            Label {
                                text: page.sval(shareRow.info, "expireText", "")
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                            Label {
                                text: page.sval(shareRow.info, "remainText", "")
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                            Label {
                                text: {
                                    const d = shareRow.info.downloads
                                    const mx = shareRow.info.maxDownloads
                                    const dn = (d === undefined || d === null) ? 0 : Number(d)
                                    if (mx !== undefined && mx !== null && Number(mx) > 0)
                                        return qsTr("已下载 %1/%2 次").arg(dn).arg(Number(mx))
                                    return qsTr("已下载 %1 次").arg(dn)
                                }
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                            Label {
                                // 创建时间
                                text: shareRow.info.created !== undefined && shareRow.info.created !== null
                                          ? qsTr("创建于 %1").arg(Qt.formatDateTime(shareRow.info.created, "yyyy-MM-dd HH:mm"))
                                          : qsTr("创建时间未知")
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                            Label {
                                // 提取码明文只在创建时的本机缓存过；其它设备创建的显示「—」
                                readonly property string codeShown: {
                                    const c = page.sval(shareRow.info, "codeText", "")
                                    return c.length > 0 ? c : ""
                                }
                                text: codeShown.length > 0 ? qsTr("提取码 %1").arg(codeShown)
                                                   : (page.sval(shareRow.info, "needCode", false) === true
                                                          ? qsTr("提取码 —（仅创建时的本机可见）")
                                                          : qsTr("无提取码"))
                                color: codeShown.length > 0 ? Theme.primary : Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // 第 3 行：链接（中间省略）+ 复制 / 撤销
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceS

                            Label {
                                Layout.fillWidth: true
                                text: shareRow.url
                                color: Theme.info
                                font.pointSize: Theme.fontSecondary
                                elide: Text.ElideMiddle
                            }
                            GhostButton {
                                glyph: "⧉"
                                implicitWidth: Theme.controlHeight
                                enabled: !page.sharesBusy && shareRow.url.length > 0
                                onClicked: page.copyLink(shareRow.url)
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("复制链接")
                            }
                            // 「打开」：用系统默认浏览器验证/查看分享链接（Qt.openUrlExternally）。
                            // 过期项仍允许打开（浏览器会显示过期提示，正好可用于验证），按钮给 ToolTip 说明。
                            GhostButton {
                                id: openLinkBtn
                                glyph: "🌐"
                                implicitWidth: Theme.controlHeight
                                enabled: !page.sharesBusy && shareRow.url.length > 0
                                onClicked: page.openExternal(shareRow.url)
                                ToolTip.visible: openLinkBtn.hovered
                                ToolTip.text: shareRow.isExpired
                                              ? qsTr("已过期：打开可验证链接状态")
                                              : qsTr("在系统默认浏览器中打开")
                            }
                            DangerButton {
                                glyph: "🗑"
                                implicitWidth: Theme.controlHeight
                                enabled: !page.sharesBusy
                                onClicked: {
                                    revokeConfirm.shareId = page.sval(shareRow.info, "id", "")
                                    revokeConfirm.shareName = page.sval(shareRow.info, "fileName", "")
                                    revokeConfirm.open()
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("撤销此分享（链接立即失效）")
                            }
                        }
                    }
                }

                // ---- 空态 ----
                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width
                    visible: page.sharesCount === 0 && !page.sharesBusy
                    glyph: "🔗"
                    title: qsTr("还没有分享链接")
                    description: qsTr("到「文件」页右键一个文件 →「分享…」，即可创建分享链接。")
                }

                // ---- 加载态 ----
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: page.sharesCount === 0 && page.sharesBusy
                    spacing: Theme.spaceS
                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: page.sharesCount === 0 && page.sharesBusy
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("正在加载…")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontBody
                    }
                }
            }
        }
    }

    // ==================================================================
    //  撤销二次确认
    // ==================================================================
    Dialog {
        id: revokeConfirm

        property string shareId: ""
        property string shareName: ""

        modal: true
        anchors.centerIn: parent
        width: Math.min(440, (parent ? parent.width - Theme.spaceXl * 2 : 440))
        padding: Theme.spaceL
        closePolicy: Popup.CloseOnEscape
        title: qsTr("撤销分享链接")
        standardButtons: Dialog.NoButton

        contentItem: ColumnLayout {
            spacing: Theme.spaceM

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS
                Text {
                    text: "⚠️"
                    font.pointSize: 22
                    color: Theme.danger
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("确定撤销「%1」的分享链接吗？").arg(revokeConfirm.shareName)
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontBody
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("撤销后该链接立即失效，且不可恢复。")
                color: Theme.textSecondary
                font.pointSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS
                Item { Layout.fillWidth: true }
                SecondaryButton {
                    text: qsTr("取消")
                    onClicked: revokeConfirm.close()
                }
                DangerButton {
                    glyph: "🗑"
                    text: qsTr("撤销")
                    enabled: !page.sharesBusy
                    onClicked: {
                        page.doRevoke(revokeConfirm.shareId)
                        revokeConfirm.close()
                    }
                }
            }
        }
    }

    // 进入分享页即自动刷新（每次进入都拉最新列表；失效项自动归入「已失效」栏）
    Component.onCompleted: page.doRefresh()

    // 「清除无效分享」确认框：物理删除、不可恢复，必须二次确认
    Dialog {
        id: cleanupConfirm
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, (parent ? parent.width - Theme.spaceXl * 2 : 440))
        standardButtons: Dialog.NoButton
        title: qsTr("清除无效分享")

        ColumnLayout {
            spacing: Theme.spaceM
            Label {
                Layout.fillWidth: true
                text: qsTr("将永久删除全部「已撤销 / 已过期 / 次数用尽」的分享记录（含其链接），此操作不可恢复。")
                wrapMode: Text.WordWrap
                color: Theme.textPrimary
                font.pointSize: Theme.fontBody
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("进行中的分享不受影响。")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
        }
        footer: DialogButtonBox {
            GhostButton { text: qsTr("取消"); onClicked: cleanupConfirm.reject() }
            DangerButton {
                text: qsTr("确认清除")
                onClicked: { cleanupConfirm.close(); Shares.cleanupInvalid() }
            }
        }
    }
}
