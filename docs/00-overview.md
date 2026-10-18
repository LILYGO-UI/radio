# LILYGO Radio 架构总览

本应用是基于 LILYGO UI AppKit 和 LVGL 的网络电台播放器，运行目标为 CM0 的
ARM64 Linux 环境，同时保留 SDL2 主机模拟器用于开发和测试。电台目录来自
[Radio Browser](https://www.radio-browser.info/)，音频流由各电台提供。

项目介绍、运行依赖、编译和打包见 [README](../README.md) / [中文 README](../README.zh-CN.md)，
页面行为和开发约束见 [UI 开发规范](01-user-interface.md)，网络请求、配置文件、
收藏持久化与 Logo 缓存见 [服务与缓存说明](02-services-and-cache.md)。

## 数据流

```text
用户输入 -> RadioView -> RadioViewModel -> RadioModel
                |              |             电台列表、当前电台快照、URL、收藏
                |              +-> 单调时钟睡眠定时 -> 到期停止播放
                |              +-> RadioBrowserClient -> curl -> Radio Browser
                |              +-> StreamPlayer -> ffmpeg -> aplay / ffplay
                |
                +-> ArtworkCache -> 工作线程 -> 磁盘原图 / curl 下载
                                               -> ffmpeg 解码 -> BGRA 图像

LVGL 主线程的 200 ms 定时器
    -> 轮询目录和播放器、检查睡眠定时、接收 Logo 结果
    -> Subject 绑定与控件刷新 -> LVGL 渲染
```

目录请求使用独立的 `curl` 子进程和非阻塞管道；Logo 的下载、解码和磁盘操作由
`ArtworkCache` 的工作线程处理。所有 LVGL 对象和图像描述符都在主线程创建、
修改和释放，后台服务不操作 LVGL。目录与 Logo 请求期间仍可使用播放控件。

## 模块边界

以下路径均相对于 `src/`。

| 模块 | 责任 |
| --- | --- |
| `pages/radio/radio_model.*` | 电台列表、独立的当前电台快照、流地址校验、自定义地址和收藏持久化；不依赖 LVGL。 |
| `pages/radio/radio_view_model.*` | 热门目录、名称搜索、分页、播放和收藏命令、睡眠定时；发布 Subject 和供 View 轮询的展示状态。 |
| `pages/radio/radio_view.*` | 单页面内的四个导航标签、播放器、目录/收藏列表、设置、输入与定时弹窗、响应式布局；持有 Logo 服务和 LVGL 图像描述符。 |
| `services/radio_browser_client.*` | Radio Browser 请求参数、JSON 解析、服务器重试、超时、响应大小限制和取消。 |
| `services/stream_player.*` | 音频管道、播放状态、暂停/继续、终止和回收整个播放进程组。 |
| `services/artwork_cache.*` | Logo 下载、原格式磁盘缓存、图像解码、内存结果发布和取消。 |
| `components/` | 语义颜色、基础 LVGL 组件、Subject RAII 封装和模拟器实体键盘输入桥接。 |
| `app.cpp` | AppKit 应用打开/关闭回调，以及播放器、ViewModel、View 的创建和销毁顺序。 |

构建目标也保持依赖分层：`lilygo_ui_radio_core` 编译 Model 和服务，不链接 LVGL；
`lilygo_ui_radio_ui` 在其上链接 `LilyGoUI::Runtime`，承载 ViewModel、View 和应用入口。
当前没有独立路由器，四个底部标签均由同一个 `RadioView` 管理。

## 状态一致性

- 启动立即呈现 `Playing`，同时加载热门目录；不会自动播放。进入 `Stations` 或 `Favorites`
  时列表从顶部开始。目录结果到达前保留四个
  SomaFM 备用电台；每页最多展示 50 条，额外请求一条判断是否还有下一页。
- 搜索和刷新替换目录，加载更多追加结果。正在播放的电台保存为独立快照，替换、
  清空目录或从收藏移除该电台，都不会改变当前流地址或停止播放。
- 收藏保存完整电台快照，播放收藏无需重新查询目录。保存失败时显示错误，内存中的
  收藏状态保持原样；实际收听仍需要可达的广播流。
- 播放状态与目录状态分别维护。目录加载失败不会把播放器切换成错误状态，
  切换标签也不会停止播放。
- 睡眠定时使用单调时钟，由 `poll()` 检查到期并停止播放。暂停、切换电台不会
  重置倒计时；取消定时保留播放，手动停止同时取消定时。定时不跨应用重启保存。

## 图像与生命周期

Logo 来源为电台的 `favicon`，不代表当前歌曲或节目的专辑图。下载的原始图片
保留在磁盘，显示时才解码为带透明留边的 `256x256` BGRA 内存图像。

`ArtworkCache::request()` 返回的像素在对应 URL 被释放、`clear()` 或服务销毁前
保持有效，不会被隐式淘汰。View 释放图像时，必须先解除 LVGL 对象的图像引用，
清除对应的 LVGL 图像缓存和描述符，再调用 `release()`。当前电台的 Logo 优先请求，
列表图像按 LVGL 可见性保留；容量、有效期和文件校验规则见 [服务与缓存说明](02-services-and-cache.md)。

关闭应用时依次释放实体输入焦点和页面定时器、删除 LVGL 对象、解除图像资源并取消
Logo 工作，然后销毁 View 与 ViewModel，最后停止并销毁播放器。目录服务析构会
取消并回收请求子进程，Logo 服务析构会等待工作线程退出，播放器会回收整个进程组。
`ArtworkCache::clear()` 清理内存并取消待处理工作，保留磁盘缓存。

## 失败策略与边界

- 目录请求失败时尝试备用服务器；全部失败后显示错误和刷新入口，保留原有列表。
  首次加载失败时保留四个 SomaFM 电台；成功返回空列表时显示空结果。
- Logo 缺失、加载中、下载或解码失败时显示默认电台图标，不阻止播放。
- 自定义地址必须通过 Model 校验，输入错误保留在弹窗中。通过校验仅表示地址格式
  可接受，不能保证远端流可播放。
- Linux 使用 `ffmpeg -> aplay`，macOS 使用 `ffmpeg -> ffplay`。依赖缺失、音频设备
  不可用或播放子进程失败时显示播放错误；详细诊断见服务文档。
- `Playing` 根据播放进程仍在运行的状态判断，并非音频设备已出声的确认。
  暂停通过挂起进程组实现，不提供广播节目的回看或时间轴定位。
- 备用目录、收藏和 Logo 缓存不包含音频内容；当前没有离线音频下载、实时曲目信息
  或歌曲封面服务。

## 验收

主机配置、编译和测试的前置依赖见 README：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

模拟器配置包含 5 项核心/服务/输入测试和 40 项渲染测试。后者在 `568x1232`、
`1232x568`、`390x844`、`320x568` 四个视口分别覆盖 10 个场景，使用固定目录和
图片响应，不依赖外部电台目录。测试矩阵、视觉检查和预览生成见
[UI 开发规范](01-user-interface.md#验收)。这些测试不替代真实网络与音频设备的播放验证。
