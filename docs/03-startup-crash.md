# 实体键盘环境下的启动闪退（2026-09-29）

## 原因与复现

目标板上的 Radio 在输出 `UI ready` 后不到一秒收到 `SIGSEGV`，Launcher
记录退出码 139。已安装程序与本地修复前 ARM64 构建的 SHA-256 一致：
`3dce7278a8c23a903de5f40a013ac5b937130f1758ae9d1e191eeb7726ab247d`。
GDB 捕获的主线程调用栈为：

```text
focus_next_core
lv_group_add_obj
collect_focusable（递归遍历）
input_housekeeping
lv_timer_handler
cm0_app_run
```

AppKit 的 `hide_software_keyboards()` 在遍历界面树时，将普通控件也移出
键盘焦点组。焦点维护随后重新加入控件、触发同步焦点回调，重入的遍历破坏
正在处理的焦点组，导致 LVGL 访问无效焦点。

原有 45 项测试都能通过，因为视图渲染与输入测试未运行完整的
`cm0_app_run()` 生命周期。新增的完整程序竖屏测试在旧程序上稳定以
`SIGSEGV (-11)` 失败；旧程序的横屏测试未复现该崩溃。

## 修复

将独立 AppKit 源码中已有的修复同步到本应用的
`third_party/cm0-appkit/src/runtime.cpp`：只隐藏实际的 `lv_keyboard`
对象并将它移出焦点组，保留普通控件的成员关系。LVGL 软件键盘是单个
按钮矩阵对象，各按键并非需要逐一移出焦点组的独立子对象。

AppKit 静态链接进 Radio，因此必须重新构建并安装 Radio；只更新目标板
SDK 包不会改变现有可执行文件。

## 验证

- 新增 `radio-runtime-portrait` 和 `radio-runtime-landscape`，使用 SDL
  dummy 驱动启动完整应用、固定目录响应以及隔离的配置和封面缓存。
  首帧后继续运行至少一秒，覆盖多轮 50 ms 输入维护和 200 ms 目录轮询，
  再发送 `SIGTERM`，要求程序正常退出。
- 修复后重新构建主机程序，全部 47 项 CTest 通过。
- ARM64 Release 交叉编译及 Debian 打包成功。
- 目标板以普通用户 `pi`（UID 1000）运行，横竖屏各两轮，每轮首帧后
  持续运行八秒，四轮均以状态 0 退出。首帧时间为 172–211 ms。
  每轮确认进程打开 DRM、触摸屏和实体键盘设备。
- 已安装 `dist/lilygo-ui-radio_0.1.0_arm64.deb`，并确认安装后的程序与已
  验证程序的 SHA-256 一致：
  `e9a7ab58eb5110d2511c84b143acf5d850d185d08fc1a9d60bd55923a90e22ca`。
  旧程序保留在目标板 `/tmp/lilygo-ui-radio-before-focus-fix`。
  Launcher 服务已恢复运行，`dpkg --audit` 无异常。

测试覆盖此次启动故障和短时间运行，不验证实际电台音频输出或长期稳定性。
