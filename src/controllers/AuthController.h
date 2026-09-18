/****************************************************************************
 * controllers/AuthController.h —— 账户控制器（服务端不支持）
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：supported(=false)、userName(="")、unsupportedNotice
 *   · 方法：无
 * 服务端没有登录 / 用户体系（契约 §1 Unsupported 清单），因此本控制器
 * **不持有后端、不发起任何网络请求**，只向 QML 暴露「不支持」的呈现信息：
 *   - supported == false  → QML 据此把入口置灰 + tooltip
 *   - unsupportedNotice   → 置灰入口的中文说明（tooltip 文案）
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>

namespace cv {

class AuthController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported         READ supported         CONSTANT)
    Q_PROPERTY(QString userName          READ userName          NOTIFY userNameChanged)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)

public:
    explicit AuthController(QObject *parent = nullptr);

    bool    supported() const { return false; }
    QString userName() const { return m_userName; }
    QString unsupportedNotice() const;

signals:
    void userNameChanged();

private:
    QString m_userName; // 服务端无用户体系，恒为空
};

} // namespace cv
