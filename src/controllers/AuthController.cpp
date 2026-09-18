/****************************************************************************
 * controllers/AuthController.cpp —— 账户控制器实现（无网络请求）
 ****************************************************************************/
#include "AuthController.h"

namespace cv {

AuthController::AuthController(QObject *parent)
    : QObject(parent)
{
    // 服务端无登录 / 用户体系：本控制器不持有后端、不发任何请求。
    // userName 固定为空，避免误导成「已登录某用户」。
    m_userName.clear();
}

QString AuthController::unsupportedNotice() const
{
    // 中文、可操作：说明为什么不可用 + 当前替代方式（访问令牌）。
    return QStringLiteral("服务端暂不支持此功能：当前版本没有登录 / 用户体系，"
                          "客户端通过「访问令牌」直接对接服务端。");
}

} // namespace cv
