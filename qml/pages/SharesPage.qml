import QtQuick

import "../controls"

/*!
    分享链接页 —— 服务端未实现分享（契约 §1 Unsupported），占位保留。
*/
UnsupportedPage {
    featureName: qsTr("分享链接")
    glyph: "🔗"
    notice: Shares.unsupportedNotice
}
