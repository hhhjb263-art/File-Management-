/****************************************************************************
 * controllers/ShareController.cpp —— 分享控制器实现（无网络请求）
 ****************************************************************************/
#include "ShareController.h"

namespace cv {

ShareController::ShareController(QObject *parent)
    : QObject(parent)
{
    // 服务端未实现分享链接：不发请求。
}

QString ShareController::unsupportedNotice() const
{
    return QStringLiteral("服务端暂不支持此功能：分享链接在本版本未开放。");
}

} // namespace cv
