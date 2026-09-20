import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"
import "../controls"

/*!
    连接设置页：服务器地址（协议 / 主机·IP / 端口 三段）/ 访问令牌 / 自签证书开关
    / 证书指纹查看·清除 / 下载目录 / 能力说明。

    只使用 AppController（App）暴露的属性与方法；不直接发 HTTP、不碰 net/。
    ⚠️ C++ 冻结：AppController 只暴露「拼好的」serverUrl 一个属性，
       因此三段地址的「拆分显示 / 合并写回」由本页在 QML 侧完成。
*/
Item {
    id: page

    property string fingerprint: ""

    readonly property int contentMargin: Theme.spaceXl
    readonly property bool twoColumn: width >= 760

    // ------------------------------------------------------------------
    //  服务器地址三段（QML 侧拆分 / 合并 App.serverUrl）
    // ------------------------------------------------------------------
    property bool   syncing: false      // 写回 App 时抑制回灌，避免编辑被打断
    property string proto: "http"       // http | https
    property string host: ""            // 主机名 / IP
    property string port: ""            // 端口（字符串，可空）
    property bool   portEdited: false   // 用户是否显式设过端口（改过则不随协议联动覆盖）
    property string hostError: ""       // 主机字段校验提示

    // 令牌可见性
    property bool tokenVisible: false

    // 令牌是否由登录会话托管（已登录且非 legacy）：此时手动输入区改为只读，避免两套凭据并存。
    readonly property bool tokenManaged: (typeof Auth !== "undefined" && Auth.loggedIn !== undefined)
                                        ? (Auth.loggedIn && !Auth.legacyMode) : false

    // ---- 自动刷新：回灌抑制（避免 currentIndex ↔ 秒值 双向映射产生信号回环）----
    property bool autoRefreshSyncing: false
    readonly property var autoRefreshOptions: [
        { label: qsTr("15 秒"), seconds: 15 },
        { label: qsTr("30 秒"), seconds: 30 },
        { label: qsTr("60 秒"), seconds: 60 },
        { label: qsTr("120 秒"), seconds: 120 }
    ]

    // 秒值 → 索引（不在列表内时回落到「30 秒」）
    function intervalIndexFor(secs) {
        for (let i = 0; i < page.autoRefreshOptions.length; ++i) {
            if (page.autoRefreshOptions[i].seconds === secs)
                return i
        }
        return 1
    }

    // 从 App 回灌自动刷新控件（首次进入 + 外部改动）。程序化赋值不触发 onActivated / 用户 toggled，
    // 叠加 syncing 标记双保险，杜绝写回回环。
    function syncAutoRefresh() {
        page.autoRefreshSyncing = true
        autoRefreshSwitch.checked = App.autoRefresh
        intervalBox.currentIndex = page.intervalIndexFor(App.autoRefreshInterval)
        page.autoRefreshSyncing = false
    }

    // ------------------------------------------------------------------
    //  下载目录（可编辑 + 浏览 + 校验；持久化交给既有【保存设置】）
    //  App.downloadDir 是冻结的**运行时**属性：C++ 未落地时用 typeof 守卫，
    //  仅本地生效、不抛错。
    // ------------------------------------------------------------------
    property bool   dirSyncing: false        // 回灌文本时抑制 onTextChanged 写回
    property string downloadDir: ""          // 当前编辑值
    property string savedDownloadDir: ""     // 最近一次「已保存」值（用于「未保存」提示）
    property string downloadDirError: ""     // 校验提示
    readonly property bool downloadDirDirty: downloadDir !== savedDownloadDir

    // 本机磁盘列表（冻结 API；未落地时为空数组 → 卡片给「暂不可用」提示）
    readonly property var localDisks: (typeof Stats !== "undefined" && Stats.localDisks !== undefined)
                                      ? Stats.localDisks : []

    // file:// URL → 本地路径（Windows：file:///C:/x → C:/x；POSIX：file:///home/x → /home/x）
    function urlToPath(u) {
        let s = String(u)
        if (s.indexOf("file://") === 0) {
            s = s.substring(7)
            if (s.charAt(0) === "/" && s.length > 2 && s.charAt(2) === ":")
                s = s.substring(1)
        }
        try { s = decodeURIComponent(s) } catch (e) { /* 保持原样 */ }
        return s
    }

    // 校验：非空 + 绝对路径（Windows 盘符 / POSIX 根 / UNC）
    function validateDownloadDir(p) {
        const s = String(p === undefined || p === null ? "" : p).trim()
        if (s.length === 0)
            return qsTr("下载目录不能为空")
        const isWin = /^[A-Za-z]:[\\/]/.test(s)
        const isUnix = s.charAt(0) === "/"
        const isUnc = /^\\\\/.test(s)
        if (!isWin && !isUnix && !isUnc)
            return qsTr("请填写绝对路径（例如 C:\\Users\\me\\Downloads）")
        return ""
    }

    // 应用一个新目录值：更新本地态；合法且要求推送时写入 App.downloadDir
    function applyDownloadDir(path, pushToApp) {
        page.downloadDir = path
        page.downloadDirError = page.validateDownloadDir(path)
        if (pushToApp && page.downloadDirError.length === 0
                && typeof App.downloadDir !== "undefined")
            App.downloadDir = path
    }

    // 从 App 回灌（首次进入 / 外部改动）
    function syncDownloadDir() {
        const cur = (typeof App.downloadDir !== "undefined") ? String(App.downloadDir) : ""
        page.dirSyncing = true
        page.downloadDir = cur
        page.savedDownloadDir = cur
        page.downloadDirError = ""
        downloadDirField.text = cur
        page.dirSyncing = false
    }

    // 【保存设置】成功时调用：把当前值标记为已保存（消除「未保存」提示）
    function markDownloadDirSaved() {
        if (page.downloadDirError.length === 0)
            page.savedDownloadDir = page.downloadDir
    }

    function defaultPortFor(scheme) {
        return scheme === "http" ? "8080" : "8443"
    }

    // 把 App.serverUrl 解析成三段（scheme / host / port）
    function parseUrl(u) {
        let s = String(u === undefined || u === null ? "" : u).trim()
        let scheme = "http"
        let hostpart = ""
        let p = ""

        const sep = s.indexOf("://")
        if (sep >= 0) {
            scheme = s.substring(0, sep).toLowerCase()
            s = s.substring(sep + 3)
        }
        if (scheme !== "http" && scheme !== "https")
            scheme = "http"

        // 去掉路径 / 查询
        let cut = s.length
        const slash = s.indexOf("/")
        if (slash >= 0 && slash < cut) cut = slash
        const q = s.indexOf("?")
        if (q >= 0 && q < cut) cut = q
        s = s.substring(0, cut)

        // host[:port]（IPv6 以 [] 包裹）
        if (s.charAt(0) === "[") {
            const close = s.indexOf("]")
            if (close >= 0) {
                hostpart = s.substring(0, close + 1)
                const rest = s.substring(close + 1)
                if (rest.charAt(0) === ":")
                    p = rest.substring(1)
            } else {
                hostpart = s
            }
        } else {
            const colon = s.lastIndexOf(":")
            if (colon >= 0) {
                hostpart = s.substring(0, colon)
                p = s.substring(colon + 1)
            } else {
                hostpart = s
            }
        }

        page.proto = scheme
        page.host = hostpart
        page.port = p
        page.portEdited = (p.length > 0) // 地址里显式带端口 → 视为用户设定
    }

    // 三段合并回一个 URL（主机为空则视为无效，返回空串）
    function composeUrl() {
        if (page.host.length === 0)
            return ""
        let u = page.proto + "://" + page.host
        if (page.port.length > 0)
            u += ":" + page.port
        return u
    }

    function validateHost() {
        const h = page.host.trim()
        if (h.length === 0) {
            page.hostError = qsTr("请填写主机名或 IP 地址")
            return false
        }
        if (/\s/.test(h)) {
            page.hostError = qsTr("主机名不能包含空格")
            return false
        }
        if (h.indexOf("://") >= 0) {
            page.hostError = qsTr("主机字段无需重复填写协议（请在左侧选择）")
            return false
        }
        page.hostError = ""
        return true
    }

    // 用户改动后立即合并写回 App.serverUrl
    function commit() {
        if (page.syncing)
            return
        if (!page.validateHost())
            return
        const u = page.composeUrl()
        if (u.length === 0)
            return
        page.syncing = true
        App.serverUrl = u
        page.syncing = false
    }

    // 从 App 回灌三段（外部改动 / 首次进入）
    function syncFromApp() {
        page.syncing = true
        page.parseUrl(App.serverUrl)
        if (!page.portEdited && page.port.length === 0 && page.host.length > 0)
            page.port = page.defaultPortFor(page.proto)
        hostField.text = page.host
        portField.text = page.port
        protoBox.currentIndex = (page.proto === "http") ? 0 : 1
        page.syncing = false
    }

    function refreshFingerprint() {
        fingerprint = App.pinnedFingerprint()
    }

    Component.onCompleted: {
        syncFromApp()
        refreshFingerprint()
        syncAutoRefresh()
        syncDownloadDir()
        page.refreshLocalDisks()
    }

    // 切回设置页时重扫本机磁盘（U 盘插拔 / 空间变化都能反映）
    onVisibleChanged: {
        if (page.visible)
            page.refreshLocalDisks()
    }

    // 本机磁盘查询走 C++（QStorageInfo，纯本机、不阻塞）。
    // 未落地（undefined）时静默降级：localDisks 为空 → 卡片给「暂不可用」提示。
    function refreshLocalDisks() {
        if (typeof Stats !== "undefined" && typeof Stats.refreshLocalDisks === "function")
            Stats.refreshLocalDisks()
    }

    Connections {
        target: App
        function onServerUrlChanged() {
            if (!page.syncing)
                page.syncFromApp()
            page.refreshFingerprint()
        }
        function onAutoRefreshChanged() {
            autoRefreshSwitch.checked = App.autoRefresh
        }
        function onAutoRefreshIntervalChanged() {
            page.autoRefreshSyncing = true
            intervalBox.currentIndex = page.intervalIndexFor(App.autoRefreshInterval)
            page.autoRefreshSyncing = false
        }
        // 下载目录变更 → 只重扫磁盘（让「默认」标记跟着走）。
        // ⚠️ 这里**不能**调 syncDownloadDir()：那会把 savedDownloadDir 一起重置，
        //    导致「未保存」提示在用户刚敲完字时就被抹掉。
        function onDownloadDirChanged() {
            page.refreshLocalDisks()
        }
    }

    ScrollView {
        id: settingsScroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: settingsScroll.availableWidth
            spacing: Theme.spaceM

            Item { Layout.preferredHeight: Theme.spaceL }

            Label {
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                text: qsTr("连接设置")
                color: Theme.textPrimary
                font.pointSize: Theme.fontTitle + 3
                font.bold: true
            }

            // ==========================================================
            //  ① 连接服务器（最醒目的一张卡）
            // ==========================================================
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("连接服务器")
                subtitle: qsTr("填写服务器地址后点击【保存设置】。修改后即时生效并写入本地配置。")

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS

                    // ---- 协议 / 主机 / 端口 ----
                    // 固定 2 列（左：标签 右：控件），并给标签/控件显式最小尺寸。
                    // 说明：原先写的是 `columns: page.twoColumn ? 6 : 1`，把 3 个标签与 3 个控件
                    // 混排进「列数随宽度变化」的网格里，布局意图难以预测、窄容器下行为很不稳定。
                    // 注意：本次「控件看不见」的真正原因**不是**布局塌陷（实测控件几何完全正常，
                    // 700x36），而是样式调色板为深色导致文字/底色与白色卡片同色 —— 修复见
                    // src/app/main.cpp 的 setColorScheme(Light) + setPalette()。这里改成固定 2 列
                    // 是为了让布局具备确定性，与颜色问题无关。
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Theme.spaceM
                        rowSpacing: Theme.spaceS

                        Label {
                            text: qsTr("协议")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontBody
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            Layout.minimumWidth: 72
                        }
                        ComboBox {
                            id: protoBox
                            Layout.fillWidth: true
                            Layout.minimumWidth: 120
                            Layout.preferredHeight: Theme.controlHeight
                            implicitHeight: Theme.controlHeight
                            font.pointSize: Theme.fontBody
                            model: ["http", "https"]
                            onActivated: {
                                page.proto = currentText
                                // 端口未被用户设置过时，按协议联动默认端口
                                if (!page.portEdited) {
                                    page.port = page.defaultPortFor(page.proto)
                                    portField.text = page.port
                                }
                                page.commit()
                            }
                        }

                        Label {
                            text: qsTr("主机 / IP")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontBody
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                        }
                        TextField {
                            id: hostField
                            Layout.fillWidth: true
                            Layout.minimumWidth: 120
                            Layout.preferredHeight: Theme.controlHeight
                            implicitHeight: Theme.controlHeight
                            font.pointSize: Theme.fontBody
                            placeholderText: qsTr("192.168.185.231 或 example.com")
                            onTextChanged: {
                                if (page.syncing)
                                    return
                                page.host = text
                                page.hostError = ""
                            }
                            onEditingFinished: page.commit()
                        }

                        Label {
                            text: qsTr("端口")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontBody
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                        }
                        TextField {
                            id: portField
                            Layout.fillWidth: true
                            Layout.minimumWidth: 120
                            Layout.preferredHeight: Theme.controlHeight
                            implicitHeight: Theme.controlHeight
                            font.pointSize: Theme.fontBody
                            placeholderText: page.defaultPortFor(page.proto)
                            inputMethodHints: Qt.ImhDigitsOnly
                            onTextChanged: {
                                if (page.syncing)
                                    return
                                page.port = text
                                page.portEdited = true
                            }
                            onEditingFinished: page.commit()
                        }
                    }

                    // 主机校验提示
                    Label {
                        Layout.fillWidth: true
                        visible: page.hostError.length > 0
                        text: page.hostError
                        color: Theme.danger
                        font.pointSize: Theme.fontSecondary
                        wrapMode: Text.WordWrap
                    }

                    // 合并后的完整地址预览
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("将连接到：%1").arg(page.composeUrl().length > 0
                                                     ? page.composeUrl()
                                                     : qsTr("（未填写）"))
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                        elide: Text.ElideRight
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                // ---- 访问令牌（显示 / 隐藏）----
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs

                    Label {
                        text: qsTr("访问令牌")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontBody
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceS
                        TextField {
                            id: tokenField
                            Layout.fillWidth: true
                            Layout.minimumWidth: 160
                            Layout.preferredHeight: Theme.controlHeight
                            implicitHeight: Theme.controlHeight
                            font.pointSize: Theme.fontBody
                            echoMode: page.tokenVisible ? TextInput.Normal : TextInput.Password
                            placeholderText: qsTr("Bearer 令牌（可留空）")
                            text: App.accessToken
                            readOnly: page.tokenManaged
                            enabled: !page.tokenManaged
                        }
                        GhostButton {
                            glyph: page.tokenVisible ? "🙈" : "👁"
                            text: page.tokenVisible ? qsTr("隐藏") : qsTr("显示")
                            enabled: !page.tokenManaged
                            onClicked: page.tokenVisible = !page.tokenVisible
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: page.tokenManaged
                        text: qsTr("已通过登录会话托管访问令牌，无需手动填写。退出登录后可在此手动指定。")
                        color: Theme.success
                        font.pointSize: Theme.fontSecondary
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !page.tokenManaged
                        text: qsTr("服务端未启用鉴权时可留空。")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS

                    PrimaryButton {
                        glyph: "💾"
                        text: qsTr("保存设置")
                        onClicked: {
                            page.commit()
                            // 已登录时令牌由会话托管，不要用手动输入框（可能为空/过期）覆盖。
                            if (!page.tokenManaged)
                                App.accessToken = tokenField.text
                            App.saveSettings()
                            page.markDownloadDirSaved()
                        }
                    }
                    SecondaryButton {
                        glyph: "⇄"
                        text: App.busy ? qsTr("测试中…") : qsTr("测试连接")
                        enabled: !App.busy
                        onClicked: {
                            page.commit()
                            App.healthCheck()
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                // ---- 测试结果（就近反馈）----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    visible: App.statusText.length > 0

                    StatusBadge {
                        tone: App.statusText.indexOf("✗") === 0 ? "danger"
                            : (App.statusText.indexOf("⚠") === 0 ? "warning" : "success")
                        text: App.statusText.indexOf("✗") === 0 ? qsTr("连接失败")
                            : (App.statusText.indexOf("⚠") === 0 ? qsTr("提示") : qsTr("已连接"))
                    }
                    Label {
                        Layout.fillWidth: true
                        text: App.statusText
                        color: App.statusText.indexOf("✗") === 0 ? Theme.danger : Theme.textPrimary
                        font.pointSize: Theme.fontSecondary
                        wrapMode: Text.WordWrap
                        elide: Text.ElideRight
                    }
                }
            }

            // ==========================================================
            //  ①b 自动刷新
            // ==========================================================
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("自动刷新")
                subtitle: qsTr("每隔设定间隔检查一次服务器；只有文件发生变化时才刷新列表"
                               + "（内容未变则不打断选中与滚动）。")

                // ---- 总开关 ----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceM

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        Label {
                            text: qsTr("自动刷新文件列表")
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontBody
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("仅当你停留在「文件」页时自动刷新；离开该页不会轮询。")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                    }

                    // 裸 Switch 可用：src/app/main.cpp 已把调色板钉死为浅色。
                    Switch {
                        id: autoRefreshSwitch
                        onToggled: {
                            if (!page.autoRefreshSyncing)
                                App.autoRefresh = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                // ---- 刷新间隔（currentIndex ↔ 秒值 双向映射，syncing 防回环）----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceM

                    Label {
                        text: qsTr("刷新间隔")
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontBody
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ComboBox {
                        id: intervalBox
                        Layout.preferredWidth: 160
                        Layout.preferredHeight: Theme.controlHeight
                        implicitHeight: Theme.controlHeight
                        font.pointSize: Theme.fontBody
                        enabled: App.autoRefresh
                        model: page.autoRefreshOptions
                        textRole: "label"
                        onActivated: {
                            if (page.autoRefreshSyncing)
                                return
                            if (currentIndex >= 0 && currentIndex < page.autoRefreshOptions.length)
                                App.autoRefreshInterval = page.autoRefreshOptions[currentIndex].seconds
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("服务器不可达时，自动刷新会自动暂停以避免反复卡顿；"
                               + "恢复连接后右键「刷新」或按 F5 即可再次启用。")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }

            // ==========================================================
            //  ② 自签证书 / TOFU
            // ==========================================================
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
                            page.markDownloadDirSaved()
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

            // ==========================================================
            //  ③ 下载目录（可编辑 + 浏览 + 校验）
            //     根因：Settings 里早就有 downloadDir()/setDownloadDir()，但
            //     TransferManager 直接调 AppPaths::downloadDir()，设置项从未被读取。
            //     现在：编辑 → App.downloadDir → Application 注入 TransferManager，
            //     并落盘到 general/downloadDir（点【保存设置】时持久化）。
            // ==========================================================
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("下载目录")
                subtitle: qsTr("下载文件默认保存到此目录。修改后需点【保存设置】才会持久化；"
                               + "保存前即已在本次运行中生效。")

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceS

                        TextField {
                            id: downloadDirField
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.controlHeight
                            font.pointSize: Theme.fontBody
                            // ⚠️ 显式配色：系统控件跟随平台调色板，深色下会「白字白底」。
                            //    遵守 SearchField 模式（显式 color + 自带 background）。
                            color: Theme.textPrimary
                            placeholderTextColor: Theme.textSecondary
                            placeholderText: qsTr("例如 C:\\Users\\me\\Downloads")
                            selectByMouse: true
                            text: page.downloadDir

                            background: Rectangle {
                                implicitHeight: Theme.controlHeight
                                radius: Theme.radiusControl
                                color: Theme.card
                                border.width: downloadDirField.activeFocus ? 2 : 1
                                border.color: downloadDirField.activeFocus
                                              ? Theme.primary : Theme.border
                            }

                            onTextChanged: {
                                if (page.dirSyncing)
                                    return
                                page.applyDownloadDir(text, true)
                            }
                        }

                        SecondaryButton {
                            glyph: "📁"
                            text: qsTr("浏览")
                            onClicked: folderPicker.open()
                        }
                        SecondaryButton {
                            glyph: "📂"
                            text: qsTr("打开")
                            onClicked: Transfers.openLocalFolder()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceS
                        Label {
                            Layout.fillWidth: true
                            visible: page.downloadDirError.length > 0
                            text: page.downloadDirError
                            color: Theme.danger
                            font.pointSize: Theme.fontSecondary
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            visible: page.downloadDirError.length === 0
                                     && page.downloadDirDirty
                            text: qsTr("● 未保存")
                            color: Theme.warning
                            font.pointSize: Theme.fontSecondary
                        }
                        Label {
                            visible: page.downloadDirError.length === 0
                                     && !page.downloadDirDirty
                                     && page.downloadDir.length > 0
                            text: qsTr("✓ 已保存")
                            color: Theme.success
                            font.pointSize: Theme.fontSecondary
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ==========================================================
            //  ③b 本机磁盘空间（QStorageInfo，纯本机、无网络、不阻塞）
            //     与下载目录联动：当前下载目录所在卷标「默认」。
            // ==========================================================
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: page.contentMargin
                Layout.rightMargin: page.contentMargin
                title: qsTr("本机磁盘空间")
                subtitle: qsTr("客户端所在机器的各磁盘容量与剩余空间。")

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS

                    Repeater {
                        model: page.localDisks
                        delegate: ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceXs
                            required property var modelData

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceS
                                Label {
                                    text: modelData.label !== undefined
                                          ? modelData.label : modelData.name
                                    color: Theme.textPrimary
                                    font.pointSize: Theme.fontBody
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    visible: modelData.isDefault === true
                                    text: qsTr("默认")
                                    color: Theme.textOnPrimary
                                    font.pointSize: Theme.fontSecondary
                                    leftPadding: Theme.spaceXs
                                    rightPadding: Theme.spaceXs
                                    topPadding: 1
                                    bottomPadding: 1
                                    background: Rectangle {
                                        radius: 3
                                        color: Theme.primary
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: qsTr("剩余 %1 / 共 %2")
                                          .arg(modelData.freeText)
                                          .arg(modelData.totalText)
                                    color: Theme.textSecondary
                                    font.pointSize: Theme.fontSecondary
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 6
                                radius: 3
                                color: Theme.hoverStrong
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: Math.max(0, Math.min(1, modelData.usedRatio)) * parent.width
                                    radius: 3
                                    color: Theme.primary
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceS
                        visible: page.localDisks.length === 0
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("暂不可用（本机磁盘信息未获取到）")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        SecondaryButton {
                            glyph: "⟳"
                            text: qsTr("重新扫描")
                            onClicked: page.refreshLocalDisks()
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ==========================================================
            //  ④ 能力说明
            // ==========================================================
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

    // 下载目录选择（QtQuick.Dialogs；已确认模块可用）
    FolderDialog {
        id: folderPicker
        title: qsTr("选择下载目录")
        currentFolder: page.downloadDir.length > 0
                       ? Qt.resolvedUrl("file:///" + page.downloadDir) : ""
        onAccepted: {
            const p = page.urlToPath(String(selectedFolder))
            if (p.length > 0)
                page.applyDownloadDir(p, true)
        }
    }
}
