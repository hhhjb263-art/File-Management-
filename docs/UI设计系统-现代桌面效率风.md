# 云匣 CloudVault · UI 设计系统（现代桌面效率风）

| 项 | 内容 |
|---|---|
| 设计方向 | **现代桌面效率风**（Linear / Notion 一脉） |
| 适用客户端 | 正式 QML 客户端（`qml/` + `src/`，qmake → `bin/CloudVault.exe`） |
| 改造范围 | **视觉换肤**：设计令牌 + 控件层 + 图标系统 + 主题模式。**布局与信息架构保持不动** |
| 主题 | 浅色 / 深色 / 跟随系统（三态，可持久化） |
| 可访问性 | 正文级对比度全部 ≥ 4.5:1（WCAG AA） |
| 令牌唯一来源 | `qml/theme/Theme.qml`（自绘控件）+ `src/core/AppTheme.cpp`（Qt 内建控件调色板） |

---

## 1. 设计原则（改令牌前先读这四条）

1. **中性低彩度底 + 单一强调色。**
   强调色只用于「主操作 / 选中态 / 焦点」，绝不做装饰。一屏内出现的强调色块应 ≤ 2 处。
2. **层次靠「背景明度阶梯 + 1px 极细描边」，不靠投影。**
   明度阶梯：`surfaceSunken`（最沉） < `bg` < `card`（最亮）。
   因此本系统**不引入** `DropShadow` / `MultiEffect` —— 少一个模块依赖，观感也更耐看。
3. **交互态优先改背景，不改边框。**
   边框只在「输入聚焦」与「危险操作」时变化。边框不动，界面就不"抖"。
4. **紧凑但有呼吸。**
   控件高 32、表格行高 36；卡片内边距 16、卡内项间距 12。

### 与改造前的差异（为什么改）

| 维度 | 改造前 | 改造后 | 理由 |
|---|---|---|---|
| 强调色 | `#2563EB`（Tailwind blue-600） | `#3B5BDB`（低彩度靛蓝） | 原色偏"互联网产品"，久看刺眼；靛蓝更"工具感"，也更能与系统蓝区分 |
| 中性底 | `#F8FAFC` 冷灰 | `#F7F8FA` 中性 | 降低冷色偏移，白卡片在底上更稳 |
| 描边 | `#E2E8F0` | `#E6E9EF` + 新增 `divider #EFF1F5` | 卡片内部分隔不应与卡片外框同强度 |
| 控件高 | 36 | **32** | 效率风密度；一屏能多放一行 |
| 圆角 | 控件 8 / 卡片 12 | **控件 6 / 卡片 10** | 紧一档更"利落"，与原 36 高控件不匹配的松垮感消失 |
| 字号 | 标题 17 | **16**（新增 `fontDisplay 20`） | 标题不必比正文大 4pt；层级靠字重与颜色区分更高级 |
| 语义色 | `#16A34A` / `#DC2626` 等高饱和 | 降饱和（`#0F7A54` / `#C43D3D`…） | 高饱和语义色在深色主题会"发光"，且难以同时满足 AA |
| 图标 | emoji（`🗂 ⇅ 🔗 🗑`…） | **矢量图标**（24×24 线性描边） | emoji 跨平台字形不一、无法跟随主题着色、16px 下糊成一团 |
| 深色主题 | 令牌已备但**未启用**（`main.cpp` 钉死 Light） | 完整启用 + 跟随系统 | 桌面客户端的基本期待 |

---

## 2. 色彩系统

### 2.1 强调色

| 令牌 | 浅色 | 深色 | 用途 |
|---|---|---|---|
| `primary` | `#3B5BDB` | `#8AA0F2` | 主按钮、选中态、焦点、链接 |
| `primaryHover` | `#3352C9` | `#9AAEF5` | 悬停 |
| `primaryPressed` | `#2B45AC` | `#7A90E6` | 按下 |
| `primarySoft` | `#E7EDFC` | `#1E2740` | 选中行底、Ghost 激活底 |
| `textOnPrimary` | `#FFFFFF` | `#0E1116` | 压在实心强调/危险填充上的文字 |

> **深色主题的强调色是"浅色"**（`#8AA0F2`），因此其上的文字必须是**近黑**（`#0E1116`），
> 而不是白色 —— 这是深色主题最容易踩的对比度坑（白字压在浅蓝上只有 2.2:1）。

### 2.2 语义色（每色配一个 soft 浅底）

| 语义 | 浅色 | 浅色 soft | 深色 | 深色 soft |
|---|---|---|---|---|
| success | `#0F7A54` | `#E3F4EC` | `#4CC38A` | `#14301F` |
| warning | `#A2600A` | `#FBF0DC` | `#E0A44A` | `#33270F` |
| danger | `#C43D3D` | `#FBEAEA` | `#E86B6B` | `#3A2020` |
| info | `#1A6FA8` | `#E5F1F9` | `#63B3E8` | `#12293A` |

danger 另配交互态：`dangerHover`（`#AE3434` / `#F07E7E`）、`dangerPressed`（`#932C2C` / `#D45C5C`）。

> **soft 令牌的存在意义**：徽标 / 提示条的浅底不再现场算 `Qt.rgba(color, 0.14)` ——
> 那个公式在深色底上会得到"发光"效果。深浅两套 soft 值是分别调过的。

### 2.3 中性色

| 令牌 | 浅色 | 深色 | 用途 |
|---|---|---|---|
| `bg` | `#F7F8FA` | `#0E1116` | 应用底 |
| `surfaceSunken` | `#F1F3F6` | `#0A0D11` | 表头 / 输入井 / 历史展开体 |
| `card` | `#FFFFFF` | `#161A21` | 卡片 / 浮层 |
| `border` | `#E6E9EF` | `#262C36` | 极细描边 |
| `divider` | `#EFF1F5` | `#1F242C` | 卡片内部分隔线（更淡） |
| `borderStrong` | `#D3D8E0` | `#333A46` | 输入框 / 强调描边 |
| `textPrimary` | `#141821` | `#E8EBF0` | 正文 |
| `textSecondary` | `#5B6472` | `#9BA4B2` | 次要说明 |
| `textTertiary` | `#8B94A3` | `#6E7686` | 提示 / 占位 / 路径 |
| `hover` / `hoverStrong` | `#F2F4F7` / `#E9ECF1` | `#1D222B` / `#232935` | 悬停 / 按下 |
| `selected` | `#E7EDFC` | `#1E2A47` | 列表选中行 |
| `disabledText` | `#A3ABBA` | `#525A68` | 禁用文字 |

另有 `overlay`（模态遮罩）、`scrollbarThumb` / `scrollbarThumbHover`、`focusRing`（强调色 35%）、`tableHeader`（= `surfaceSunken`）。

### 2.4 可访问性实测（WCAG 2.1 对比度）

**浅色**

| 前景 / 背景 | 对比度 | 判定 |
|---|---|---|
| textPrimary / card | 17.76:1 | AA 正文 ✓ |
| textPrimary / bg | 16.71:1 | AA 正文 ✓ |
| textSecondary / card | 5.98:1 | AA 正文 ✓ |
| textSecondary / bg | 5.63:1 | AA 正文 ✓ |
| textTertiary / card | 3.06:1 | AA 大字（仅用于提示，非必读） |
| primary / card | 5.67:1 | AA 正文 ✓ |
| 白字 on primary | 5.67:1 | AA 正文 ✓ |
| danger / card | 5.14:1 | AA 正文 ✓ |
| 白字 on danger | 5.14:1 | AA 正文 ✓ |
| success / card | 5.34:1 | AA 正文 ✓ |
| warning / card | 4.98:1 | AA 正文 ✓ |
| info / card | 5.41:1 | AA 正文 ✓ |
| border / card | 1.22:1 | 装饰（非文字） |

**深色**

| 前景 / 背景 | 对比度 | 判定 |
|---|---|---|
| textPrimary / card | 14.60:1 | AA 正文 ✓ |
| textPrimary / bg | 15.83:1 | AA 正文 ✓ |
| textSecondary / card | 6.93:1 | AA 正文 ✓ |
| textTertiary / card | 3.82:1 | AA 大字（仅用于提示） |
| primary / card | 6.97:1 | AA 正文 ✓ |
| textOnPrimary on primary | 7.56:1 | AA 正文 ✓ |
| danger / card | 5.60:1 | AA 正文 ✓ |
| textOnPrimary on danger | 6.08:1 | AA 正文 ✓ |
| success / card | 7.88:1 | AA 正文 ✓ |
| warning / card | 7.96:1 | AA 正文 ✓ |
| info / card | 7.60:1 | AA 正文 ✓ |

> 复算方法（脚本化，可回归）：
> `python -c "…"` 用 WCAG 相对亮度公式
> `L = 0.2126R + 0.7152G + 0.0722B`（各通道先做 sRGB 反伽马），
> 对比度 = `(L₁+0.05)/(L₂+0.05)`。

---

## 3. 字体系统

| 令牌 | 值（pt） | 实际像素（96 DPI） | 用途 |
|---|---|---|---|
| `fontDisplay` | 13 | ≈17.3px | 页面主标题 |
| `fontTitle` | 11 | ≈14.7px | 卡片标题 / 区块标题 |
| `fontBody` | 9.75 | ≈13.0px | 正文（基准） |
| `fontSecondary` | 9 | ≈12.0px | 次要说明、徽标 |
| `fontMicro` | 8.5 | ≈11.3px | 表头 / 状态条 / 极小注释 |
| `fontMono` | 9 | ≈12.0px | 仅哈希与日志 |

> ⚠️ **单位是 `pointSize` 而不是 `pixelSize`**：96 DPI 下 1pt = 4/3 px。
> 取值按「目标像素」反推得出。保留 pointSize 是为了让 HiDPI 屏能自动等比放大
> （`pixelSize` 不会跟随 DPI）。正文目标 13px —— 略大于 Windows 默认的 9pt/12px，
> 中文更舒适；这是桌面效率风的标准档位。
>
> 📌 **历史修正**：初版令牌误把 pt 当 px 用（`fontBody: 13` 实际渲染 17.3px），
> 对 30–32px 高的控件明显偏大，是"界面显得陈旧、松垮"的主要原因之一。
> 全仓 `Theme.font*` 只用在 `font.pointSize` 上（已脚本校验），因此改值即可全局生效。

**字重只用两档**：常规（400）与加粗（`font.bold: true`）。
不使用 600/700 之类中间档 —— 中文字体在小字号下的合成加粗质量不可控。
标题层级主要靠**字重 + 颜色**区分，而不是靠字号差（见原则 1）。

字体族不指定，跟随系统（Windows 为 Microsoft YaHei UI / Segoe UI）——
桌面客户端指定字体族反而会破坏平台一致性。

---

## 4. 间距 / 圆角 / 尺寸 / 动效

| 类别 | 令牌 | 值 |
|---|---|---|
| 间距 | `spaceXs` / `spaceS` / `spaceM` / `spaceL` / `spaceXl` / `spaceXxl` | 4 / 8 / 12 / 16 / 24 / 32 |
| 圆角 | `radiusControl` / `radiusCard` / `radiusPill` | 6 / 10 / 999 |
| 尺寸 | `controlHeight` / `controlHeightSmall` | 30 / 24 |
| | `rowHeight` / `tableHeaderHeight` | 34 / 30 |
| | `iconSize` / `iconSizeSmall` / `iconSizeLarge` | 14 / 12 / 18 |
| | `toolbarHeight` / `statusBarHeight` | 44 / 26 |
| | `navRailWidth` / `sidePanelWidth` | 84 / 320 |
| 断点 | `breakpointWide` / `breakpointNarrow` | 1280 / 768 |
| 动效 | `animFast` / `animNormal` / `animSlow` | 100 / 180 / 260 ms |

**尺寸与字号配套**：字号下调后控件高度同步收紧（36→30、行高 36→34），
避免出现"字小框大"的松垮感 —— 这是与字号重标定同等重要的另一半工作。
新增 `tableHeaderHeight`（表头比数据行矮 4px，形成层次）与 `statusBarHeight`。

**动效原则**：只用 `ColorAnimation` 做背景/描边过渡，时长 100ms（`animFast`）。
效率风偏快，超过 200ms 会产生"粘滞感"。不做位移/缩放动效 —— 桌面工具不需要。

---

## 5. 图标系统

### 5.1 规范

- 画布 **24×24**，线性描边，`strokeWidth = 1.75`，圆头圆角（`RoundCap` / `RoundJoin`）
- 颜色**永远由调用方传入** → 天然跟随主题，不需要为深浅两套主题各准备一套资源
- 仅 `play` / `star` 两个图标用填充渲染（`Icons.filledNames`）
- 用 `QtQuick.Shapes` 渲染：**无位图资源、无 QtSvg 依赖**，任意 DPI 下都锐利
- 描边宽度随 item 等比缩放：16px 渲染时约 1.17px，视觉重量与正文一致

### 5.2 关键设计：字形映射层（`qml/theme/Icons.qml`）

改造前全项目 **23 个文件、140+ 处**用 emoji 当图标。全量替换意味着改 140 处页面代码 ——
风险高、易回归。因此设计了一个**映射层**：

```
页面代码（一行不改）          控件内部（统一翻译）           渲染
glyph: "⬆"          →   Icons.nameForGlyph("⬆")  →  "upload"  →  CvIcon（矢量）
```

- `Icons.glyphAlias` 是 emoji → 图标名的映射表（**唯一需要维护的地方**）
- 控件同时提供 `iconName` 属性（加法式，优先级高于 `glyph`），新代码直接写图标名
- 未登记的图标名 → 不绘制任何东西（`path` 为空串），**不显示方框、不报错**

于是"换图标"从「改 140 处」收敛为「改 1 张表」。

### 5.3 图标清单（35 个）

| 分组 | 图标名 |
|---|---|
| 品牌 / 导航 | `cloud` `grid` `transfer` `exchange` `settings` `more` `chevron-right` `chevron-down` `back` |
| 文件系统 | `folder` `folder-open` `file` `inbox` `drive` |
| 传输 | `upload` `download` `refresh` `sort` `pause` |
| 操作 | `plus` `close` `search` `copy` `clipboard` `edit` `trash` `save` `link` |
| 状态 | `check` `check-circle` `info` `warning` `lock` `shield` |
| 账户 / 其它 | `user` `key` `logout` `eye` `eye-off` `history` `tag` `play` `star` `list` |

新增图标：往 `Icons.paths` 加一条；若源自新 emoji，同时在 `Icons.glyphAlias` 登记。

---

## 6. 组件规格

所有控件位于 `qml/controls/`，只消费 `Theme.*` 令牌 —— **页面里不允许出现硬编码色值**
（改造前已达成，本次保持）。

| 控件 | 高度 | 关键规格 |
|---|---|---|
| `PrimaryButton` | 32 | 实心强调色；hover/pressed 变色；禁用态退到 `surfaceSunken` + `disabledText`（**不用半透明**，半透明在深色底会糊） |
| `SecondaryButton` | 32 | 1px `borderStrong` 描边 + 透明底；**边框恒定不变，交互只改背景** |
| `GhostButton` | 32 | 无边框；`active` 选中态 = `primarySoft` 底 + `primary` 文字；纯图标时内边距自动收紧到 8 |
| `DangerButton` | 32 | 实心 `danger`；文字用 `textOnPrimary`（深色主题下 danger 是浅红，压白字不达 AA） |
| `StatusBadge` | 文字高+6 | 药丸形；底色取语义 **soft 令牌**；`outlined` 变体 = 透明底 + 语义色描边；可选前置小图标 |
| `SectionCard` | 内容决定 | 圆角 10 + 1px `border`；头下分割线用 `divider`；可选头部图标；⚠️ 必须同时给 `implicitWidth` 与 `implicitHeight` |
| `EmptyState` | 内容决定 | 64×64 圆形"图标井"（`glyphColor` 的 8% 叠加 + 16% 描边）+ 标题 + 描述 + 动作行。**错误态只需传 `glyphColor: Theme.danger` 即自动得到淡红圆底** |
| `SearchField` | 32 | 常态 `surfaceSunken` 底；**聚焦时底色升到 `card` + 边框变 `primary`**（唯一允许改边框的控件）+ 3px 焦点环 |
| `HistoryGroup` | 内容决定 | 头部 = `card`；展开体 = `surfaceSunken`（用"下沉"表达从属关系，避免同亮嵌套）；记录行 = `card` + `divider` 边框，从下沉底上"浮"出来 |
| `UnsupportedPage` | 内容决定 | 与 `EmptyState` 同构（保证"空态家族"一致）；图标井用中性色 —— 这是"尚未开放"，不是"出错了" |
| `CvIcon`（新增） | 可配 | 矢量图标渲染；`name` / `glyph` / `size` / `color` / `strokeWidth` |

### 交互态矩阵

| 状态 | Primary | Secondary | Ghost | Danger | SearchField |
|---|---|---|---|---|---|
| 默认 | `primary` | 透明 + `borderStrong` | 透明 | `danger` | `surfaceSunken` + `border` |
| hover | `primaryHover` | `hover` | `hover` | `dangerHover` | 同默认 |
| pressed | `primaryPressed` | `hoverStrong` | `hoverStrong` | `dangerPressed` | 同默认 |
| focus（键盘） | +2px 焦点环 | +2px 焦点环 | +2px 焦点环 | +2px 焦点环 | 边框→`primary` + 焦点环 |
| disabled | `surfaceSunken` + `disabledText` | `bg` + `disabledText` 边框 | `disabledText` 文字 | `surfaceSunken` + `disabledText` | — |

> **焦点环只在 `visualFocus` 时出现** —— 鼠标点击不显示。这是效率风的细节：
> 键盘用户得到明确反馈，鼠标用户不被多余描边打扰。

---

## 7. 主题机制

### 7.1 三态与数据流

```
用户在设置页点「深色」
        │
        ▼
SettingsPage.qml  →  AppTheme.setMode("dark")          ← QML 上下文属性（C++）
        │
        ▼
src/core/AppTheme.cpp
   ├─ Settings::setThemeMode("dark") + sync()          持久化 ui/themeMode
   ├─ QStyleHints::setColorScheme(Dark)                → Qt 内建控件跟随
   │    （TextField / ComboBox / Switch / Menu / ScrollBar / Drawer…）
   ├─ QGuiApplication::setPalette(buildPalette(dark))  → 不依赖平台主题，
   │                                                      保证无头渲染与真机一致
   └─ emit changed()
        │
        ▼
qml/Main.qml 的 Binding：Theme.darkMode = AppTheme.isDark
        │
        ▼
Theme.qml 的全部令牌重新求值 → 自绘控件整体换色
```

**唯一数据源**是 `AppTheme`：它同时驱动「Qt 内建控件」与「自绘控件」，
两者永远同色 —— 这正是改造前"设置页 TextField 白字白底"缺陷的根治方式。

### 7.2 跟随系统

- 模式 = `system` 时，`AppTheme` 把 ColorScheme 设为 `Unknown`（交还平台）
- 连接 `QStyleHints::colorSchemeChanged`，平台切换深浅色时自动重应用 + 通知 QML
- 显式模式（light/dark）下忽略平台变化
- 设置页会实时显示「当前系统为深色主题，已自动跟随」

### 7.3 自检钩子

`CV_THEME=light|dark|system` 环境变量可**覆盖启动模式**（不写回配置）。
与既有 `CV_START_PAGE` / `CV_SCREENSHOT` 同一套约定，供无头截图与回归验证使用：

```bash
CV_THEME=dark CV_SCREENSHOT=/tmp/dark.png ./bin/CloudVault.exe
```

### 7.4 需要人工同步的一处

`src/core/AppTheme.cpp` 中的调色板色值必须与 `qml/theme/Theme.qml` 的令牌**逐条对应**
（QML 侧负责自绘控件，C++ 侧负责 Qt 内建控件）。该文件用 `[L]` / `[D]` 标注了每个色值
对应的 QML 令牌名，改任一侧请同步另一侧。

> 为什么不消除这份重复：QML 无法把运行时令牌传给 `QPalette`；
> 反过来让 QML 读 C++ 调色板又需要为每个令牌建桥，成本更高。
> 折中是**明确标注 + 文档化**，并把重复限制在这一个文件里。

---

## 8. 可访问性

| 项 | 落地方式 |
|---|---|
| 对比度 | 正文级 ≥ 4.5:1（§2.4 已实测）；`textTertiary` 仅用于非必读提示（3.06 / 3.82:1，达大字标准） |
| 键盘可达 | 全部按钮支持 `visualFocus` 焦点环；既有快捷键保留（`Ctrl+U` 上传 / `Ctrl+D` 下载 / `Ctrl+L` 设置 / `F5`·`Ctrl+R` 刷新） |
| 焦点可见 | 3px 外扩环，`focusRing` = 强调色 35% 透明，不遮挡内容 |
| 触达尺寸 | 桌面指针场景：控件高 32 ≥ Windows 建议的 32px；列表行高 36 |
| 不用颜色单独传达信息 | 状态徽标同时有文字（「已完成」「失败」），不只靠颜色 |
| 深色主题对比 | 深色下强调色为浅色、其上文字为近黑，避免白字压浅底（§2.4 已验证 7.56:1） |
| 不依赖位图 | 图标全部矢量，任意 DPI / 缩放比例下不糊 |

---

## 9. 开发交接

### 9.1 新增/修改令牌

1. 只改 `qml/theme/Theme.qml`（自绘控件）与 `src/core/AppTheme.cpp`（内建控件调色板）
2. **禁止改名或删除既有令牌** —— 它们被 30 个 QML 文件引用，改名会导致整包界面加载失败
3. **禁止新增以 `on` 开头的属性名** —— 会被 QML 解析为信号处理器 `on<Xxx>`，导致单例加载失败
   （例：主色上的文字色叫 `textOnPrimary`，不叫 `onPrimary`）

### 9.2 新增图标

1. 往 `qml/theme/Icons.qml` 的 `paths` 加一条 24×24 路径
2. 若该图标源自某个 emoji，同时在 `glyphAlias` 登记
3. 需要填充而非描边时，加进 `filledNames`
4. 新 QML 文件必须登记进 `resources/resources.qrc`，否则不进 exe

### 9.3 提交前门禁

```bash
python scripts/check_build_registration.py     # 三条规则必须全 OK
qmllint -I <Qt>/qml -I qml <改动过的 qml>       # 只应出现 context property 的 unqualified 告警
```

### 9.4 本次改动的文件清单

| 文件 | 改动 |
|---|---|
| `qml/theme/Theme.qml` | **重写**：新令牌体系 + 完整双主题 + 新增 `divider`/`surfaceSunken`/`textTertiary`/soft 语义色/`fontDisplay`/`radiusPill`/`animSlow` 等（既有属性名全部保留） |
| `qml/theme/Icons.qml` | **新增**：46 个图标路径 + 61 条 emoji 映射 |
| `qml/theme/qmldir` | 登记 `Icons` 单例 |
| `qml/controls/CvIcon.qml` | **新增**：矢量图标渲染 |
| `qml/controls/*.qml`（10 个） | **重写**：统一骨架（图标 + 文字）、统一交互态与焦点环、禁用态改实色、新增 `iconName` 属性 |
| `qml/Main.qml` | 界面骨架 emoji → 矢量图标；菜单去掉 emoji 前缀；新增 `Theme.darkMode` 绑定 |
| `qml/pages/SettingsPage.qml` | **新增「外观」卡片**（分段控件三选一 + 跟随系统状态提示） |
| `qml/pages/FilesPage.qml` | 文件类型图标（📁/📄）→ `CvIcon`；右键菜单与排序菜单去掉 emoji 前缀 |
| `qml/pages/LoginPage.qml` | 品牌区改「应用标记 + 名称」；「稍后」按钮改 `GhostButton` + 图标 |
| `qml/pages/SharesPage.qml` | 撤销确认的 ⚠️ → `CvIcon` |
| `qml/panels/PreviewPanel.qml` | 空态 🗂 → 圆形图标井 + `CvIcon`（与 EmptyState 同构） |
| `qml/dialogs/AboutDialog.qml` | 品牌 ☁ → 应用标记 |
| `qml/dialogs/DeleteConfirmDialog.qml` | ⚠️ → `CvIcon` |
| `src/core/AppTheme.{h,cpp}` | **新增**：主题模式解析 + ColorScheme/QPalette 应用 + QML 桥接 |
| `src/core/Settings.{h,cpp}` | `themeMode` 三态持久化（`ui/themeMode`）+ 旧 `ui/darkMode` 自动迁移（在构造函数中完成，getter 保持 const） |
| `src/core/core.pri` | 登记 `AppTheme` |
| `src/app/main.cpp` | 硬编码浅色调色板 → `AppTheme`；注入 `AppTheme` 上下文属性 |
| `src/controllers/AuthController.cpp` | **缺陷修复**（见 §10） |
| `resources/resources.qrc` | 登记 `Icons.qml`、`CvIcon.qml` |
| `resources/qtquickcontrols2.conf` | 移除 `Theme=Light` 钉死（否则深色模式下内建控件停在浅色） |

### 9.5 验证结果

| 项 | 结果 |
|---|---|
| 编译 | qmake + mingw32-make：**0 error / 0 warning**，产物 `bin/CloudVault.exe` |
| 构建注册门禁 | `scripts/check_build_registration.py` 三条规则全 OK |
| QML 静态检查 | 30 个 QML 文件 qmllint 全部通过（仅 context property 的 unqualified 提示，属预期） |
| 字形映射覆盖 | 46 图标 / 61 映射；脚本校验**全部 glyph 取值均已覆盖**，无"空图标" |
| 对比度 | 浅色/深色正文级全部 ≥ 4.5:1（§2.4 实测） |
| 主题切换 | 浅色 / 深色 两套均出图验证；Qt 内建控件（下拉框 / 输入框 / 开关）跟随变色 |

### 9.6 后续建议（本次未做，属"重构"而非"换肤"）

1. **表格行渲染**：文件列表在有数据时的行内图标已改为 `CvIcon`，但表头底色（`tableHeader` 令牌）
   与 hover 行高亮尚未启用 —— 令牌已就位，可后续开启。
2. **导航改文字侧栏**：当前是 96px 图标+文字竖栏。效率风的常见形态是 200–240px 文字侧栏，
   可承载「最近文件 / 星标 / 标签」等快捷入口 —— 属信息架构调整，需单独立项。
3. **动效**：可为主题切换加 180ms 的颜色过渡（当前切换是瞬时的）。
4. **深色模式下的图标描边**：16px 渲染时描边约 1.17px，个别图标（如 `key`、`dice`）细节偏密，
   可考虑为 16px 单独简化路径。

---

## 10. 顺带修复的缺陷（本次换肤过程中发现）

### 10.1 `--mock`（本地引擎模式）界面被登录门锁死

**现象**：以 `--mock` 启动（离线开发 / 本地引擎模式）时，登录门覆盖整个界面且**无法通过** ——
用户名密码怎么填都失败，主界面完全不可用。

**根因**（`src/controllers/AuthController.cpp::startupCheck`）：

```cpp
if (!m_hb) {            // MockBackend 不是 HttpBackend → m_hb 为空
    setInitializing(false);
    return;             // ← 直接返回，legacyMode 仍为 false
}
```

QML 登录门的显示条件是
`Auth.supported && !Auth.loggedIn && !Auth.legacyMode && !Auth.initializing`，
其中 `supported` 恒为 `true`。于是 `legacyMode == false` 时登录门必然显示；
而登录动作本身也依赖 `m_hb`（为空 → 必然失败）→ **死锁**。

**修复**：`m_hb` 为空时按 legacy 模式处理（本地引擎没有账号体系，本就不该显示登录门）：

```cpp
if (!m_hb) {
    setLegacyMode(true);    // 不显示登录门
    setLoggedIn(false);
    setInitializing(false);
    return;
}
```

**影响面**：仅影响 `--mock` 路径；真实远程服务端路径行为不变。

### 10.2 未修复但已记录

- `PreviewPanel` / `FilesPage` 的**表头底色与 hover 行高亮**未启用（令牌已就位）。
- 深色模式下若用户把系统配色改回浅色，而应用设为显式「深色」，应用保持深色 —— 这是预期行为，
  但设置页的「跟随系统」提示文案依赖 `AppTheme.isDark`，若 C++ 侧未注入该属性则恒显示「浅色」。

---

## 11. 第二轮：字体尺度重标定与界面打磨

### 11.1 字体尺度重标定（根因）

初版令牌把 **pt 当 px 用**：`fontBody: 13` 在 96 DPI 下实际渲染 **17.3px**，
而控件高度只有 32px —— 字几乎顶满按钮，行与行之间也显得松垮。这是"界面看起来陈旧"的
最主要来源（比配色更致命）。

修正方式：按目标像素反推 pointSize（正文目标 13px → 9.75pt），并**同步收紧所有尺寸令牌**
（控件 32→30、行高 36→34、图标 16→14、工具栏 48→44、导航栏 96→84），
避免"字小框大"。字号令牌类型由 `int` 改为 `real`（9.75 / 8.5 等分数值）。

**风险控制**：全仓 `Theme.font*` 只出现在 `font.pointSize:` 上（脚本校验，0 例外），
因此改令牌值即可全局生效，无需改动任何页面。

### 11.2 表格现代化（文件列表）

| 改动 | 说明 |
|---|---|
| **表头可点击排序** | 点「名称 / 大小 / 修改时间」切换排序列；点同一列切换升降序。此前排序只能从右键菜单进入 —— 那是十年前的做法 |
| **排序方向指示** | 当前排序列显示 chevron 上下箭头（新增 `chevron-up` 图标），标签变 `textSecondary` 并加粗 |
| **表头层次** | 高度 30px（比数据行矮 4px）+ `fontMicro` + `textTertiary`。现代表格的层次来自"表头退后、数据前置"，而不是给表头加粗加底色块 |
| **表头交互反馈** | 单元格 hover 高亮 + `PointingHandCursor` |
| **行分隔细线** | 每行底部 1px `divider`（`#EFF1F5` / `#1F242C`）；悬停/选中行不画线，避免与背景色打架 |
| **移除「状态」列** | 该列依赖服务端不返回的 `instant` 字段，**永远显示 `-`**（契约 §9-L3 已记录）。保留一个恒为空白的列是纯粹的视觉噪音；模型角色 `status` 仍保留（契约冻结），仅不再展示 |
| **表头右对齐修正** | 右对齐列的表头内容改为显式 `x: parent.width - width` 定位，不依赖 spacer 的 `fillWidth` 语义 —— 保证表头与数据列**严格对齐**（见 §11.4 客观测量） |

### 11.3 状态条与其它细节

| 改动 | 说明 |
|---|---|
| **状态条收窄** | 高度 34 → **26px**（新增 `statusBarHeight` 令牌），文字降到 `fontMicro` + `textTertiary`。状态条只放"被动信息"，职责是随时可查，不该抢注意力 |
| **空间用量加图标** | 「空间 xxx」前加 `drive` 图标，比纯文字更易扫读 |
| **登录门遮罩** | 由"应用底色 82% 透明"改为标准模态遮罩 `Theme.overlay`（浅色 `#141A29` 32% / 深色纯黑 55%） |

### 11.4 客观验证（像素级测量）

肉眼判断对齐不可靠，因此新增 `scripts/png_probe.py`（纯标准库 PNG 解码器 +
墨迹区间测量），对截图做像素级核验：

| 测量项 | 表头 | 数据 | 结论 |
|---|---|---|---|
| 大小列右边界 | x=733..**753** | x=709..**753** | 右对齐精确一致 ✓ |
| 修改时间右边界 | x=872..**928** | x=821..**931** | 差 3px（图标描边内缩），可接受 ✓ |
| 名称列左边界 | x=**112** | 图标 x=**114** | 一致（差 2px）✓ |
| 表头带高度 | y=143..172 = **30px** | 令牌 `tableHeaderHeight=30` | 一致 ✓ |
| 数据行高度 | y=174..207 = **34px** | 令牌 `rowHeight=34` | 一致 ✓ |
| 行分隔线颜色 | `#1F242C` | 令牌 `divider`（深色） | 一致 ✓ |

用法：

```bash
python scripts/png_probe.py <png> ink --y 145,170 --bg 0A0D11 --tol 45   # 墨迹区间
python scripts/png_probe.py <png> color --x 400 --y 160                  # 取像素色
```

### 11.5 本轮改动文件

| 文件 | 改动 |
|---|---|
| `qml/theme/Theme.qml` | 字号 pt 值重标定（类型 int→real）；尺寸令牌同步收紧；新增 `tableHeaderHeight` / `statusBarHeight` |
| `qml/theme/Icons.qml` | 新增 `chevron-up` 图标 |
| `qml/pages/FilesPage.qml` | 表头重做（可排序 + 指示箭头 + hover）；行分隔细线；移除「状态」列与其列宽令牌 |
| `qml/Main.qml` | 状态条收窄重排 + `drive` 图标 |
| `qml/pages/LoginPage.qml` | 常驻关闭按钮 + 底部「暂不登录」入口；遮罩改 `Theme.overlay`；`loginSkipped` 纳入可见条件 |
| `src/controllers/AuthController.{h,cpp}` | **新增** `loginSkipped` 属性 + `skipLogin()` / `resumeLogin()`；登录成功与切服务器时复位 |
| `src/core/Settings.{h,cpp}` | 新增 `skipLogin()` / `setSkipLogin()`（键 `auth/skipLogin`） |
| `scripts/png_probe.py` | **新增**：截图像素探针（对齐/间距客观测量工具） |

### 11.6 验证结果

| 项 | 结果 |
|---|---|
| 编译 | qmake + mingw32-make：**0 error / 0 warning** |
| 构建注册门禁 | 三条规则全 OK |
| QML 静态检查 | 30 个文件 qmllint 通过（仅 context property 提示） |
| 登录门修复 | ① 未跳过 → 登录门显示且**有 ✕ 与「暂不登录」两个出口**；② `auth/skipLogin=true` → 登录门消失、主界面可用、文件列表正常发起加载 ✓ |
| 表格 | 在有数据的列表上验证：排序指示器、行分隔线、矢量文件/文件夹图标、数值右对齐 ✓ |
| 对齐 | 像素级测量全部一致（§11.4） |
