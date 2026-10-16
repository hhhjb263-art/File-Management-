import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../controls"

/*!
    标签关联对话框：给「一个文件」勾选标签（全量提交）。

    为什么是对话框而不是文件页内联编辑：
      文件列表行高只有 34px，塞不下可变的标签集合；而标签是"横向分类"，
      本质上是在给单个对象做多选，对话框是最不容易出错的载体。

    ⚠️ 全量语义：保存时提交的是**勾选后的完整集合**（PUT /api/v1/files/:id/tags），
       不是增量。所以服务端只需一次"先清空再重建"，不需要 diff，也不会出现
       "取消勾选却删不掉"的经典麻烦。

    ⚠️ 选中态以服务端为准：打开时先拉一次该文件当前的标签（loadFileTags），
       期间显示加载态并禁用保存 —— 否则用户可能把"还没加载完的空集合"当默认值提交，
       一保存就把原有标签全抹了。
*/
Dialog {
    id: dlg

    property string fileId: ""
    property string fileName: ""

    // 当前勾选的标签 id（字符串数组）
    property var selectedIds: []
    // 加载中：正在取该文件的已有标签（此期间禁止保存，见文件头说明）
    property bool loading: false
    // 待选中的新标签名：刚创建的标签回包只有名字，列表刷新后按名字把它勾上
    property string pendingName: ""
    // 页内错误
    property string errorText: ""

    readonly property bool apiReady: (typeof Tags !== "undefined")
    readonly property bool tagReady: apiReady && (Tags.supported === true)
    readonly property bool modelReady: (typeof TagModel !== "undefined")
    readonly property bool tagBusy: tagReady && Tags.busy !== undefined ? Tags.busy : false
    readonly property int  tagCount: tagReady && Tags.count !== undefined ? Tags.count : 0
    readonly property string emptyText: tagReady && Tags.emptyText !== undefined
                                        ? String(Tags.emptyText) : ""

    // 新建标签用的配色（服务端接受 #RGB / #RRGGBB，非法值回退默认色）
    readonly property var palette: ["#4C6FFF", "#0F7A54", "#A2600A", "#C43D3D",
                                    "#7A5AF8", "#1A6FA8", "#B5408A", "#5B6472"]
    property string newColor: "#4C6FFF"

    modal: true
    anchors.centerIn: parent
    width: Math.min(460, (parent ? parent.width - Theme.spaceXl * 2 : 460))
    height: Math.min(560, (parent ? parent.height - Theme.spaceXl * 2 : 560))
    padding: Theme.spaceL
    closePolicy: Popup.CloseOnEscape
    title: qsTr("标签")
    standardButtons: Dialog.NoButton

    function has(id) {
        return dlg.selectedIds.indexOf(String(id)) >= 0
    }

    function toggle(id) {
        const key = String(id)
        const next = dlg.selectedIds.slice()
        const at = next.indexOf(key)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(key)
        dlg.selectedIds = next
    }

    function openFor(id, name) {
        dlg.fileId = String(id)
        dlg.fileName = (name === undefined || name === null) ? "" : String(name)
        dlg.selectedIds = []
        dlg.errorText = ""
        dlg.pendingName = ""
        dlg.loading = true
        // 列表先刷新（可能在上次打开后又建了新标签），再取该文件的当前标签
        if (dlg.tagReady && typeof Tags.refresh === "function")
            Tags.refresh()
        if (dlg.tagReady && typeof Tags.loadFileTags === "function")
            Tags.loadFileTags(dlg.fileId)
        else
            dlg.loading = false
        dlg.open()
    }

    function doCreate() {
        const n = newTagField.text.trim()
        if (n.length === 0) {
            dlg.errorText = qsTr("标签名不能为空")
            return
        }
        dlg.errorText = ""
        dlg.pendingName = n
        if (dlg.tagReady && typeof Tags.create === "function")
            Tags.create(n, dlg.newColor)
        newTagField.text = ""
    }

    function doSave() {
        if (!dlg.tagReady || typeof Tags.assign !== "function")
            return
        Tags.assign(dlg.fileId, dlg.selectedIds)
        dlg.close()
    }

    Connections {
        target: dlg.tagReady ? Tags : null

        function onFileTagsLoaded(fid, tagIds) {
            // 只接受当前目标的回包（切换文件/重复打开时旧回包必须丢弃）
            if (String(fid) !== dlg.fileId)
                return
            dlg.selectedIds = (tagIds === undefined || tagIds === null)
                              ? [] : tagIds.map(function (x) { return String(x) })
            dlg.loading = false
        }
        function onTagsChanged() {
            // 刚创建的标签：列表刷新后按名字把它勾上（新建即选中，省一次点击）
            if (dlg.pendingName.length === 0 || !dlg.modelReady)
                return
            for (let i = 0; i < TagModel.count; ++i) {
                const t = TagModel.at(i)
                if (t && String(t.name) === dlg.pendingName) {
                    if (!dlg.has(t.id))
                        dlg.toggle(t.id)
                    dlg.pendingName = ""
                    break
                }
            }
        }
        function onErrorOccurred(message) {
            dlg.errorText = String(message)
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        // ---- 目标文件 ----
        Label {
            Layout.fillWidth: true
            text: qsTr("为「%1」设置标签").arg(dlg.fileName)
            color: Theme.textPrimary
            font.pointSize: Theme.fontBody
            font.bold: true
            elide: Text.ElideMiddle
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("一个文件可以有多个标签；标签不改变文件原有的位置。")
            color: Theme.textSecondary
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }

        // ---- 错误 / 加载提示 ----
        Label {
            Layout.fillWidth: true
            visible: dlg.errorText.length > 0
            text: dlg.errorText
            color: Theme.danger
            font.pointSize: Theme.fontSecondary
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            visible: dlg.loading
            BusyIndicator {
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                running: dlg.loading
            }
            Label {
                text: qsTr("正在读取该文件已有的标签…")
                color: Theme.textSecondary
                font.pointSize: Theme.fontSecondary
            }
            Item { Layout.fillWidth: true }
        }

        // ---- 标签勾选列表 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusControl
            color: Theme.surfaceSunken
            border.width: 1
            border.color: Theme.border
            clip: true

            // 空态：一个标签都没有时给明确引导（而不是一片空白）
            Label {
                anchors.centerIn: parent
                anchors.margins: Theme.spaceL
                width: parent.width - Theme.spaceL * 2
                visible: dlg.tagCount === 0
                text: dlg.emptyText.length > 0
                      ? dlg.emptyText
                      : qsTr("还没有标签。在下方输入名称并选择颜色即可创建。")
                color: Theme.textTertiary
                font.pointSize: Theme.fontSecondary
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            ListView {
                anchors.fill: parent
                anchors.margins: Theme.spaceXs
                clip: true
                visible: dlg.tagCount > 0
                model: dlg.modelReady ? TagModel : null
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }

                delegate: Rectangle {
                    id: tagRow

                    readonly property bool picked: dlg.has(model.id)

                    width: ListView.view ? ListView.view.width : 0
                    height: Theme.rowHeight
                    radius: Theme.radiusControl
                    color: tagRow.picked ? Theme.selected
                         : (tagMouse.containsMouse ? Theme.hover : "transparent")

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceS
                        anchors.rightMargin: Theme.spaceS
                        spacing: Theme.spaceS

                        // 色点：标签的颜色本身就是它的身份，必须在列表里可见
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            width: 10
                            height: 10
                            radius: 5
                            color: model.color !== undefined && String(model.color).length > 0
                                   ? model.color : Theme.primary
                        }
                        Label {
                            Layout.fillWidth: true
                            text: model.name
                            color: Theme.textPrimary
                            font.pointSize: Theme.fontBody
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        Label {
                            text: qsTr("%1 个文件").arg(model.count)
                            color: Theme.textTertiary
                            font.pointSize: Theme.fontMicro
                            verticalAlignment: Text.AlignVCenter
                        }
                        CvIcon {
                            Layout.alignment: Qt.AlignVCenter
                            visible: tagRow.picked
                            name: "check"
                            size: Theme.iconSize
                            color: Theme.primary
                        }
                    }

                    MouseArea {
                        id: tagMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dlg.toggle(model.id)
                    }
                }
            }
        }

        // ---- 就地新建标签（不必先跳到标签页再回来）----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            TextField {
                id: newTagField
                Layout.fillWidth: true
                implicitHeight: Theme.controlHeight
                placeholderText: qsTr("新建标签名称")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textTertiary
                selectByMouse: true
                font.pointSize: Theme.fontBody
                enabled: !dlg.tagBusy
                onAccepted: dlg.doCreate()
            }

            // 颜色选择：预设色块，点选即定
            RowLayout {
                spacing: Theme.spaceXs
                Repeater {
                    model: dlg.palette
                    delegate: Rectangle {
                        width: 16
                        height: 16
                        radius: 8
                        color: modelData
                        border.width: dlg.newColor === modelData ? 2 : 0
                        border.color: Theme.textPrimary
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dlg.newColor = modelData
                        }
                    }
                }
            }

            SecondaryButton {
                iconName: "plus"
                text: qsTr("创建")
                enabled: !dlg.tagBusy
                onClicked: dlg.doCreate()
            }
        }

        // ---- 底部操作 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS

            Item { Layout.fillWidth: true }
            SecondaryButton {
                text: qsTr("取消")
                onClicked: dlg.close()
            }
            PrimaryButton {
                iconName: "check"
                text: qsTr("保存")
                // 加载未完成前禁止保存：否则会拿"空集合"覆盖掉已有标签
                enabled: !dlg.loading && !dlg.tagBusy
                onClicked: dlg.doSave()
            }
        }
    }
}
