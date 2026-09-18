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

    // 排序状态（与模型内排序保持一致）
    property int sortKey: 2
    property bool sortAsc: false

    // 列表错误（仅 count===0 时呈现错误态）
    property string listError: ""

    // 面包屑
    property var crumbs: []

    // 请求跳转到设置页（错误态「检查令牌」引导）
    signal requestSettings()

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
        注意：不在此处（Component.onCompleted）调用 Files.refresh()，
        以免与 Main.qml 启动时已有的 Files.refresh() 重复。
    */
    Timer {
        id: autoRefreshTimer
        interval: Math.max(5, App.autoRefreshInterval) * 1000
        repeat: true
        running: App.autoRefresh
                 && page.visible
                 && !Files.loading
                 && !page.autoRefreshPaused
        onTriggered: Files.refresh()
    }

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
    readonly property int colStatusWidth: 76

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
        function onCountChanged() { page.rebuild() }
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
                page.notify(qsTr("自动刷新已暂停：服务器不可达，可点【刷新】重试"), false)
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

            GhostButton {
                glyph: "⟳"
                text: qsTr("刷新")
                onClicked: {
                    // 手动重试：先解除暂停再刷新，使自动刷新重新武装；
                    // 若仍失败，onRefreshFinished(false) 会立即再次暂停。
                    page.autoRefreshPaused = false
                    Files.refresh()
                }
            }

            // 自动刷新状态按钮（三态）。点击切换 App.autoRefresh；开启时清除暂停态。
            GhostButton {
                text: !App.autoRefresh
                      ? qsTr("自动 ⟳ 关")
                      : (page.autoRefreshPaused
                         ? qsTr("自动 ⟳ 已暂停")
                         : qsTr("自动 ⟳ %1s").arg(App.autoRefreshInterval))
                onClicked: {
                    App.autoRefresh = !App.autoRefresh
                    if (App.autoRefresh)
                        page.autoRefreshPaused = false
                }
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
            PrimaryButton {
                glyph: "⬇"
                text: qsTr("下载")
                enabled: page.selectedIds.length > 0
                onClicked: Files.downloadSelected()
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
                    GhostButton {
                        text: modelData.label
                        enabled: index !== (page.crumbs.length - 1)
                        onClicked: Files.enterDir(modelData.path)
                    }
                    Label {
                        visible: index !== (page.crumbs.length - 1)
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

                // 表头
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Theme.rowHeight
                    color: Theme.hover
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceM
                        anchors.rightMargin: Theme.spaceM
                        spacing: Theme.spaceS

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("名称")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            font.bold: true
                        }
                        Label {
                            Layout.preferredWidth: page.colSizeWidth
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("大小")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            font.bold: true
                        }
                        Label {
                            visible: !page.narrow
                            Layout.preferredWidth: page.colTimeWidth
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("修改时间")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            font.bold: true
                        }
                        Label {
                            Layout.preferredWidth: page.colStatusWidth
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("状态")
                            color: Theme.textSecondary
                            font.pointSize: Theme.fontSecondary
                            font.bold: true
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                // 列表
                ListView {
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

                            Text {
                                text: rowDelegate.isDir ? "📁" : "📄"
                                font.pointSize: Theme.fontBody
                                color: Theme.textSecondary
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

                            Item {
                                Layout.preferredWidth: page.colStatusWidth
                                implicitHeight: Theme.rowHeight
                                StatusBadge {
                                    anchors.centerIn: parent
                                    visible: model.status === "是"
                                    tone: "success"
                                    text: qsTr("秒传")
                                }
                                Label {
                                    anchors.centerIn: parent
                                    visible: model.status !== "是"
                                    text: "-"
                                    color: Theme.textSecondary
                                    font.pointSize: Theme.fontSecondary
                                }
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
                            onClicked: Files.refresh()
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
                            onClicked: Files.refresh()
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
        MenuItem { text: "📄  " + qsTr("新建文件"); onTriggered: newFileDialog.openFor("file") }
        MenuItem { text: "📁  " + qsTr("新建文件夹"); onTriggered: newFileDialog.openFor("folder") }
    }

    /*!
        右键菜单：四组（操作 / 修改 / 新建 / 排序），组间用 MenuSeparator 分隔。
        · 图标用内联字形（不引入外部资源）
        · 删除为 danger 色，且目录行置灰（服务端目录无删除语义）
        · 排序为嵌套 Menu（Qt6：Menu 嵌 Menu 自动成为子菜单；MenuItem.menu 只读）
    */
    Menu {
        id: ctxMenu
        width: 224

        // ---- 组1 操作 ----
        MenuItem {
            id: miOpen
            text: "📂  " + (page.ctxIsDir ? qsTr("打开文件夹") : qsTr("打开下载目录"))
            onTriggered: {
                if (page.ctxIsDir)
                    Files.enterDir(page.dirPathOf(page.ctxId))
                else
                    Transfers.openLocalFolder()
            }
        }
        MenuItem {
            text: "⬇  " + qsTr("下载")
            enabled: !page.ctxIsDir
            onTriggered: Transfers.download([page.ctxId], "")
        }

        MenuSeparator { }

        // ---- 组2 修改 ----
        MenuItem {
            text: "✏  " + qsTr("重命名")
            enabled: !page.ctxIsDir
            onTriggered: renameDialog.openFor(page.ctxId, page.ctxName)
        }
        MenuItem {
            id: miDelete
            text: "🗑  " + qsTr("删除")
            enabled: !page.ctxIsDir
            onTriggered: deleteDialog.openFor(page.ctxId, page.ctxName)
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
        MenuItem { text: "📄  " + qsTr("新建文件"); onTriggered: newFileDialog.openFor("file") }
        MenuItem { text: "📁  " + qsTr("新建文件夹"); onTriggered: newFileDialog.openFor("folder") }
        MenuItem { text: "⬆  " + qsTr("上传到此目录"); onTriggered: Files.uploadHere() }

        MenuSeparator { }

        // ---- 组4 排序（子菜单，单选）----
        Menu {
            title: "↕  " + qsTr("排序")
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
                Files.createFolder(name)
            else
                Files.createFile(name)
        }
    }

    RenameDialog {
        id: renameDialog
        onSubmit: (id, name) => Files.rename(id, name)
    }

    DeleteConfirmDialog {
        id: deleteDialog
        onConfirmed: (id) => Files.remove(id)
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
