# ESP32 UI 项目结构

固件由设备服务、共享 UI 层和各应用模块组成。`agent_ui::Runtime` 注册顶层应用、分发生命周期，并连接导航、状态栏、待机与应用展示接口。

## 目录

| 目录 | 内容 |
| --- | --- |
| `main/boards/` | 板级硬件、音频、存储和电源服务 |
| `main/ai/` | AI 操作与语音采集协调 |
| `main/display/agent_ui/core/` | 导航、主题、状态栏和待机等共享服务 |
| `main/display/agent_ui/components/` | 公共 LVGL 控件、表情和绘制资源 |
| `main/display/agent_ui/apps/` | 按应用组织的界面、状态与业务接线 |
| `components/codex_remote/` | Codex WebSocket 客户端、消息队列与解析管线 |
| `external_apps/` | 外部 App SDK、示例和构建入口 |
| `scripts/hardware/` | 固件构建、打包、烧录和串口监控 |
| `scripts/tools/` | 字体、语言、音频和资源生成工具 |
| `patches/` | 第三方驱动及组件补丁 |

## 应用模块

应用按职责使用以下模块；简单页面可以只提供 View。

| 模块 | 职责 |
| --- | --- |
| Contract | 意图、事件、命令和共享值类型 |
| ViewState | 可渲染状态，不保存 LVGL 或硬件句柄 |
| Controller | 处理意图、事件和状态转换 |
| Adapter | 调用设备、存储、网络和协议服务 |
| View / Renderer | 创建控件、渲染状态并发出交互意图 |
| Module | 连接 Controller、Adapter、View 和应用生命周期 |

Controller 不依赖 LVGL，Adapter 不保留界面对象。共享控件放在 `components/`，应用专属布局保留在对应的 `apps/<app>/` 中。各应用的 `sources.cmake` 登记源文件，由 `agent_ui/sources.cmake` 汇总。

Home 通过 Module/View 提供展示接口。Network 和 Bluetooth 嵌入 Settings，Camera 作为顶层应用注册。Files 的 Controller 管理路径、分页和请求票据，Adapter 管理存储、USB 占用与后台 worker，Repository 执行文件操作；现有 `FilesView` 入口负责兼容接线。

状态栏的 Provider 采集值快照，Policy 决定图标和显示状态，StatusBar 拥有 LVGL 控件及动画。电池快照由后台采样服务发布，网络模式通过失效通知更新缓存。

`CodexCaptureCoordinator` 通过 Application 提供的服务接口协调语音采集和实时音频发送。`CodexProtocolService` 管理命令接纳、连接上下文与值事件，Codex View 负责对话呈现。底部语音、停止和菜单控件由 `codex_voice_footer` 创建。

## 后台工作与线程

网络解析、发送、文件操作和电池采样在后台执行；UI 回调只应用结果。跨线程消息携带页面、连接或请求代际，关闭页面、取消请求或重连后丢弃过期结果。

UI 调度队列最多保留 32 项和 2 MiB 载荷，每轮最多执行 8 个回调，并在回调之间检查 2 ms 时间预算。时间预算不能打断正在执行的回调，因此回调中不得等待网络、文件系统或 I²C。

网络入队、完成本地 WebSocket 写出和收到服务端确认是不同状态。`on_sent` 在传输 worker 完成整条本地写出后执行，调用方须保证线程安全，并将界面工作交回 UI 所有者。

Files 的请求只传路径、票据和代际。目录按 48 项分页，目录条目与名称使用 16 KiB 预算，文本预览最多读取 48 KiB。本地文件操作持有 SD lease，USB 切换等待 lease 释放；图片解码和缓存的 lease 覆盖其资源生命周期。

## 绘制资源

`RenderSnapshotBuffer` 拥有快照内存、稳定的 LVGL 描述符以及加速注册。快照对象不可复制或移动；捕获前注销旧注册，释放时先注销并清理图像缓存，再释放内存。调用方负责捕获时机、源控件状态和异步取消。

`opaque_render_acceleration` 提供 RGB565/RGB888 PPA 拷贝、边界检查和缓存同步。不支持或失败的操作使用软件绘制。`expression_acceleration` 提供 A8 混合与公开兼容接口，`display_render_telemetry` 统计刷新、脏区、flush 和等待时间。

界面修改与快照操作在 LVGL 所有者线程或显示锁内执行。显示回调引用的遥测对象须在回调期间保持有效。

## 开发命令

从仓库根目录运行：

```sh
npm run package
npm run build:apps
```

固件与打包产物位于 `build/`。触控、音频、SD 卡、待机恢复及硬件加速性能可在设备上检查。

工具和参数见 [开发命令](../scripts/README.md)，设备操作见 [开发与烧录](development.md)。
