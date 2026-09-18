/****************************************************************************
 * controllers/SyncController.h —— 同步控制器（本阶段不支持）
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：supported(=false)、unsupportedNotice
 *   · 方法：无
 * src/sync 的地基（LocalIndex / SyncRules）保留，但同步引擎（SyncEngine /
 * FileWatcher）尚未实现，故本阶段 supported == false。本控制器
 * **不发起任何网络请求**，只提供置灰入口所需的中文说明。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>

namespace cv {

class SyncController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported         READ supported         CONSTANT)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)

public:
    explicit SyncController(QObject *parent = nullptr);

    bool    supported() const { return false; }
    QString unsupportedNotice() const;
};

} // namespace cv
