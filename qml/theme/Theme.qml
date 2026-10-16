pragma Singleton

import QtQuick

/*!
    云匣 CloudVault 设计令牌单例 —— 界面唯一配色 / 字号 / 间距 / 圆角 / 尺寸来源。

    ─────────────────────────────────────────────────────────────────────
     设计方向：现代桌面效率风（Linear / Notion 一脉）
    ─────────────────────────────────────────────────────────────────────
     四条原则（改令牌前请先读）：
       1. 中性低彩度底 + 单一强调色。强调色只用于「主操作 / 选中态 / 焦点」，
          绝不用作装饰；一屏内出现的强调色块应 ≤ 2 处。
       2. 层次靠「背景明度阶梯 + 1px 极细描边」表达，不靠投影。
          故本文件不引入 DropShadow / MultiEffect —— 少一个模块依赖，也更耐看。
          明度阶梯：surfaceSunken（最沉） < bg < card（最亮）。
       3. 交互态优先改背景（hover / selected），不改边框 —— 界面更安静。
          边框只在「输入聚焦」与「危险」时变化。
       4. 紧凑但有呼吸：控件高 32、表格行高 36；卡片内边距 16、卡内项间距 12。

    ─────────────────────────────────────────────────────────────────────
     ⚠️ 兼容性硬约束（换肤时务必遵守）
    ─────────────────────────────────────────────────────────────────────
     · 本文件的**既有属性名已被 30 个 QML 文件引用**（含 dialogs / panels），
       只能「改值」与「新增」，**禁止改名或删除**，否则整包界面加载失败。
     · 命名禁忌：属性不要以 `on` 开头（会被 QML 解析为信号处理器 on<Xxx>，
       导致整份单例加载失败）。例：主色上的文字色取名 textOnPrimary。
     · 深色模式由 `darkMode`（可写）驱动；写入方是 `qml/Main.qml` 的一条
       Binding（值来自 App.isDark）。因此这里**不直接引用 App**，保持单例独立。

     主题取值表与可访问性对照见 docs/UI设计系统-现代桌面效率风.md。
*/
QtObject {
    id: theme

    // =================================================================
    //  模式开关
    // =================================================================
    // 是否深色模式。由 Main.qml 的 Binding 写入（App.isDark：浅色/深色/跟随系统
    // 三态在 C++ 侧解析后给出最终布尔值）。此处默认浅色，保证单例可独立加载。
    property bool darkMode: false

    // 便捷别名（只读）：语义上比 darkMode 更好读，供新增代码使用。
    readonly property bool isDark: darkMode

    // =================================================================
    //  色彩 · 强调色（单一强调色；深浅两套等权完整）
    // =================================================================
    // 浅色：靛蓝 #3B5BDB —— 比原 #2563EB 更低彩度，更"工具感"、久看不刺眼。
    // 深色：浅靛蓝 #8AA0F2 —— 深底上足够亮（对比度 7.4:1），其上压深色文字。
    readonly property color primary:        darkMode ? "#8AA0F2" : "#3B5BDB"
    readonly property color primaryHover:   darkMode ? "#9AAEF5" : "#3352C9"
    readonly property color primaryPressed: darkMode ? "#7A90E6" : "#2B45AC"
    // 主色上的文字色（勿用 onXxx 命名）
    readonly property color textOnPrimary:  darkMode ? "#0E1116" : "#FFFFFF"
    // 主色的浅底（用于选中行 / Ghost 激活态 / 轻量高亮）
    readonly property color primarySoft:    darkMode ? "#1E2740" : "#E7EDFC"

    // =================================================================
    //  色彩 · 语义（降饱和到 AA 可用区间；每色配一个 soft 浅底）
    // =================================================================
    readonly property color success:     darkMode ? "#4CC38A" : "#0F7A54"
    readonly property color warning:     darkMode ? "#E0A44A" : "#A2600A"
    readonly property color danger:      darkMode ? "#E86B6B" : "#C43D3D"
    readonly property color info:        darkMode ? "#63B3E8" : "#1A6FA8"

    readonly property color successSoft: darkMode ? "#14301F" : "#E3F4EC"
    readonly property color warningSoft: darkMode ? "#33270F" : "#FBF0DC"
    readonly property color dangerSoft:  darkMode ? "#3A2020" : "#FBEAEA"
    readonly property color infoSoft:    darkMode ? "#12293A" : "#E5F1F9"

    // 危险色的交互态（用于实心危险按钮）
    readonly property color dangerHover:   darkMode ? "#F07E7E" : "#AE3434"
    readonly property color dangerPressed: darkMode ? "#D45C5C" : "#932C2C"

    // =================================================================
    //  色彩 · 中性（浅色）
    // =================================================================
    readonly property color lightBg:            "#F7F8FA" // 应用底
    readonly property color lightSurfaceSunken: "#F1F3F6" // 表头 / 输入井 / 内嵌块
    readonly property color lightCard:          "#FFFFFF" // 卡片 / 浮层
    readonly property color lightBorder:        "#E6E9EF" // 极细描边（0.5~1px）
    readonly property color lightDivider:       "#EFF1F5" // 卡片内部分隔线（更淡）
    readonly property color lightBorderStrong:  "#D3D8E0" // 输入框 / 强调描边
    readonly property color lightTextPrimary:   "#141821"
    readonly property color lightTextSecondary: "#5B6472"
    readonly property color lightTextTertiary:  "#8B94A3"
    readonly property color lightHover:         "#F2F4F7"
    readonly property color lightHoverStrong:   "#E9ECF1"
    readonly property color lightSelected:      "#E7EDFC"
    readonly property color lightDisabledText:  "#A3ABBA"

    // =================================================================
    //  色彩 · 中性（深色）
    // =================================================================
    readonly property color darkBg:            "#0E1116"
    readonly property color darkSurfaceSunken: "#0A0D11"
    readonly property color darkCard:          "#161A21"
    readonly property color darkBorder:        "#262C36"
    readonly property color darkDivider:       "#1F242C"
    readonly property color darkBorderStrong:  "#333A46"
    readonly property color darkTextPrimary:   "#E8EBF0"
    readonly property color darkTextSecondary: "#9BA4B2"
    readonly property color darkTextTertiary:  "#6E7686"
    readonly property color darkHover:         "#1D222B"
    readonly property color darkHoverStrong:   "#232935"
    readonly property color darkSelected:      "#1E2A47"
    readonly property color darkDisabledText:  "#525A68"

    // =================================================================
    //  语义别名（页面 / 控件只用这些，不直接用上面的原始令牌）
    // =================================================================
    readonly property color bg:            darkMode ? darkBg : lightBg
    readonly property color surfaceSunken: darkMode ? darkSurfaceSunken : lightSurfaceSunken
    readonly property color card:          darkMode ? darkCard : lightCard
    readonly property color border:        darkMode ? darkBorder : lightBorder
    readonly property color divider:       darkMode ? darkDivider : lightDivider
    readonly property color borderStrong:  darkMode ? darkBorderStrong : lightBorderStrong
    readonly property color textPrimary:   darkMode ? darkTextPrimary : lightTextPrimary
    readonly property color textSecondary: darkMode ? darkTextSecondary : lightTextSecondary
    readonly property color textTertiary:  darkMode ? darkTextTertiary : lightTextTertiary
    readonly property color hover:         darkMode ? darkHover : lightHover
    readonly property color hoverStrong:   darkMode ? darkHoverStrong : lightHoverStrong
    readonly property color selected:      darkMode ? darkSelected : lightSelected
    readonly property color disabledText:  darkMode ? darkDisabledText : lightDisabledText

    // 表格 / 列表表头底色（= 沉一档的表面）
    readonly property color tableHeader: surfaceSunken

    // 模态遮罩
    readonly property color overlay: darkMode ? Qt.rgba(0, 0, 0, 0.55)
                                              : Qt.rgba(0.08, 0.11, 0.16, 0.32)

    // 滚动条
    readonly property color scrollbarThumb:      darkMode ? "#39414E" : "#C9CFD9"
    readonly property color scrollbarThumbHover: darkMode ? "#4A5464" : "#AEB6C2"

    // 焦点环：强调色 35% 透明（键盘可达性的可见聚焦描边）
    readonly property color focusRing: Qt.rgba(primary.r, primary.g, primary.b, 0.35)

    // =================================================================
    //  工具函数
    // =================================================================

    // 把语义色按 alpha 叠到当前卡片底色上 —— 供徽标 / 提示条做浅底。
    // （StatusBadge 等控件用它，避免各处手写 Qt.rgba。）
    function softOf(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha === undefined ? 0.14 : alpha)
    }

    // 传输状态 → StatusBadge 色调 的映射
    // （传输页活动队列 / 历史分组 / 传输抽屉三处共用，避免复制同一段 switch）
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

    // 字节数 → 人类可读串（仅用于**无 text 变体可用**的兜底：
    // 远程容量只有 totalBytes 数值；本机磁盘与历史大小一律用后端 text 键。）
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

    // =================================================================
    //  字号（pt）
    //  ⚠️ 单位是 pointSize，不是 pixelSize：在 96 DPI 下 1pt = 4/3 px，
    //     所以 9.75pt ≈ 13px、11pt ≈ 14.7px。取值按「目标像素」反推得出，
    //     保留 pointSize 是为了让 HiDPI 屏能自动等比放大（pixelSize 不会）。
    //     目标：正文 13px（略大于 Windows 默认 9pt/12px，中文更舒适）。
    // =================================================================
    readonly property real fontDisplay:   13   // ≈17.3px 页面主标题
    readonly property real fontTitle:     11   // ≈14.7px 卡片标题 / 区块标题
    readonly property real fontBody:      9.75 // ≈13.0px 正文（基准）
    readonly property real fontSecondary: 9    // ≈12.0px 次要说明
    readonly property real fontMicro:     8.5  // ≈11.3px 表头 / 徽标 / 极小注释
    readonly property real fontMono:      9    // ≈12.0px 仅哈希与日志

    // =================================================================
    //  间距栅格（px，4 的倍数）
    // =================================================================
    readonly property int spaceXs:  4
    readonly property int spaceS:   8
    readonly property int spaceM:   12
    readonly property int spaceL:   16
    readonly property int spaceXl:  24
    readonly property int spaceXxl: 32

    // =================================================================
    //  圆角（px）—— 效率风偏紧：控件 6、卡片 10
    // =================================================================
    readonly property int radiusControl: 6
    readonly property int radiusCard:    10
    readonly property int radiusPill:    999

    // =================================================================
    //  尺寸（px）—— 与缩小后的字号配套收紧，避免"字小框大"的松垮感
    // =================================================================
    readonly property int rowHeight:          34 // 表格 / 列表行高
    readonly property int tableHeaderHeight:  30 // 表头（比数据行略矮，形成层次）
    readonly property int controlHeight:      30 // 按钮 / 输入框高度
    readonly property int controlHeightSmall: 24 // 行内小控件
    readonly property int iconSize:           14
    readonly property int iconSizeSmall:      12
    readonly property int iconSizeLarge:      18
    readonly property int toolbarHeight:      44
    readonly property int statusBarHeight:    26
    readonly property int navRailWidth:       84
    readonly property int sidePanelWidth:     320

    // =================================================================
    //  响应式断点（px）：≥wide 三栏；[narrow, wide) 抽屉；<narrow 单栏
    // =================================================================
    readonly property int breakpointWide:   1280
    readonly property int breakpointNarrow: 768

    // =================================================================
    //  动效时长（ms）—— 效率风偏快，避免"粘滞感"
    // =================================================================
    readonly property int animFast:   100
    readonly property int animNormal: 180
    readonly property int animSlow:   260
}
