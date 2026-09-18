pragma Singleton

import QtQuick

/*!
    云匣 CloudVault 设计令牌单例（界面唯一配色 / 字号 / 间距 / 圆角来源）。

    取值对齐 docs/UI优化方案.md §2.1；页面与控件中禁止出现硬编码色值，
    一律通过 Theme.* 引用，保证视觉统一与后续换肤能力。

    注意：qtquickcontrols2.conf 已将控件风格固定为 FluentWinUI3 / Light，
    因此 darkMode 默认 false；此处保留完整深色令牌以便后续跟随系统时启用。

    ⚠️ 命名禁忌：属性不要以 `on` 开头（会被 QML 解析为信号处理器 on<Xxx>，
    导致整份单例加载失败）。例如主色上的文字色取名 textOnPrimary 而非 onPrimary。
*/
QtObject {
    id: theme

    // 是否深色模式（默认浅色，与 FluentWinUI3 配置一致）
    property bool darkMode: false

    // -----------------------------------------------------------------
    //  色彩 · 主色
    // -----------------------------------------------------------------
    readonly property color primary:        "#2563EB"
    readonly property color primaryHover:   "#1D4ED8"
    readonly property color primaryPressed: "#1E40AF"
    readonly property color textOnPrimary:  "#FFFFFF" // 主色上的文字色（勿用 onXxx 命名）

    // -----------------------------------------------------------------
    //  色彩 · 语义
    // -----------------------------------------------------------------
    readonly property color success: "#16A34A"
    readonly property color warning: "#D97706"
    readonly property color danger:  "#DC2626"
    readonly property color info:    "#0284C7"

    // 语义色的交互态（hover / pressed）
    readonly property color dangerHover:   "#B91C1C"
    readonly property color dangerPressed: "#991B1B"

    // -----------------------------------------------------------------
    //  色彩 · 中性（浅色）
    // -----------------------------------------------------------------
    readonly property color lightBg:            "#F8FAFC"
    readonly property color lightCard:          "#FFFFFF"
    readonly property color lightBorder:        "#E2E8F0"
    readonly property color lightBorderStrong:  "#CBD5E1"
    readonly property color lightTextPrimary:   "#0F172A"
    readonly property color lightTextSecondary: "#64748B"
    readonly property color lightHover:         "#F1F5F9"
    readonly property color lightHoverStrong:   "#E2E8F0"
    readonly property color lightSelected:      "#DBEAFE"

    // -----------------------------------------------------------------
    //  色彩 · 中性（深色，跟随系统时使用）
    // -----------------------------------------------------------------
    readonly property color darkBg:            "#0F172A"
    readonly property color darkCard:          "#1E293B"
    readonly property color darkBorder:        "#334155"
    readonly property color darkBorderStrong:  "#475569"
    readonly property color darkTextPrimary:   "#E2E8F0"
    readonly property color darkTextSecondary: "#94A3B8"
    readonly property color darkHover:         "#334155"
    readonly property color darkHoverStrong:   "#3E4C61"
    readonly property color darkSelected:      "#1E3A8A"

    // -----------------------------------------------------------------
    //  语义别名（页面 / 控件只用这些，不直接用上面的原始令牌）
    // -----------------------------------------------------------------
    readonly property color bg:            darkMode ? darkBg : lightBg
    readonly property color card:          darkMode ? darkCard : lightCard
    readonly property color border:        darkMode ? darkBorder : lightBorder
    readonly property color borderStrong:  darkMode ? darkBorderStrong : lightBorderStrong
    readonly property color textPrimary:   darkMode ? darkTextPrimary : lightTextPrimary
    readonly property color textSecondary: darkMode ? darkTextSecondary : lightTextSecondary
    readonly property color hover:         darkMode ? darkHover : lightHover
    readonly property color hoverStrong:   darkMode ? darkHoverStrong : lightHoverStrong
    readonly property color selected:      darkMode ? darkSelected : lightSelected
    readonly property color disabledText:  darkMode ? "#475569" : "#94A3B8"

    // 焦点环：主色 40% 透明（用于键盘可达性的可见聚焦描边）
    readonly property color focusRing: Qt.rgba(primary.r, primary.g, primary.b, 0.40)

    // -----------------------------------------------------------------
    //  传输状态 → StatusBadge 色调 的映射
    //  （传输页活动队列 / 历史分组 / 传输抽屉三处共用，避免复制同一段 switch）
    // -----------------------------------------------------------------
    function toneForState(token) {
        switch (token) {
        case "completed": return "success"
        case "failed":    return "danger"
        case "canceled":  return "neutral"
        case "paused":    return "warning"
        case "running":   return "info"
        case "hashing":   return "info"
        default:          return "neutral"
        }
    }

    // -----------------------------------------------------------------
    //  字节数 → 人类可读串（仅用于**无 text 变体可用**的兜底：
    //  远程容量只有 totalBytes 数值；本机磁盘与历史大小一律用后端 text 键。）
    // -----------------------------------------------------------------
    function humanBytes(bytes) {
        const n = Number(bytes)
        if (!isFinite(n) || n < 0)
            return "—"
        if (n < 1024)
            return n + " B"
        const KB = 1024, MB = KB * 1024, GB = MB * 1024, TB = GB * 1024
        const fmt = (v) => v >= 100 ? v.toFixed(0) : (v >= 10 ? v.toFixed(1) : v.toFixed(2))
        if (n < MB) return fmt(n / KB) + " KB"
        if (n < GB) return fmt(n / MB) + " MB"
        if (n < TB) return fmt(n / GB) + " GB"
        return fmt(n / TB) + " TB"
    }

    // -----------------------------------------------------------------
    //  字号（pt）
    // -----------------------------------------------------------------
    readonly property int fontTitle:     17
    readonly property int fontBody:      13
    readonly property int fontSecondary: 12
    readonly property int fontMono:      12 // 仅用于哈希与日志

    // -----------------------------------------------------------------
    //  间距栅格（px）
    // -----------------------------------------------------------------
    readonly property int spaceXs:  4
    readonly property int spaceS:   8
    readonly property int spaceM:   12
    readonly property int spaceL:   16
    readonly property int spaceXl:  24
    readonly property int spaceXxl: 32

    // -----------------------------------------------------------------
    //  圆角（px）
    // -----------------------------------------------------------------
    readonly property int radiusControl: 8
    readonly property int radiusCard:    12

    // -----------------------------------------------------------------
    //  尺寸（px）
    // -----------------------------------------------------------------
    readonly property int rowHeight:      38
    readonly property int controlHeight:  36
    readonly property int iconSize:       16
    readonly property int toolbarHeight:  48
    readonly property int navRailWidth:   96
    readonly property int sidePanelWidth: 320

    // -----------------------------------------------------------------
    //  响应式断点（px）：≥wide 三栏；[narrow, wide) 抽屉；<narrow 单栏
    // -----------------------------------------------------------------
    readonly property int breakpointWide:   1280
    readonly property int breakpointNarrow: 768

    // -----------------------------------------------------------------
    //  动效时长（ms）
    // -----------------------------------------------------------------
    readonly property int animFast:   120
    readonly property int animNormal: 200
}
