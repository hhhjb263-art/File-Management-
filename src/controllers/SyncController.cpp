/****************************************************************************
 * controllers/SyncController.cpp —— 同步控制器实现（无网络请求）
 ****************************************************************************/
#include "SyncController.h"

namespace cv {

SyncController::SyncController(QObject *parent)
    : QObject(parent)
{
    // 本阶段不启用同步：不同步引擎、不发请求。
}

QString SyncController::unsupportedNotice() const
{
    return QStringLiteral("服务端暂不支持此功能：文件夹自动同步在本版本未开放"
                          "（同步地基已保留，后续版本启用）。");
}

} // namespace cv
