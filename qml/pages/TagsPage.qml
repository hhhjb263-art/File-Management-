import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"
import "../dialogs"

/*!
    标签页：管理标签本身 + 按标签看文件。

    两件事在这里合体（这也是 TagController 把"标签增删查"与"文件↔标签关联"合并的原因）：
      ① 标签自身：新建（名称 + 颜色）、删除、看每个标签关联了多少文件；
      ② 通过标签**反查文件**：点某个标签 → 列出所有打了该标签的文件。

    ⚠️ 一个文件可以有多个标签，且标签**不改变文件位置** —— 这是标签与文件夹的根本区别，
       页面文案必须说清楚，否则用户会拿它当"另一个文件夹"用然后困惑。

    ⚠️ 数据源：TagModel（角色 id/name/color/count）。
       Tags.matchedFiles() 是 Q_INVOKABLE 函数调用，QML 无法自动追踪其依赖 →
       用自增 matchesVersion 作为显式依赖，重取值（与 SharesPage 同一手法）。

    ⚠️ 所有 Tags.* 访问都用 typeof 守卫 —— C++ 未落地时本页退化为占位态，绝不抛错。
*/
Item {
    id: page

    // ---- 冻结 API 守卫 ----
    readonly property bool apiReady: (typeof Tags !== "undefined")
    readonly property bool supported: apiReady
                                      && (Tags.supported === undefined ? true : Tags.supported)
    readonly property bool tagReady: apiReady && (Tags.supported === true)
    readonly property bool modelReady: (typeof TagModel !== "undefined")

    readonly property bool tagBusy: tagReady && Tags.busy !== undefined ? Tags.busy : false
    readonly property int  tagCount: tagReady && Tags.count !== undefined ? Tags.count : 0
    readonly property int  matchCount: tagReady && Tags.matchCount !== undefined ? Tags.matchCount : 0
    readonly property string filterTagId: tagReady && Tags.filterTagId !== undefined
                                          ? String(Tags.filterTagId) : ""
    readonly property string emptyText: tagReady && Tags.emptyText !== undefined
                                        ? String(Tags.emptyText) : ""

    // at()/matchedFiles() 的函数式取值需要显式依赖
    property int matchesVersion: 0

    // 页内就近反馈
    property string statusText: ""
    property bool   statusOk: true

    // 新建标签的表单
    property string newColor: "#4C6FFF"
    readonly property var palette: ["#4C6FFF", "#0F7A54", "#A2600A", "#C43D3D",
                                    "#7A5AF8", "#1A6FA8", "#B5408A", "#5B6472"]

    // 搜索关键字（非空 = 按关键词检索；两者都作用到同一份 matchedFiles）
    property string searchText: ""

    readonly property bool showResults: page.filterTagId.length > 0
                                        || page.searchText.length > 0

    function setStatus(text, ok) {
        page.statusText = String(text)
        page.statusOk = (ok !== false)
    }

    function doRefresh() {
        if (page.tagReady && typeof Tags.refresh === "function")
            Tags.refresh()
    }

    function tagAt(i) {
        if (!page.modelReady || i < 0 || i >= TagModel.count)
            return ({})
        const t = TagModel.at(i)
        return (t === undefined || t === null) ? ({}) : t
    }

    function doCreate() {
        const n = newTagField.text.trim()
        if (n.length === 0) {
            page.setStatus(qsTr("标签名不能为空"), false)
            return
        }
        page.setStatus("", true)
        if (page.tagReady && typeof Tags.create === "function")
            Tags.create(n, page.newColor)
        newTagField.text = ""
    }

    function doRemove(id, name) {
        page.setStatus(qsTr("正在删除标签「%1」…").arg(String(name)), true)
        if (page.tagReady && typeof Tags.remove === "function")
            Tags.remove(String(id))
    }

    // 点标签 = 切换筛选（再点一次取消）
    function toggleFilter(id) {
        if (!page.tagReady || typeof Tags.applyFilter !== "function")
            return
        const same = (String(id) === page.filterTagId)
        Tags.applyFilter(same ? "" : String(id))
    }

    function doSearch(text) {
        page.searchText = String(text)
        if (!page.tagReady || typeof Tags.search !== "function")
            return
        // 关键词检索时不叠加标签筛选：两者都写同一个 matchedFiles，混在一起会互相覆盖。
        // 用"最后一次操作生效"的简单规则，比让用户猜哪个优先更可预期。
        if (page.searchText.length > 0) {
            if (typeof Tags.applyFilter === "function")
                Tags.applyFilter("")
            Tags.search(page.searchText)
        } else if (page.filterTagId.length > 0) {
            Tags.applyFilter(page.filterTagId)
        } else {
            Tags.search("")
        }
    }

    function matchedAt(i) {
        if (!page.tagReady || typeof Tags.matchedFiles !== "function")
            return ({})
        // 显式读取 matchesVersion：函数体内对属性的读取同样会被 QML 记入当前绑定的
        // 依赖集，结果变化时委托才会重新取值（恒假分支，只为建立依赖）。
        if (page.matchesVersion < 0)
            return ({})
        const list = Tags.matchedFiles()
        if (i < 0 || i >= list.length)
            return ({})
        return list[i]
    }

    // 进页面时刷新一次
    onVisibleChanged: {
        if (page.visible)
            page.doRefresh()
    }

    Connections {
        target: page.tagReady ? Tags : null
        function onMatchesChanged() { page.matchesVersion++ }
        function onTagsChanged() { page.matchesVersion++ }
        function onFilterChanged() {
            page.matchesVersion++
            if (page.filterTagId.length > 0)
                page.searchFieldClear()
        }
        function onErrorOccurred(message) { page.setStatus(String(message), false) }
        function onStatusMessage(message, ok) {
            if (String(message).length > 0)
                page.setStatus(String(message), ok !== false)
        }
    }

    // 进入某个标签筛选时，清掉搜索框（避免"搜索框里有字但结果是标签筛选"的错位感）
    function searchFieldClear() {
        if (page.searchText.length > 0) {
            page.searchText = ""
            searchField.text = ""
        }
    }

    // ==================================================================
    //  未就绪兜底：原有占位页（保留既有，勿删）
    // ==================================================================
    UnsupportedPage {
        anchors.fill: parent
        visible: !page.supported
        featureName: qsTr("标签")
        glyph: "🏷"
        notice: (typeof Tags !== "undefined" && Tags.unsupportedNotice !== undefined
                 && String(Tags.unsupportedNotice).length > 0)
                ? Tags.unsupportedNotice
                : qsTr("当前服务端不支持标签。")
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
                    text: qsTr("标签")
                    color: Theme.textPrimary
                    font.pointSize: Theme.fontTitle + 3
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("标签是给文件打的横向分类：一个文件可以有多个，"
                               + "且不改变文件原有的位置。")
                          + "\n" + qsTr("点标签即可查看该标签下的全部文件；"
                                        + "给文件打标签请到「文件」页右键文件 →「标签…」。")
                    color: Theme.textSecondary
                    font.pointSize: Theme.fontSecondary
                    wrapMode: Text.WordWrap
                }
            }

            SearchField {
                id: searchField
                Layout.preferredWidth: 220
                placeholderText: qsTr("搜索文件")
                onTextChanged: page.doSearch(text)
            }

            GhostButton {
                glyph: "⟳"
                text: qsTr("刷新")
                enabled: !page.tagBusy
                onClicked: page.doRefresh()
            }
        }

        // ---- 就近状态 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            visible: page.statusText.length > 0

            StatusBadge {
                text: page.statusText
                tone: page.statusOk ? "success" : "danger"
            }
            Item { Layout.fillWidth: true }
        }

        // ---- 新建标签 ----
        SectionCard {
            Layout.fillWidth: true
            title: qsTr("新建标签")
            iconName: "plus"

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS

                TextField {
                    id: newTagField
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeight
                    placeholderText: qsTr("标签名称（例如：工作、待整理、2026）")
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textTertiary
                    selectByMouse: true
                    font.pointSize: Theme.fontBody
                    enabled: !page.tagBusy
                    onAccepted: page.doCreate()
                }

                RowLayout {
                    spacing: Theme.spaceXs
                    Repeater {
                        model: page.palette
                        delegate: Rectangle {
                            width: 16
                            height: 16
                            radius: 8
                            color: modelData
                            border.width: page.newColor === modelData ? 2 : 0
                            border.color: Theme.textPrimary
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.newColor = modelData
                            }
                        }
                    }
                }

                PrimaryButton {
                    iconName: "plus"
                    text: qsTr("创建")
                    enabled: !page.tagBusy
                    onClicked: page.doCreate()
                }
            }
        }

        // ---- 标签列表 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true
            visible: page.tagCount > 0

            ListView {
                id: tagList
                anchors.fill: parent
                anchors.margins: Theme.spaceXs
                clip: true
                model: page.modelReady ? TagModel : null
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }

                delegate: Rectangle {
                    id: tagDelegate

                    readonly property bool picked: String(model.id) === page.filterTagId
                    readonly property string tagColor: (model.color !== undefined
                                                        && String(model.color).length > 0)
                                                       ? String(model.color) : Theme.primary

                    width: ListView.view ? ListView.view.width : 0
                    height: Theme.rowHeight + Theme.spaceS
                    radius: Theme.radiusControl
                    color: tagDelegate.picked ? Theme.selected
                         : (tagMouse.containsMouse ? Theme.hover : "transparent")

                    // ⚠️ 整行点击区必须在**按钮之前**声明：QML 里后声明的子项在上层。
                    //    若把它放到按钮后面，它会盖住按钮、把所有点击都吃掉
                    //    （表现就是"点删除没反应，反而切换了筛选"）。
                    MouseArea {
                        id: tagMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.toggleFilter(model.id)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceM
                        anchors.rightMargin: Theme.spaceM
                        spacing: Theme.spaceS

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            width: 12
                            height: 12
                            radius: 3
                            color: tagDelegate.tagColor
                        }
                        Label {
                            Layout.fillWidth: true
                            text: model.name
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontBody
                            font.bold: tagDelegate.picked
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        StatusBadge {
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("%1 个文件").arg(model.count)
                            tone: "neutral"
                        }
                        SecondaryButton {
                            text: tagDelegate.picked ? qsTr("取消筛选") : qsTr("查看文件")
                            enabled: !page.tagBusy
                            onClicked: page.toggleFilter(model.id)
                        }
                        DangerButton {
                            iconName: "trash"
                            text: qsTr("删除")
                            enabled: !page.tagBusy
                            onClicked: page.doRemove(model.id, model.name)
                        }
                    }
                }
            }
        }

        // ---- 空态 ----
        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.tagCount === 0
            iconName: "tag"
            title: qsTr("还没有标签")
            description: page.emptyText.length > 0
                         ? page.emptyText
                         : qsTr("标签是给文件打的横向分类，一个文件可以有多个。")
            glyphColor: Theme.textTertiary
        }

        // ---- 结果区（标签筛选 / 关键词检索命中）----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(260, page.height * 0.36)
            visible: page.showResults
            radius: Theme.radiusCard
            color: Theme.card
            border.width: 1
            border.color: Theme.border
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

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

                        Label {
                            Layout.fillWidth: true
                            text: page.filterTagId.length > 0
                                  ? qsTr("该标签下的文件")
                                  : qsTr("搜索结果")
                            color: Theme.textTertiary
                            font.pointSize: Theme.fontMicro
                        }
                        Label {
                            text: qsTr("%1 个").arg(page.matchCount)
                            color: Theme.textTertiary
                            font.pointSize: Theme.fontMicro
                        }
                        GhostButton {
                            iconName: "close"
                            onClicked: {
                                page.searchText = ""
                                searchField.text = ""
                                if (page.tagReady && typeof Tags.applyFilter === "function")
                                    Tags.applyFilter("")
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("清除结果")
                        }
                    }
                }

                ListView {
                    id: resultList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: Math.max(0, page.matchCount)
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }

                    delegate: Rectangle {
                        readonly property var m: page.matchedAt(index)

                        width: resultList.width
                        height: Theme.rowHeight
                        color: resultMouse.containsMouse ? Theme.hover : "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.divider
                            visible: !resultMouse.containsMouse
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceM
                            anchors.rightMargin: Theme.spaceM
                            spacing: Theme.spaceS

                            CvIcon {
                                Layout.alignment: Qt.AlignVCenter
                                name: m.isDir === true ? "folder" : "file"
                                size: Theme.iconSize
                                color: Theme.textSecondary
                            }
                            Label {
                                Layout.fillWidth: true
                                text: m.name !== undefined ? String(m.name) : ""
                                color: Theme.textPrimary
                                font.pointSize: Theme.fontBody
                                elide: Text.ElideMiddle
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.preferredWidth: 200
                                text: (m.path !== undefined && String(m.path).length > 0)
                                      ? String(m.path) : "/"
                                color: Theme.textTertiary
                                font.pointSize: Theme.fontSecondary
                                elide: Text.ElideLeft
                                verticalAlignment: Text.AlignVCenter
                            }
                            Label {
                                Layout.preferredWidth: 90
                                text: m.sizeText !== undefined ? String(m.sizeText) : "-"
                                color: Theme.textSecondary
                                font.pointSize: Theme.fontSecondary
                                horizontalAlignment: Text.AlignRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        MouseArea {
                            id: resultMouse
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                }

                // 结果区内空态：筛选/搜索没命中时别留一片空白
                Label {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: page.matchCount === 0
                    text: qsTr("没有匹配的文件")
                    color: Theme.textTertiary
                    font.pointSize: Theme.fontSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
