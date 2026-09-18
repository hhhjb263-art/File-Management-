/****************************************************************************
 * controllers/ShareController.h —— 分享控制器（服务端不支持）
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：supported(=false)、unsupportedNotice
 *   · 方法：无
 * 服务端未实现分享链接（契约 §1 Unsupported 清单），故本控制器
 * **不发起任何网络请求**，只提供置灰入口所需的中文说明。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>

namespace cv {

class ShareController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported         READ supported         CONSTANT)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)

public:
    explicit ShareController(QObject *parent = nullptr);

    bool    supported() const { return false; }
    QString unsupportedNotice() const;
};

} // namespace cv
