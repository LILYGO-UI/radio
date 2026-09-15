# LILYGO Radio UI 开发规范

本文规定本项目的页面行为、布局、组件、字体与验收要求。开发者和 AI agent 新增或
修改页面、组件、布局、样式、文案或字体时必须遵守本文。

项目介绍与构建入口见 [README](../README.md) / [中文 README](../README.zh-CN.md)，
模块协作见 [架构总览](00-overview.md)，服务限制和配置路径见
[服务与缓存说明](02-services-and-cache.md)。

本文中“必须”和“不得”是强制要求，“应”是默认实现方式。开始编辑前，
必须先检查当前页面、`src/components/` 中的现有能力和相关测试，不得假设
仓库中存在未实现的基类、主题或响应式辅助 API。

## 架构边界

| 关注点 | 位置 | 规则 |
| --- | --- | --- |
| 页面状态和 UI | `src/pages/<page>/` | 页面私有的 Model、ViewModel 和 View 放在同一目录。 |
| 共享领域规则 | `src/domain/` | 仅在业务对象被多个页面实际共享时创建，不得依赖 LVGL。 |
| 外部服务 | `src/services/` | 目录、音频进程与 Logo 缓存不依赖 LVGL，不从后台线程操作 UI。 |
| 通用组件和绑定 | `src/components/` | 复用现有组件和 Subject RAII 封装，明确所有权和销毁顺序。 |
| 生命周期和路由 | `src/app.cpp` | 当前由应用回调管理 View、ViewModel 和播放器；只有增加独立页面时才按需引入路由器。 |

当前业务页为 `src/pages/radio/`，`Playing / Stations / Favorites / Settings` 是同一个
`RadioView` 内的标签，不是四个独立页面。`src/domain/` 与 `src/app_router.*` 当前不存在。

依赖方向必须是：

```text
View -> ViewModel -> Model
```

- Model 保存业务状态和领域规则，必须能在不初始化 LVGL 的情况下测试。
- ViewModel 将 Model 转换为展示状态，通过 Subject 发布变化，并提供用户命令。
- View 只负责 LVGL 对象创建、布局、事件转发和状态绑定，不得直接修改 Model。

Logo 服务由 View 持有，因为图像描述符和像素引用与 LVGL 对象共同存活。其下载、
解码与文件操作仍封装在 `ArtworkCache` 中；不得在 View 的事件回调中执行阻塞网络请求。
标题和播放状态使用 Subject 绑定，其余列表、收藏、倒计时和图像状态由 200 ms 页面
定时器统一刷新，不应假定每项展示状态都有单独的 Subject。

数据流必须保持单向：

```text
用户输入 -> View 回调 -> ViewModel 命令 -> Model 更新
         -> Subject -> View 绑定或 observer -> LVGL 渲染
```

## 页面与交互

页面从 AppKit 传入的内容根容器顶部开始，不增加重复的应用标题栏。系统状态栏
与底部系统指示条由 AppKit 管理，应用底部导航位于自己的内容区域内。

| 标签 | 当前行为 |
| --- | --- |
| `Playing` | 显示电台名称、元数据、Logo、播放状态、暂停/继续、停止、收藏、睡眠定时和添加流入口。 |
| `Stations` | 默认启动标签，显示名称搜索、热门目录、刷新、分页、Logo 与电台行收藏按钮。 |
| `Favorites` | 复用电台列表展示已保存的快照，隐藏目录搜索和刷新控件；空列表显示 `No favorites yet`。 |
| `Settings` | 显示 `Sleep Timer` 和 `Add a stream` 两项设置入口；切换至此保留播放。 |

- 点击电台行开始播放并选择 `Playing`；当前电台正在连接、播放或暂停时，再次点击
  该行切换暂停/继续。电台行内收藏按钮不得触发整行的播放事件。
- 未选择电台时，播放按钮尝试播放目录首项；目录为空且没有当前选择时禁用。
  停止按钮仅在连接、播放或暂停时可用，收藏当前电台按钮在没有选择时禁用。
- 目录加载、空结果、请求失败与收藏保存失败必须有独立可见反馈。新查询期间保留
  旧列表，目录错误不能覆盖播放器状态；收藏保存失败不能假装已完成收藏。
- 播放区与设置页共享同一个睡眠定时。选项为 15、30、45、60、90 分钟，初始选中
  30 分钟；关闭弹窗不取消已启动的定时，取消定时不停止播放。
- Logo 表示电台标识，缺失、加载中或失败时保留默认音频图标；不得显示为实时歌曲封面。

### 搜索与地址输入

- 名称搜索位于 `Stations` 列表上方，提交时去除首尾空白；空查询和 `Popular`
  均请求热门目录，刷新重试当前查询。搜索提交后列表回到顶部，输入内容保留。
- 搜索软键盘是底部浮层，会覆盖导航栏和底部内容，不改变主体、迷你播放器或导航栏
  的布局尺寸。打开、收起及跨 800 px 断点调整时，搜索文本和应保留的焦点不得丢失。
- 搜索提交或取消会收起键盘；取消不清空已输入的文字。切换标签、打开睡眠定时
  或添加流弹窗时必须先解除搜索焦点并收起键盘。
- `Add a stream` 使用独立弹窗及底部键盘，打开时填入上次记住的自定义地址。
  地址无效或播放器未能启动时显示错误并保持弹窗；提交成功后选择 `Playing`。
- 主机模拟器通过 `HostTextInput` 支持实体键盘、输入法提交、光标移动、Shift 选择、
  删除、Enter 提交、Esc 取消及剪贴板操作。macOS 使用 `Cmd+A/C/X/V`，Linux 使用
  `Ctrl+A/C/X/V`。SDL 事件监听器只入队，由 LVGL 定时器修改文本对象；设备构建中
  该桥接不处理输入，触控输入使用 LVGL 软键盘。

## 视觉规范

应优先复用 `src/components/components.hpp` 中的语义颜色，不得在页面中重复定义
相同用途的颜色。

| 颜色角色 | 用途 | 色值 | 不透明度 |
| --- | --- | --- | --- |
| 页面背景 | 页面根容器 | `#F2F2F7` | `100%` |
| 容器背景 | 头部、面板和卡片 | `#FFFFFF` | `100%` |
| 容器标题 | 标题和小标题 | `#1A1A1A` | `100%` |
| 正文 | 正文和主要数值 | `#262C33` | `100%` |
| 边框 | 分隔线和容器边界 | `#D8DDE3` | `100%` |
| 主操作 | 播放按钮、选中导航、播放状态 | `#15735D` | `100%` |
| 强调 | 连接与暂停状态 | `#E4573D` | `100%` |
| 次要文本 | 元数据、辅助状态、未选中导航 | `#68737F` | `100%` |
| 控件背景 | 次要按钮、键盘、Logo 占位 | `#EDF1F4` | `100%` |
| 选中背景 | 当前电台、迷你播放器和按下状态 | `#E7F4EF` | `100%` |
| 错误 | 输入、目录、收藏和播放错误 | `#C83E4D` | `100%` |
| 收藏 | 已收藏星标 | `#C88A16` | `100%` |
| 弹窗遮罩 | 地址与睡眠定时弹窗背景 | `#18221E` | `40%` |

新增颜色时必须先定义语义角色、使用场景、色值和透明度；两个页面共享的
颜色应提升到 `src/components/`。不得用渐变替代明确的颜色角色。

播放、停止、刷新、收藏和添加流优先使用 AppKit 字体链内的 LVGL / Font Awesome
图标，带有稳定的控件尺寸和悬停提示。导航使用图标加文字，状态变化需同时反映到
图标、选中/禁用状态或文本，不能只靠颜色表达。页面分区保持平整，弹窗和 Logo
使用有边界的容器，避免层层嵌套的卡片。

## 字体与文本

- 显示文本的 UI 源文件必须包含 `<cm0/typography.h>`。除下述应用专用字体场景外，
  LVGL 文本对象必须使用 `lilygo_ui_font_get(size)` 返回的 AppKit 字体。
- 必须在 `lv_init()` 完成后获取字体。`14`、`22`、`28`、`36` 和 `48` 像素是
  推荐的基础字号；产品层级或布局确有需要时，可向 `lilygo_ui_font_get(size)`
  传入其他正整数字号。
- AppKit 在首次请求某个字号时创建并缓存对应的完整字体链，后续相同字号复用该
  缓存。不得根据视口尺寸连续计算字号，也不得为相近文本层级引入大量不同字号；
  每个不同字号都会占用额外字体缓存，直至字体系统反初始化。
- 设置文本样式前必须确认字体指针非空。返回的字体对象及其 fallback 链由 AppKit
  共享和管理，应用不得修改或释放。
- 相同信息层级在同一应用中应使用一致的字号。缩小字号不得作为修复溢出的
  首选方案；应先精简文案、调整布局，或按产品要求换行和截断。
- 不得在公共 UI 中引用 `lv_font_montserrat_*`，也不得复制、改名或随应用重新打包
  AppKit 字体。

AppKit 在运行时通过 LVGL FreeType 加载共享字体文件。`lilygo_ui_font_get()` 返回
以 Inter 为主字体、Source Han Sans CN 为中文 fallback、Font Awesome 为图标字形
fallback 的字体链。中西文混排必须使用同一个字体链，不得为了切换字体拆分标签，
也不得依赖操作系统字体发现或默认字体回退。

### 应用专用字体

仅当 AppKit 运行时字体确实缺少应用所需字形时，才可在 `assets/fonts/` 提供
应用专用的 TTF、OTF 或 WOFF 字体。不得复制或重新打包 AppKit 已提供的 Inter、
Source Han Sans CN 或 Font Awesome 字体。应用专用字体必须满足：

- 通过 `lv_freetype_font_create()` 按当前 UI 字号在运行时加载；
- 将应用专用字体作为引用它的 LVGL 对象的主字体，并将其 `fallback` 设为同字号的
  `lilygo_ui_font_get(size)` 结果；
- 不得修改 `lilygo_ui_font_get()` 返回的共享字体对象或它的 fallback 链；
- 字体对象的生命周期必须不短于引用它的 LVGL 对象；移除所有引用后，必须通过
  `lv_freetype_font_delete()` 释放；
- 字体文件必须通过应用的安装和打包规则部署到稳定的运行时路径；
- 可通过可重复的子集化流程仅保留所需字形以减小安装体积，但不得手工修改生成的
  字体文件。

AppKit 的 Inter、Source Han Sans CN 和 Font Awesome 字体按 SIL Open Font License 1.1
分发。应用不得将它们描述为“无版权字体”或单独销售。包含字体的分发物必须
保留 `NOTICE.md` 和 `OFL-1.1.txt`；本项目通过 AppKit 安装辅助函数包含共享字体的
许可文件。新增应用专用字体时，必须同时记录字体来源，并补充该字体的许可文件
和打包规则。

## 响应式布局

当前结构断点依据 `app_surface` 的内容宽度，`>= 800` px 使用双栏，否则使用单栏；
不能简单以设备朝向代替这一判断。

| 布局条件 | 页面结构 |
| --- | --- |
| 内容宽度 `< 800` px | `Playing` 显示完整播放器；`Stations` / `Favorites` 显示列表，已选择电台时在导航上方显示迷你播放器。 |
| 内容宽度 `>= 800` px | 播放器与当前目录或收藏列表并排，播放器宽度约为内容宽度的三分之一并限制在 320 至 440 px；不显示重复的迷你播放器。 |
| `Settings` | 两种宽度下均独占主体区域；已选择电台时仍显示迷你播放器。 |

导航栏固定高 72 px，迷你播放器高 64 px。Logo 依据可用高度调整尺寸，短视口下
播放器允许纵向滚动；URL 弹窗位于键盘上方，睡眠定时面板在窄屏底部、宽屏居中，
内容不足以容纳时可滚动。点击迷你播放器本体进入 `Playing`，其播放按钮只切换播放状态。

- 页面主体必须使用 LVGL Flex 或 Grid，并结合百分比尺寸、`LV_SIZE_CONTENT`、
  最小/最大尺寸、对齐、间距和滚动容器分配空间。
- 不得用大量绝对坐标组织页面主体，不得为横屏、竖屏或不同尺寸复制整套
  页面。
- 固定像素值只能用于有明确约束的尺寸，例如图标、边框、间距、控件下限和设计
  token。
- 连续尺寸变化应由 layout 自动处理。只有导航方向、栏数、内容优先级或交互
  方式发生结构变化时才设置断点。
- 断点必须依据父容器的实际可用宽高，不得依据 CPU 架构、设备型号或应用展示
  模式。可复用组件不得假设自己占满 display。
- 尺寸变化的布局更新必须可重复执行，不得把重建整个页面作为常规适配方式，
  并必须保留业务状态、焦点、滚动位置和用户输入。

响应式结果必须保证：

- 文本按产品要求换行或截断，不与相邻内容重叠；
- 主要操作始终可见、可达，并有足够的命中区域；
- 内容增多、文案变长或字体回退时，容器可扩展、重排或滚动；
- 横竖屏和尺寸切换后业务状态和用户输入不丢失。

## LVGL 对象和生命周期

- View 必须在调用者传入的 `parent` 下创建顶层对象，不得假设 parent 是当前
  screen 或整个 display。
- 页面 View 必须保存自己的顶层 LVGL 对象，并提供可重复调用的销毁流程。
- LVGL 父对象负责删除子对象。销毁父对象后必须清空所有缓存的子对象指针，
  不得重复删除。
- 事件回调使用 `user_data` 时，其指向对象的生命周期必须覆盖事件注册期间。
- 优先使用 LVGL 对象绑定的 observer。全局 observer、输入监听器、timer 和其他非对象
  绑定资源必须在销毁时显式注销。
- ViewModel 必须比绑定其 Subject 的 View 存活更久。Subject 应使用现有 RAII 封装，
  不得复制或移动底层 `lv_subject_t`。
- 延迟页面切换时，不得在当前 LVGL observer 或事件回调中同步删除触发回调的
  页面。
- 需要被渲染测试定位的关键对象必须设置稳定、有语义的 `lv_obj_set_name_static()`
  名称。

### Logo 引用与销毁

`ArtworkCache` 返回的像素由服务持有，`lv_image_dsc_t` 由 View 持有。释放 URL 对应的
内存前必须先清除所有 LVGL 图片对象对该描述符的引用，调用 `lv_image_cache_drop()`
清除 LVGL 缓存，再删除描述符并调用 `ArtworkCache::release()`。待处理请求无法立即
释放时，后续轮询应再次尝试；不得让描述符指向已释放像素。

View 销毁时先解除实体输入焦点和删除页面定时器，再删除页面对象、清除图像描述符
与内存请求。`HostTextInput` 析构注销 SDL 监听器并删除自身定时器；ViewModel 及其
Subject 必须在 View 之后销毁。服务析构负责取消并回收子进程、等待工作线程。
`ArtworkCache::clear()` 保留磁盘缓存，不能将它描述成用户可用的磁盘缓存清理功能。

## 新增页面

新增页面前必须先检查 `src/pages/radio/` 和现有测试。一个完整页面通常包含：

1. `src/pages/<page>/<page>_model.cpp/.hpp`：页面私有状态和领域规则；
2. `src/pages/<page>/<page>_view_model.cpp/.hpp`：命令、展示状态和 Subject；
3. `src/pages/<page>/<page>_view.cpp/.hpp`：LVGL 对象、绑定、回调和响应式布局；
4. `src/app.cpp` 中的创建和销毁，多页面应用则按需新增 `src/app_router.cpp/.hpp`；
5. Model、ViewModel 和用户可见行为的测试；
6. 修改后的预览图和用户文档。

页面私有 Model 与 ViewModel/View 放在同一目录，但仍必须编译到不同依赖层。纯 Model
代码不得因为目录内聚而链接 LVGL。

## 新增通用组件

只有组件被多个页面使用，或它明确承担通用绑定、样式或资源生命周期时，才放入
`src/components/`。否则应保持在所属页面目录。

通用组件必须：

- 在调用者传入的 parent 下构建；
- 以父容器的可用尺寸进行响应式布局；
- 复用语义颜色和 AppKit 字体；
- 声明所有权、回调 `user_data`、observer、timer 和销毁责任；
- 在最长文案、字体回退和支持视口下不发生重叠、裁切或不可达操作。

## 验收

每次 UI 修改必须至少完成：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

主机模拟器配置当前注册 45 项测试，具体定义以 [CMakeLists.txt](../CMakeLists.txt) 为准。

| 测试 | 覆盖范围 |
| --- | --- |
| `radio-core` | 无需初始化 LVGL 的 URL 校验、选择快照、自定义地址与收藏持久化。 |
| `radio-browser` | 固定目录响应、JSON / Unicode 解析、请求参数、大小限制、服务器回退和取消。 |
| `radio-artwork` | Logo 下载与解码约束、内存/磁盘缓存、完整性、过期和取消。 |
| `radio-view-model` | 播放命令、异步目录、分页、保留当前选择、收藏、睡眠定时和 Subject 发布。 |
| `radio-host-input` | 模拟器实体键盘、文本输入和编辑；仅在模拟器构建中注册。 |
| `radio-render-<viewport>[-<scenario>]` | 四个视口的布局、点击、输入、状态切换与图像像素检查，共 40 项。 |

| 视口后缀 | 分辨率 |
| --- | --- |
| `portrait` | `568x1232` |
| `landscape` | `1232x568` |
| `mobile` | `390x844` |
| `compact` | `320x568` |

每个视口包括播放器（无场景后缀）、`dialog`、`search`、`empty`、`error`、`stations`、
`favorites`、`settings`、`sleep`、`artwork` 共 10 个场景。目录与图像使用固定响应；
测试会验证搜索浮层不改变底层布局、跨断点保留输入和焦点、收藏操作不误触播放、
定时到期停止，以及图像描述符和最终渲染像素。渲染文件输出到 `build/host-simulator/`，
例如 `radio-compact-dialog.ppm`。这些测试不验证真实广播流和音频设备出声。

测试通过不能替代视觉检查。修改 UI 时还必须检查四种视口的相关渲染产物，
并确认：

- 布局不依赖单一固定分辨率；
- 文本无缺字、错误回退、重叠或裁切；
- 主要操作可见、可达，按钮文案和命中区域正常；
- 尺寸或方向变化后布局正确重排，业务状态、焦点、滚动位置和用户输入保留；
- 页面背景、容器、文本、边框和主操作使用正确的语义颜色；
- 关闭页面后无野指针、重复删除、未注销 observer、timer 或输入监听器。

修改 SDK、工具链、安装或打包相关 UI 资源时，还必须完成 `cm0-cross` 构建和
Debian 打包验证。

### 更新预览

README 使用 [竖屏播放器](../assets/previews/radio-main.png) 和
[列表内搜索](../assets/previews/radio-search.png) 两张预览。它们来自 `radio-render-ui`，
使用固定电台数据；`--status-bar` 增加 AppKit 状态栏与底部系统指示条，固定时间为
`9:41`、电量为 `82%`。常规 CTest 渲染不带此选项。

完成主机构建后，在仓库根目录运行以下命令更新这两张预览，需要 `ffmpeg`：

```sh
build/host-simulator/radio-render-ui 568 1232 build/host-simulator/radio-main-status.ppm main --status-bar
build/host-simulator/radio-render-ui 568 1232 build/host-simulator/radio-search-status.ppm search --status-bar
for preview in main search; do
    ffmpeg -y -loglevel error -i "build/host-simulator/radio-${preview}-status.ppm" \
        -frames:v 1 "assets/previews/radio-${preview}.png"
done
```

设置页和横屏检查图可先输出到构建目录，不必加入 README：

```sh
build/host-simulator/radio-render-ui 568 1232 build/host-simulator/radio-settings-status.ppm settings --status-bar
build/host-simulator/radio-render-ui 1232 568 build/host-simulator/radio-landscape-status.ppm main --status-bar
for preview in settings landscape; do
    ffmpeg -y -loglevel error -i "build/host-simulator/radio-${preview}-status.ppm" \
        -frames:v 1 "build/host-simulator/radio-${preview}-status.png"
done
```

`artwork` 场景默认使用仓库中的 `assets/app-icon-radio.png`，也支持通过测试专用的
`RADIO_RENDER_ARTWORK_IMAGE` 指定本地图片。该变量不是应用运行配置；生成的预览
仍应明确反映固定测试数据，不能当作真实电台连通性或实时节目信息的证据。
