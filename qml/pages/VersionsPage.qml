import QtQuick

import "../controls"

/*!
    文件版本页 —— 服务端未实现历史版本（契约 §1 Unsupported），占位保留。
*/
UnsupportedPage {
    featureName: qsTr("文件版本")
    glyph: "🕘"
    notice: App.unsupportedNotice
}
