import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"
import "../controls"
import "../dialogs"

/*!
    文件页：工具条（左侧搜索 · 右侧主操作）+ 面包屑 + 列表（空 / 加载 / 错误三态）
    + 右键菜单（四组，带图标）+ 拖拽上传 + 双击进入目录。

    数据来源：FileModel（契约 §4 角色）；操作只经 FileController / TransferController。
    目录识别：FileController 合成的目录行 id 以 "dir:" 前缀（net 层约定），
    进入目录用 Files.enterDir(id.substring(4))。
*/
Item {
    id: page

    // 由 Main 注入的过滤关键字（header 搜索框）
    property string filterText: ""

    // 本地选择集（与 Files.selectedIds 同步）
    property var selectedIds: []

    // 右键目标
    property string ctxId: ""
    property string ctxName: ""
    property bool ctxIsDir: false
    // 右键目标**所在**的目录（相对路径）。用来拼出文件夹自身的完整路径：
    // 不用 page.dirPathOf(ctxId) —— 那是靠 substring(4) 剥 HttpBackend 的 id 前缀，
    // 对 MockBackend 的内部 id 会得到垃圾。path 才是两个数据源共有的稳定标识。
    property string ctxDir: ""

    // 排序状态（与模型内排序保持一致）
    property int sortKey: 2
    property bool sortAsc: false

    // 列表错误（仅 count===0 时呈现错误态）
    property string listError: ""

    // 面包屑
    property var crumbs: []

    // 请求跳转到设置页（错误态「检查令牌」引导）
    signal requestSettings()

    // 请求打开指定文件的版本历史（Main.qml 负责把目标交给 Versions 控制器并切页）。
    // 页面不直接改 navIndex：StackLayout 的页索引是 Main 的私有约定，页面不该知道。
    signal requestVersions(string fileId, string fileName)

    // 请求主窗口弹出通知：复用 Main.qml 唯一的 window.showToast，不新造第二套 toast。
    signal notify(string message, bool ok)

    // 自动刷新：服务器不可达时暂停（L1 下 refresh() 为同步阻塞，超时 60s，
    // 若持续轮询会反复冻结界面 —— 失败一次即停，不做重试风暴）。
    property bool autoRefreshPaused: false

    /*!
        自动刷新定时器：以下四个条件**同时**满足才轮询 —— 缺一不可。
          1) App.autoRefresh       —— 设置里的总开关
          2) page.visible          —— StackLayout 会把非当前页置 visible=false，不在文件页不轮询
          3) !Files.loading        —— 防重入堆叠（同步阻塞下堆叠会雪上加霜）
          4) !page.autoRefreshPaused —— 失败后暂停
        低频轮询走 refreshIfChanged()（内容指纹未变则**不动界面、不打断选中/滚动**）；
        只在本客户端自身增删改 / 传输完成 / 用户手动刷新时才用 refresh() 立即更新界面。
        注意：不在此处（Component.onCompleted）调用刷新，以免与 Main.qml 启动时已有的刷新重复。
    */
    Timer {
        id: autoRefreshTimer
        interval: Math.max(5, App.autoRefreshInterval) * 1000
        repeat: true
        running: App.autoRefresh
                 && page.visible
                 && !Files.loading
                 && !page.autoRefreshPaused
        onTriggered: Files.refreshIfChanged()
    }

    // 立即刷新并解除暂停（右键「刷新」/ F5 共用）。若仍失败，onRefreshFinished(false) 会再次暂停。
    function refreshNow() {
        page.autoRefreshPaused = false
        Files.refresh()
    }

    // ------------------------------------------------------------------
    //  预览联动（右侧预览面板）
    //  Files.requestPreview/clearPreview 是冻结的**运行时** API：C++ 未落地时
    //  typeof 守卫直接跳过，不抛错、不刷屏。
    // ------------------------------------------------------------------
    // 当前已请求预览的文件 id —— 用于「目标未变则不重复请求」去抖（防刷新/重列时刷屏）。
    property string previewedId: ""

    function syncPreview() {
        if (typeof Files.requestPreview !== "function")
            return
        // 期望目标：仅在本页可见、且恰好选中 1 项时预览；否则清空。
        const want = (page.visible && page.selectedIds.length === 1)
                     ? String(page.selectedIds[0]) : ""
        if (want === page.previewedId)
            return
        page.previewedId = want
        if (want.length === 0) {
            if (typeof Files.clearPreview === "function")
                Files.clearPreview()
        } else {
            Files.requestPreview(want)
        }
    }

    // 选中项变化 → 请求 / 清空预览
    onSelectedIdsChanged: page.syncPreview()
    // 离开文件页 → 清空预览（避免切页后仍显示上一个文件的内容）
    onVisibleChanged: page.syncPreview()

    // Ctrl+F 聚焦过滤框（仅本页可见时生效）
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: {
            searchField.forceActiveFocus()
            searchField.selectAll()
        }
    }

    // 窄版：隐藏「修改时间」列，避免横向滚动
    readonly property bool narrow: width < 560
    readonly property int colSizeWidth: 96
    readonly property int colTimeWidth: 170

    // ------------------------------------------------------------------
    //  工具函数
    // ------------------------------------------------------------------
    function isDirId(id) {
        return String(id).indexOf("dir:") === 0
    }

    function dirPathOf(id) {
        return String(id).substring(4)
    }

    function toLocalPath(u) {
        let s = String(u)
        if (s.indexOf("file://") === 0) {
            s = s.substring(7)
            // Windows：file:///C:/x → C:/x；POSIX：file:///home/x → /home/x
            if (s.length > 2 && s.charAt(2) === ":")
                s = s.substring(1)
            return decodeURIComponent(s)
        }
        return decodeURIComponent(s)
    }

    function rebuildCrumbs() {
        const d = Files.currentDir
        const arr = [{ label: qsTr("根目录"), path: "" }]
        if (d && d.length > 0) {
            const parts = d.split("/")
            let acc = ""
            for (let i = 0; i < parts.length; ++i) {
                acc = acc.length > 0 ? acc + "/" + parts[i] : parts[i]
                arr.push({ label: parts[i], path: acc })
            }
        }
        page.crumbs = arr
    }

    function applySort(key, asc) {
        page.sortKey = key
        page.sortAsc = (asc === undefined) ? page.sortAsc : asc
        FileModel.sortBy(page.sortKey, page.sortAsc)
        page.rebuild() // 模型 reset 不触发 countChanged，手动重建过滤视图
    }

    function selectRow(index, additive) {
        const item = proxyModel.get(index)
        if (!item)
            return
        const id = item.fid
        if (additive) {
            const arr = page.selectedIds.slice()
            const pos = arr.indexOf(id)
            if (pos >= 0)
                arr.splice(pos, 1)
            else
                arr.push(id)
            page.selectedIds = arr
        } else {
            page.selectedIds = [id]
        }
        Files.selectedIds = page.selectedIds
    }

    function openRow(index) {
        const item = proxyModel.get(index)
        if (!item)
            return
        if (page.isDirId(item.fid))
            Files.enterDir(page.dirPathOf(item.fid))
        else
            Transfers.download([item.fid], "")
    }

    // 由 id 反查名称（右键菜单「分享…」需要把「唯一选中项」的 name 传给对话框）
    // 空白处右键：清掉"右键目标"，于是 ctxMenu 里依赖 ctxId 的项全部自动置灰，
    // 只剩「刷新 / 新建 / 上传到此目录 / 排序」可用（这正是空白处的合理菜单）。
    function openBlankMenu() {
        page.ctxId    = ""
        page.ctxName  = ""
        page.ctxDir   = ""
        page.ctxIsDir = false
        ctxMenu.popup()
    }

    function nameForId(id) {
        const n = proxyModel.count
        for (let i = 0; i < n; ++i) {
            const it = proxyModel.get(i)
            if (it && String(it.fid) === String(id))
                return String(it.name)
        }
        return ""
    }

    // ------------------------------------------------------------------
    //  过滤视图（QML 侧，不改动 C++ 模型；模型仍负责真实排序）
    // ------------------------------------------------------------------
    ListModel { id: proxyModel }

    function rebuild() {
        proxyModel.clear()
        const n = FileModel.count
        const q = page.filterText.toLowerCase()
        const present = ({})
        for (let i = 0; i < n; ++i) {
            const r = FileModel.rowAt(i)
            if (!r || r.name === undefined)
                continue
            const name = String(r.name)
            if (q.length > 0 && name.toLowerCase().indexOf(q) < 0)
                continue
            const fid = String(r.id)
            present[fid] = true
            proxyModel.append({
                fid: fid,
                name: name,
                size: String(r.size),
                sizeBytes: r.sizeBytes,
                time: String(r.time),
                status: String(r.status),
                dir: String(r.dir),
                hash: String(r.hash)
            })
        }
        // 修剪选中项到当前可见集合
        if (page.selectedIds.length > 0) {
            const keep = []
            for (let k = 0; k < page.selectedIds.length; ++k) {
                if (present[page.selectedIds[k]] === true)
                    keep.push(page.selectedIds[k])
            }
            if (keep.length !== page.selectedIds.length) {
                page.selectedIds = keep
                Files.selectedIds = keep
            }
        }
    }

    Component.onCompleted: {
        page.rebuildCrumbs()
        page.rebuild()
    }

    onFilterTextChanged: page.rebuild()

    Connections {
        target: FileModel
        // 重列后顺带**校验**预览目标（删除/改名使被预览文件消失时清空预览）。
        // 仅一次校验（带 id 去抖），不做轮询。
        function onCountChanged() { page.rebuild(); page.syncPreview() }
    }

    // 「操作后立即刷」：本客户端自身的传输任务进入历史（完成 / 失败 / 取消）时，
    // 服务端文件必然已变化（如上传完成）→ 立即重列，不等下一次低频轮询。
    // 仅在未暂停时刷新：避免对已知不可达的服务器再发起阻塞式（60s）请求。
    Connections {
        target: TransferModel
        function onHistoryChanged() {
            if (!page.autoRefreshPaused)
                Files.refresh()
        }
    }

    Connections {
        target: Files
        function onCurrentDirChanged() { page.rebuildCrumbs() }
        function onLoadingChanged() { if (Files.loading) page.listError = "" }
        function onErrorOccurred(message) { page.listError = message }
        // 自动刷新：仅由 Files.refreshFinished 驱动（成功/失败各一次）——
        // 不能用 statusMessage：Main.qml 的 toast 接线对任何非空 message 都会弹。
        function onRefreshFinished(ok) {
            if (ok) {
                page.autoRefreshPaused = false
            } else if (App.autoRefresh && !page.autoRefreshPaused) {
                page.autoRefreshPaused = true
                page.notify(qsTr("自动刷新已暂停：服务器不可达，可右键「刷新」或按 F5 重试"), false)
            }
        }
        function onUploadRequested(dir) {
            uploadDialog.pendingDir = dir
            uploadDialog.open()
        }
        function onDownloadRequested(ids) {
            Transfers.download(ids, "")   // '' → TransferManager 用默认下载目录
        }
    }

    // ------------------------------------------------------------------
    //  布局
    // ------------------------------------------------------------------
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        // ---- 工具条：左侧搜索 · 右侧主操作 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            SearchField {
                id: searchField
                Layout.fillWidth: true
                Layout.maximumWidth: 320
                placeholderText: qsTr("搜索文件（Ctrl+F）")
                onTextChanged: page.filterText = text
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: page.selectedIds.length > 0
                text: qsTr("已选 %1 项").arg(page.selectedIds.length)
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }

            SecondaryButton {
                id: newBtn
                glyph: "＋"
                text: qsTr("新建")
                onClicked: newMenu.popup(newBtn, 0, newBtn.height)
            }
            PrimaryButton {
                glyph: "⬆"
                text: qsTr("上传")
                onClicked: Files.uploadHere()
            }
            // 「打开」：交给系统默认程序。本地没有副本会先下载再打开；
            // 可执行文件（exe/bat/ps1…）由 FileController::openWithSystem 拦截并弹出原因。
            PrimaryButton {
                text: qsTr("打开")
                enabled: page.selectedIds.length > 0 && !page.isDirId(page.selectedIds[0])
                onClicked: Files.openWithSystem(page.selectedIds[0])
            }
            PrimaryButton {
                glyph: "⬇"
                text: qsTr("下载")
                enabled: page.selectedIds.length > 0
                onClicked: Files.downloadSelected()
            }
            // 「操作」下拉：把右键菜单里**针对选中项**的动作搬到文件操作区。
            // 未选中任何文件 → 整颗按钮置灰（"没有选择文件，文件相关功能不可用"）。
            SecondaryButton {
                id: opsBtn
                glyph: "⋯"
                text: qsTr("操作")
                enabled: page.selectedIds.length > 0
                onClicked: opsMenu.popup(opsBtn, 0, opsBtn.height)
                ToolTip.visible: hovered
                ToolTip.text: page.selectedIds.length > 0
                              ? qsTr("对选中的 %1 项执行操作").arg(page.selectedIds.length)
                              : qsTr("先选中文件再操作")
            }
        }

        // ---- 面包屑 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs

            GhostButton {
                glyph: "⬅"
                text: qsTr("上一级")
                enabled: Files.currentDir.length > 0
                onClicked: Files.goUp()
            }

            Repeater {
                model: page.crumbs
                delegate: RowLayout {
                    spacing: Theme.spaceXs
                    readonly property bool isCurrent: index === (page.crumbs.length - 1)

                    // 当前所在层**不再渲染成按钮**：以前用 enabled:false 的 GhostButton，
                    // 但禁用态的观感几乎与可点状态一样 ⇒ 用户点了"根目录"却看不出自己已经在根目录。
                    // 改成明显的"当前位置"标签（主色 + 加粗），一眼就知道自己在哪。
                    Label {
                        visible: parent.isCurrent
                        text: modelData.label
                        color: Theme.primary
                        font.pointSize: Theme.fontBody
                        font.bold: true
                    }
                    GhostButton {
                        visible: !parent.isCurrent
                        text: modelData.label
                        onClicked: Files.enterDir(modelData.path)
                    }
                    Label {
                        visible: !parent.isCurrent
                        text: "/"
                        color: Theme.textSecondary
                        font.pointSize: Theme.fontSecondary
                    }
                }
            }

            Item { Layout.fillWidth: true }
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

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // 表头：比数据行略矮 + 更小更淡的字 + **可点击排序**。
                // 现代表格的层次感来自「表头退后、数据前置」，而排序入口必须在表头上 ——
                // 藏进右键菜单是十年前的做法。
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Theme.tableHeaderHeight
                    color: Theme.tableHeader

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.border
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceM
                        anchors.rightMargin: Theme.spaceM
                        spacing: Theme.spaceS

                        Repeater {
                            // width < 0 表示"占满剩余宽度"（名称列）
                            model: [
                                { label: qsTr("名称"),     key: 0, width: -1,
                                  right: false, shown: true },
                                { label: qsTr("大小"),     key: 1, width: page.colSizeWidth,
                                  right: true,  shown: true },
                                { label: qsTr("修改时间"), key: 2, width: page.colTimeWidth,
                                  right: true,  shown: !page.narrow }
                            ]

                            delegate: Item {
                                id: headCell

                                readonly property var cell: modelData
                                readonly property bool active: page.sortKey === cell.key

                                Layout.fillWidth: cell.width < 0
                                Layout.preferredWidth: cell.width < 0 ? 0 : cell.width
                                Layout.fillHeight: true
                                visible: cell.shown

                                Rectangle {
                                    anchors.fill: parent
                                    color: headMouse.containsMouse ? Theme.hover : "transparent"
                                    Behavior on color { ColorAnimation { duration: Theme.animFast } }
                                }

                                // 表头内容对齐：显式按 x 定位（width = 内容宽），
                                // 不依赖 spacer 的 fillWidth 语义 —— 右对齐列必须与数据列严格对齐。
                                RowLayout {
                                    id: cellRow
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: implicitWidth
                                    x: headCell.cell.right ? parent.width - width : 0
                                    spacing: Theme.spaceXs

                                    Label {
                                        text: headCell.cell.label
                                        color: headCell.active ? Theme.textSecondary : Theme.textTertiary
                                        font.pointSize: Theme.fontMicro
                                        font.bold: headCell.active
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    // 排序方向指示（仅当前排序列显示）
                                    CvIcon {
                                        visible: headCell.active
                                        name: page.sortAsc ? "chevron-up" : "chevron-down"
                                        size: Theme.iconSizeSmall
                                        color: Theme.textSecondary
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                }

                                MouseArea {
                                    id: headMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    // 点同一列 → 切换升降序；点别的列 → 换列并默认升序
                                    onClicked: page.applySort(headCell.cell.key,
                                                              headCell.active ? !page.sortAsc : true)
                                }
                            }
                        }
                    }
                }

                // 列表
                ListView {
                    // 空白处右键：列表里没有命中任何行时才处理，
                    // 命中行时由行自己的 MouseArea 弹"针对该行"的菜单（互不打架）。
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: (ev) => {
                            if (listView.indexAt(ev.position.x, ev.position.y) < 0)
                                page.openBlankMenu()
                        }
                    }
                    id: listView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: proxyModel
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }

                    delegate: Rectangle {
                        id: rowDelegate

                        readonly property bool isDir: page.isDirId(model.fid)
                        readonly property bool isSelected: page.selectedIds.indexOf(model.fid) >= 0

                        width: ListView.view ? ListView.view.width : 0
                        height: Theme.rowHeight
                        color: isSelected ? Theme.selected
                             : (rowMouse.containsMouse ? Theme.hover : "transparent")

                        // 行分隔细线：现代表格的"结构感"来自 1px 极淡的分隔线，
                        // 而不是斑马纹或粗边框。悬停/选中行不画线（避免和背景色打架）。
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.divider
                            visible: !rowDelegate.isSelected && !rowMouse.containsMouse
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: (mouse) => {
                                listView.currentIndex = index
                                if (mouse.button === Qt.LeftButton) {
                                    page.selectRow(index,
                                                   (mouse.modifiers & Qt.ControlModifier) !== 0)
                                } else {
                                    page.ctxId = model.fid
                                    page.ctxName = model.name
                                    page.ctxIsDir = rowDelegate.isDir
                                    page.ctxDir = model.dir
                                    const p = rowMouse.mapToItem(page, mouse.x, mouse.y)
                                    ctxMenu.popup(page, p.x, p.y)
                                }
                            }
                            onDoubleClicked: page.openRow(index)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceM
                            anchors.rightMargin: Theme.spaceM
                            spacing: Theme.spaceS

                            CvIcon {
                                Layout.alignment: Qt.AlignVCenter
                                name: rowDelegate.isDir ? "folder" : "file"
                                size: Theme.iconSize
                                color: Theme.textTertiary
                            }

                            Label {
                                Layout.fillWidth: true
                                text: model.name
                                color: Theme.textPrimary
                                font.pointSize: Theme.fontBody
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.preferredWidth: page.colSizeWidth
                                horizontalAlignment: Text.AlignRight
                                text: rowDelegate.isDir ? "—" : model.size
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                elide: Text.ElideRight
                            }

                            Label {
                                visible: !page.narrow
                                Layout.preferredWidth: page.colTimeWidth
                                horizontalAlignment: Text.AlignRight
                                text: model.time
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // ---- 空态 ----
                    EmptyState {
                        anchors.centerIn: parent
                        width: parent.width
                        visible: listView.count === 0 && !Files.loading && page.listError.length === 0
                        glyph: page.filterText.length > 0 ? "🔍" : "📭"
                        title: page.filterText.length > 0
                               ? qsTr("没有匹配「%1」的文件").arg(page.filterText)
                               : qsTr("还没有文件")
                        description: page.filterText.length > 0
                                     ? qsTr("换个关键字，或清空搜索框。")
                                     : qsTr("把文件拖到这里，或点击下方按钮上传。")

                        PrimaryButton {
                            glyph: "⬆"
                            text: qsTr("上传文件")
                            onClicked: Files.uploadHere()
                        }
                        SecondaryButton {
                            glyph: "⟳"
                            text: qsTr("刷新")
                            // 走统一入口 refreshNow()：清「已暂停」再刷新。
                            // 若直接 Files.refresh()，在 autoRefreshPaused=true（上一次失败）时
                            // 会留下「看不见的暂停」——轮询永远停摆且已无按钮显示它。
                            onClicked: page.refreshNow()
                        }
                    }

                    // ---- 错误态（可重试）----
                    EmptyState {
                        anchors.centerIn: parent
                        width: parent.width
                        visible: listView.count === 0 && !Files.loading && page.listError.length > 0
                        glyph: "⚠️"
                        glyphColor: Theme.danger
                        title: qsTr("加载失败")
                        description: page.listError

                        PrimaryButton {
                            glyph: "⟳"
                            text: qsTr("重试")
                            // 同空态：必须走 refreshNow() 清暂停，否则重试成功后轮询仍停在暂停态。
                            onClicked: page.refreshNow()
                        }
                        SecondaryButton {
                            text: qsTr("前往设置检查令牌")
                            onClicked: page.requestSettings()
                        }
                    }

                    // ---- 加载态 ----
                    ColumnLayout {
                        anchors.centerIn: parent
                        visible: listView.count === 0 && Files.loading
                        spacing: Theme.spaceS
                        BusyIndicator {
                            Layout.alignment: Qt.AlignHCenter
                            running: Files.loading
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

            // ---- 拖拽上传 ----
            DropArea {
                anchors.fill: parent
                onDropped: (drop) => {
                    const paths = []
                    for (let i = 0; i < drop.urls.length; ++i)
                        paths.push(page.toLocalPath(drop.urls[i]))
                    if (paths.length > 0)
                        Transfers.upload(paths, Files.currentDir)
                }
            }
        }
    }

    // ------------------------------------------------------------------
    //  菜单
    // ------------------------------------------------------------------
    Menu {
        id: newMenu
        width: 180
        MenuItem { text: qsTr("新建文件"); onTriggered: newFileDialog.openFor("file") }
        MenuItem { text: qsTr("新建文件夹"); onTriggered: newFileDialog.openFor("folder") }
    }

    /*!
        右键菜单：四组（操作 / 修改 / 新建 / 排序），组间用 MenuSeparator 分隔。
        · 图标用内联字形（不引入外部资源）
        · 删除为 danger 色，且目录行置灰（服务端目录无删除语义）
        · 排序为嵌套 Menu（Qt6：Menu 嵌 Menu 自动成为子菜单；MenuItem.menu 只读）
    */
    // 工具栏「操作」下拉的内容：与右键菜单同源，但**以选中项为目标**
    // （工具栏没有"右键目标"这回事）。未选中/多选时逐项自己判定可用性。
    // 新建/重命名/移动成功后稍等再刷新：这些请求是异步的，立刻刷新会拿不到新条目
    Timer {
        id: createdRefreshTimer
        interval: 400
        onTriggered: page.refreshNow()
    }

    Menu {
        id: opsMenu
        width: 224
        MenuItem {
            text: qsTr("分享…")
            enabled: page.selectedIds.length === 1
            onTriggered: {
                const id = page.selectedIds[0]
                shareDialog.openFor(id, page.nameForId(id))
            }
        }
        MenuItem {
            text: qsTr("标签…")
            enabled: page.selectedIds.length === 1 && !page.isDirId(page.selectedIds[0])
            onTriggered: {
                const id = page.selectedIds[0]
                tagAssignDialog.openFor(id, page.nameForId(id))
            }
        }
        MenuItem {
            text: qsTr("版本历史…")
            enabled: page.selectedIds.length === 1 && !page.isDirId(page.selectedIds[0])
            onTriggered: {
                const id = page.selectedIds[0]
                page.requestVersions(id, page.nameForId(id))
            }
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("重命名…")
            enabled: page.selectedIds.length === 1
            onTriggered: {
                const id = page.selectedIds[0]
                const nm = page.nameForId(id)
                // 文件夹改的是**路径**（两个数据源的目录 id 口径不同）
                if (page.isDirId(id))
                    folderRenameDialog.openFor(page.dirPathOf(id), nm)
                else
                    renameDialog.openFor(id, nm)
            }
        }
        MenuItem {
            text: page.selectedIds.length > 1
                  ? qsTr("移动到…（%1 项）").arg(page.selectedIds.length)
                  : qsTr("移动到…")
            enabled: page.selectedIds.length > 0
            onTriggered: {
                // 对话框只需要一个"名字"做展示；真正搬多少个由 onSubmit 里按选中数分派。
                const n = page.selectedIds.length
                const label = n > 1 ? qsTr("选中的 %1 项").arg(n)
                                    : page.nameForId(page.selectedIds[0])
                moveToDialog.openFor(page.selectedIds[0], label)
            }
        }
        MenuItem {
            text: page.selectedIds.length > 1
                  ? qsTr("移入回收站（%1 项）").arg(page.selectedIds.length)
                  : qsTr("移入回收站")
            enabled: page.selectedIds.length > 0
            onTriggered: {
                if (page.selectedIds.length > 1) {
                    // 批量：直接进回收站（**可恢复**，所以这里不弹确认；
                    // 真正的永久删除收敛在回收站页且带二次确认）
                    Files.removeSelected()
                } else {
                    const id = page.selectedIds[0]
                    deleteDialog.openFor(id, page.nameForId(id))
                }
            }
        }
    }

    Menu {
        id: ctxMenu
        width: 224

        // ---- 组0 刷新（替代原工具栏【刷新】按钮，始终可用；右键空白处亦可触发）----
        MenuItem {
            text: "⟳  " + qsTr("刷新")
            onTriggered: page.refreshNow()
        }

        MenuSeparator { }

        // ---- 组1 操作 ----
        MenuItem {
            // 用系统默认程序打开；可执行文件会被拦截（原因见 C++ 侧文案）
            text: qsTr("打开（系统默认程序）")
            enabled: page.ctxId.length > 0 && !page.ctxIsDir
            onTriggered: Files.openWithSystem(page.ctxId)
        }
        MenuItem {
            id: miOpen
            text: (page.ctxIsDir ? qsTr("打开文件夹") : qsTr("打开下载目录"))
            onTriggered: {
                if (page.ctxIsDir)
                    Files.enterDir(page.dirPathOf(page.ctxId))
                else
                    Transfers.openLocalFolder()
            }
        }
        MenuItem {
            text: qsTr("下载")
            enabled: !page.ctxIsDir
            onTriggered: Transfers.download([page.ctxId], "")
        }
        MenuItem {
            // 新建分享的唯一入口（分享页只管理已有分享）
            text: qsTr("分享…")
            // 仅当「恰好选中 1 项」时可用：0 个 / 多选 → 禁用
            enabled: page.selectedIds.length === 1
            onTriggered: {
                const id = page.selectedIds[0]
                shareDialog.openFor(id, page.nameForId(id))
            }
        }
        MenuItem {
            // 给单个文件打标签。用右键目标（ctxId）而非 selectedIds：
            // 右键不会改变选中集，若用 selectedIds，"右键未选中的行"就会打错目标。
            text: qsTr("标签…")
            enabled: page.ctxId.length > 0 && !page.ctxIsDir
            onTriggered: tagAssignDialog.openFor(page.ctxId, page.ctxName)
        }
        MenuItem {
            // 查看单个文件的历史版本。目录没有版本概念 → 禁用
            text: qsTr("版本历史…")
            enabled: page.ctxId.length > 0 && !page.ctxIsDir
            onTriggered: page.requestVersions(page.ctxId, page.ctxName)
        }
        MenuSeparator { }

        // ---- 组2 修改 ----
        MenuItem {
            text: page.ctxIsDir ? qsTr("重命名文件夹") : qsTr("重命名")
            enabled: page.ctxId.length > 0
            onTriggered: {
                if (page.ctxIsDir) {
                    // 文件夹传**路径**（目录在两个数据源里的 id 口径不同，path 才是共同标识）
                    const p = page.ctxDir.length > 0
                              ? page.ctxDir + "/" + page.ctxName
                              : page.ctxName
                    folderRenameDialog.openFor(p, page.ctxName)
                } else {
                    renameDialog.openFor(page.ctxId, page.ctxName)
                }
            }
        }
        MenuItem {
            // 移动到另一个文件夹（目录树 CRUD）。文件夹也支持 —— 整棵子树跟着走。
            text: qsTr("移动到…")
            enabled: page.ctxId.length > 0
            onTriggered: moveToDialog.openFor(page.ctxId, page.ctxName)
        }
        MenuItem {
            id: miDelete
            // ⚠️ 语义已变更：这里不再是永久删除，而是**移入回收站**（可恢复）。
            //    真正的永久删除收敛到回收站页的「彻底删除」，入口唯一且带二次确认。
            //    目录也可删除：服务端支持整个目录进站，其下文件一并标记，
            //    回收站里看到的是那个文件夹，而不是散开的 N 个文件。
            text: page.selectedIds.length > 1
                  ? qsTr("移入回收站（选中 %1 项）").arg(page.selectedIds.length)
                  : qsTr("移入回收站")
            enabled: page.ctxId.length > 0 || page.selectedIds.length > 1
            onTriggered: {
                if (page.selectedIds.length > 1)
                    Files.removeSelected()
                else
                    deleteDialog.openFor(page.ctxId, page.ctxName)
            }
            contentItem: Text {
                text: miDelete.text
                font: miDelete.font
                color: miDelete.enabled ? Theme.danger : Theme.disabledText
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }

        MenuSeparator { }

        // ---- 组3 新建 ----
        MenuItem { text: qsTr("新建文件"); onTriggered: newFileDialog.openFor("file") }
        MenuItem { text: qsTr("新建文件夹"); onTriggered: newFileDialog.openFor("folder") }
        MenuItem { text: qsTr("上传到此目录"); onTriggered: Files.uploadHere() }

        MenuSeparator { }

        // ---- 组4 排序（子菜单，单选）----
        Menu {
            title: qsTr("排序")
            MenuItem {
                text: qsTr("名称")
                checkable: true
                checked: page.sortKey === 0
                onTriggered: page.applySort(0)
            }
            MenuItem {
                text: qsTr("大小")
                checkable: true
                checked: page.sortKey === 1
                onTriggered: page.applySort(1)
            }
            MenuItem {
                text: qsTr("时间")
                checkable: true
                checked: page.sortKey === 2
                onTriggered: page.applySort(2)
            }
            MenuSeparator { }
            MenuItem {
                text: qsTr("升序")
                checkable: true
                checked: page.sortAsc
                onTriggered: page.applySort(page.sortKey, true)
            }
            MenuItem {
                text: qsTr("降序")
                checkable: true
                checked: !page.sortAsc
                onTriggered: page.applySort(page.sortKey, false)
            }
        }
    }

    // ------------------------------------------------------------------
    //  对话框与文件选择
    // ------------------------------------------------------------------
    NewFileDialog {
        id: newFileDialog
        onSubmit: (name, kind) => {
            if (kind === "folder")
                // 新建是异步请求：立刻刷新会拿不到新条目，所以要等它落地再刷。
                // （宁可多刷一次，也不要让用户以为"新建没生效"。）
                Files.createFolder(name)
            else
                Files.createFile(name)
            createdRefreshTimer.restart()
        }
    }

    // 病毒查杀结果（内联 Dialog：不新增文件、不动 qrc）
    Dialog {
        id: scanDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, (parent ? parent.width - Theme.spaceXl * 2 : 440))
        padding: Theme.spaceL
        title: qsTr("病毒查杀结果")
        standardButtons: Dialog.NoButton

        property string verdictText: ""
        property string detailText: ""
        property bool   blocked: false
        property bool   remoteFailed: false
        property bool   failed: false

        contentItem: ColumnLayout {
            spacing: Theme.spaceS

            Label {
                Layout.fillWidth: true
                text: scanDialog.failed
                      ? qsTr("查杀未完成")
                      : qsTr("判定：%1").arg(scanDialog.verdictText)
                color: (scanDialog.failed || scanDialog.blocked) ? Theme.danger : Theme.textPrimary
                font.pointSize: Theme.fontBody
                font.bold: true
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                text: scanDialog.detailText
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                visible: scanDialog.remoteFailed && !scanDialog.failed
                text: qsTr("服务器这次没能从远端拿到结论（网络或未配置 API Key）。"
                           + "这不代表文件安全，只是这次没查成。")
                color: Theme.textTertiary
                font.pointSize: Theme.fontMicro
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                PrimaryButton {
                    text: qsTr("知道了")
                    onClicked: scanDialog.close()
                }
            }
        }
    }

    Connections {
        target: Files
        function onScanFinished(fileId, verdictText, detail, blocked, remoteFailed, ok, error) {
            scanDialog.failed       = !ok
            scanDialog.verdictText  = verdictText
            scanDialog.detailText   = ok ? detail : error
            scanDialog.blocked      = blocked
            scanDialog.remoteFailed = remoteFailed
            scanDialog.open()
        }
    }

    // 标签变更后自动刷新文件列表（否则打了标签要手动刷新才看得到）
    Connections {
        target: (typeof Tags !== "undefined") ? Tags : null
        function onFileTagsAssigned(fileId) { page.refreshNow() }
    }

    RenameDialog {
        id: renameDialog
        onSubmit: (id, name) => Files.rename(id, name)
    }

    // 文件夹重命名：复用同一个 RenameDialog（只换文案），避免复制一份对话框出来。
    // 注意这里 submit 的第 1 个参数是**文件夹路径**而不是 id —— 上面的 openFor 传的就是路径。
    RenameDialog {
        id: folderRenameDialog
        dialogTitle: qsTr("重命名文件夹")
        hintText: qsTr("输入新的文件夹名称（不能包含 '/' 或 '\\'）")
        okText: qsTr("重命名")
        onSubmit: (dirPath, name) => Files.renameFolder(dirPath, name)
    }

    MoveToDialog {
        id: moveToDialog
        // 多选时走批量（一次请求搬 N 个）；单选保持原路径
        onSubmit: (id, targetId) => {
            if (page.selectedIds.length > 1)
                Files.moveSelectedTo(targetId)
            else
                Files.moveTo(id, targetId)
        }
    }

    /*!
        删除确认框：语义 = **移入回收站**（软删除，可恢复），因此用默认语气（非 danger 红）。
        文案由对话框自身提供（含"不再计入已用空间、但磁盘尚未释放"这句），这里不再重复。
        ⚠️ 不要再改成调用 Files.purge / Backend::purge —— 那是不可恢复的彻底删除，
           入口只保留在回收站页（唯一且带二次确认）。
    */
    DeleteConfirmDialog {
        id: deleteDialog
        onConfirmed: (id) => Files.remove(id)
    }

    // 标签关联（右键「标签…」入口）：针对单个文件的标签全量提交
    TagAssignDialog {
        id: tagAssignDialog
    }

    // 新建分享（右键「分享…」入口）
    ShareDialog {
        id: shareDialog
    }

    FileDialog {
        id: uploadDialog
        property string pendingDir: ""
        title: qsTr("选择要上传的文件（可多选）")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("所有文件 (*)")]
        onAccepted: {
            const paths = []
            const files = selectedFiles
            for (let i = 0; i < files.length; ++i)
                paths.push(page.toLocalPath(files[i]))
            if (paths.length > 0)
                Transfers.upload(paths, uploadDialog.pendingDir)
        }
    }
}
