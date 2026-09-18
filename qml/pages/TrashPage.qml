import QtQuick

import "../controls"

/*!
    回收站页 —— 服务端无回收站（契约 §8.1：删除即真实永久删除），占位保留。
    文案明确提示删除不可恢复，避免用户误以为存在回收站。
*/
UnsupportedPage {
    featureName: qsTr("回收站")
    glyph: "🗑"
    notice: qsTr("服务端不提供回收站：删除文件为真实永久删除，删除后不可恢复。"
                 + "请谨慎操作。")
}
