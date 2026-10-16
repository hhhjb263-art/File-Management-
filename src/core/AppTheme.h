/****************************************************************************
 * core/AppTheme.h —— 主题模式（浅色 / 深色 / 跟随系统）的 C++ 桥接
 *
 * 职责（只做两件事，不碰任何业务）：
 *   1. 把「模式字符串」解析为「最终是否深色」，并把结果推入 Qt：
 *        · QStyleHints::setColorScheme()  —— 让 FluentWinUI3 等**内建控件**
 *          （TextField / ComboBox / Switch / Menu / ScrollBar…）跟着变色
 *        · QGuiApplication::setPalette()  —— 不依赖平台主题，保证无头渲染与真机一致
 *   2. 以 QObject 形式暴露给 QML（上下文属性名 `AppTheme`）：
 *        · Q_PROPERTY themeMode  —— 读写，写入即持久化 + 立即生效
 *        · Q_PROPERTY isDark     —— 只读，供 `Theme.qml` 绑定
 *        · signal changed        —— 模式变化时通知 QML
 *
 * ⚠️ 色值同步约定：本文件中的调色板色值必须与 `qml/theme/Theme.qml` 的
 *    浅色 / 深色令牌**逐条对应**。两处是唯一需要人工同步的地方（QML 侧负责
 *    自绘控件，C++ 侧负责 Qt 内建控件），改动任一侧请同步另一侧。
 *    本文件顶部用 [L] / [D] 标注每个色值对应的 QML 令牌名。
 *
 * 设计说明（为什么不用 qmlRegisterSingletonInstance）：
 *   用根上下文属性暴露，QML 侧零 import 改动、无需类型注册，风险最低；
 *   主题是**表现层关注点**，也不应塞进 controllers/（那层 API 已冻结）。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace cv {

class AppTheme : public QObject
{
    Q_OBJECT
    // 模式："light" | "dark" | "system"（非法值一律归一化为 "system"）
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY changed)
    // 最终是否深色（已解析 system）
    Q_PROPERTY(bool isDark READ isDark NOTIFY changed)
    // 解析后的实际模式："light" | "dark"（system 已被解析掉）
    Q_PROPERTY(QString effectiveMode READ effectiveMode NOTIFY changed)

public:
    explicit AppTheme(QObject *parent = nullptr);
    ~AppTheme() override;

    // ---- 静态工具（无状态，便于 main.cpp 与自检使用）----

    // 归一化模式字符串：非 "light"/"dark"/"system" → "system"
    static QString normalize(const QString &mode);
    // 解析模式 → 是否深色（"system" 读平台配色方案；平台未知时按浅色）
    static bool resolveDark(const QString &mode);
    // 把模式推入 Qt（ColorScheme + QPalette）。可重复调用，幂等。
    static void applyToApplication(const QString &mode);
    // 供 QML 展示的模式清单（顺序即推荐展示顺序）
    static QStringList availableModes();

    // ---- QML 接口 ----
    QString themeMode() const { return m_mode; }
    void    setThemeMode(const QString &mode);

    bool    isDark() const { return resolveDark(m_mode); }
    QString effectiveMode() const { return resolveDark(m_mode) ? QStringLiteral("dark")
                                                               : QStringLiteral("light"); }

    // 与属性等价的显式方法（QML 调用更直观：AppTheme.setMode("dark")）
    Q_INVOKABLE void setMode(const QString &mode) { setThemeMode(mode); }

signals:
    void changed();

private:
    void onPlatformColorSchemeChanged();

    QString m_mode = QStringLiteral("system");
};

} // namespace cv
