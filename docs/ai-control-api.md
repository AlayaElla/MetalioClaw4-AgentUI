# AI 控制与可用性接口

设备 AI、内置功能和外部 App 使用统一接口。Codex 实时语音占用音频时，禁用的是设备助手入口；Codex 自身的音频通路继续工作。顶部表情显示设备助手状态，点击通过首页同一控制器开始或结束对话。

## 1. 通用 AI 禁用接口

头文件：`main/ai/ai_availability.h`。

```cpp
auto token = ai::Availability::Get().AcquireBlock("my.module", "正在录音");
// 保存 token；结束、失败、取消、卸载路径都要释放。
ai::Availability::Get().ReleaseBlock(token);
auto state = ai::Availability::Get().GetSnapshot();
// state.available / state.generation / state.reasons
```

- 多个所有者可以同时持有令牌，只有全部释放后才恢复。重复释放返回 false。
- `ReleaseOwner(owner)` 用于拥有者清理；不要释放其他模块的令牌。
- `generation` 在策略变化时递增，用于拒绝跨禁用周期的排队任务。
- 令牌只管理设备助手的可用性，不授予麦克风或扬声器所有权。策略立即生效，音频停止由 Application 队列处理；录音/播放仍须走相应音频服务的启动与清理流程。
- `SetObserver` 由 Application 统一占用；业务模块查询快照，不要覆盖它。
- 禁用统一覆盖唤醒词、顶部点击、手动监听、特殊交互和设备 AI 音频/回调。用户的唤醒设置独立持久化，临时禁用不会改写它。
- 已接入 Codex 实时语音、Codex 听写、外部录音、电话与待机生命周期。
- MCP `self.ai.availability` 返回 `available`、`generation`、`wakeEnabled`、`reasons`。禁用期间仍允许查询可用性、能力描述、现有操作结果及取消。

## 2. 通用能力协议

头文件：`main/ai/ai_capabilities.h`。Provider 注册描述、参数 schema、状态查询、调用、结果查询和取消回调。触碰界面的操作使用 `main/ai/ai_ui_operation.h`，在 UI 线程执行。

| MCP 工具 | 参数 | 用途 |
| --- | --- | --- |
| `self.capabilities.list` | 无 | 发现能力 ID |
| `self.capabilities.describe` | `id` | 查询参数 schema 和状态 |
| `self.capabilities.invoke` | `id`, `args`, `request_id`, `generation` | 调用；`args` 是 JSON 对象的字符串 |
| `self.capabilities.result` | `operation_id` | 查询异步结果 |
| `self.capabilities.cancel` | `operation_id` | 请求取消 |

例如先 describe `system.display`，再 invoke：

```json
{"id":"system.display","args":"{\"action\":\"volume\",\"value\":40}","request_id":"volume-001"}
```

结果包含 `operationId`、`status`、`generation`、`result` 和失败原因；查询时将 `operationId` 传入 `operation_id` 参数。状态为 `pending / succeeded / failed / cancelled`。`pending` 只表示已排队，必须继续查询。相同 `request_id` 与参数复用回执，参数不同会拒绝；去重只在进程内的有限回执保留期内有效。

UI 操作默认超时 30 秒，原生请求可设 100–60000 毫秒。注册表最多 48 个 provider、16 个进行中调用、64 个回执；输入 JSON 最多 4096 字节、嵌套最多 16 层。schema 校验支持 required、type、enum、数值范围、maxLength、additionalProperties 等本项目使用的子集。

取消防止后续派发并通知 provider，不会回滚已经完成的拍照、删除、拨号或文本提交。断开、App 卸载和超时均需要终结回执；不能将单纯排队报告为业务完成。

## 3. 已接入能力

以运行时 `describe` 返回的 schema 为准。

| 能力 | 操作 |
| --- | --- |
| `system.navigation` | 列出内置/已安装 App、打开 App、回首页 |
| `system.display` | 音量、亮度、主题、强调色、待机时间、唤醒设置 |
| `camera.control` | 拍照、保存、丢弃、效果、相册、查看和删除照片 |
| `files.storage` | SD 卡列表、读取、预览、删除、容量；USB 导出/忙碌时拒绝 |
| `system.network` | 状态、已保存 Wi-Fi、扫描、连接、传输模式、SIM |
| `system.bluetooth` | 状态、启用、扫描、连接、音频模式、重置 |
| `system.phone` | 拨号、挂断 |
| `codex.control` | 任务列表/状态、选择/新建、模型/推理强度/快速模式、停止、实时语音、字幕偏好、重连、交互答复、发送文本 |

网络切换需要重启时返回 `switchScheduled`，不代表重启后已联网。拨号返回 `dialAccepted`，不代表对方已接通。文件预览回执表示预览界面打开，内容解码仍以界面结果为准。

Codex 文本发送流程：读取 `list/state` → 必要时 `select/new` → 从新状态取得 `host_id`、`stream_id` 和 `thread_id` 或 `draft_id` → 调用 `text`。PC 桥检查目标仍匹配后执行提交，并去重。`submissionConfirmed:false` 表示已完成桌面输入/提交动作，但尚无 Codex 持久化消息确认；调用方不可描述为已收到模型回复。

实时语音开始后会持有设备 AI 禁用令牌。结束可使用当前语音操作的取消接口或 Codex 页面结束按钮；不能依赖已被禁用的设备助手再次发起命令。

## 4. 外部 App SDK

头文件：`external_apps/sdk/metalio_app_api.h`。扩展追加在 ABI 1 尾部，App 使用前须检查 `struct_size`、能力位和函数指针，旧固件不保证提供这些函数。

- manifest 的 `ai_actions` 声明 `id/title/description/args_schema`，ID 必须以 `<app.id>.` 开头。安装扫描即可发现，无需先运行 ELF。
- App 启动后用 `ai_register_action` 绑定处理、取消和状态回调；同步返回结果，异步返回 pending 后调用 `ai_complete_action`。
- 同步回调的 `result_json/error` 指针必须在返回后仍有效（使用静态存储或 App 实例成员）；宿主随后复制，不能指向回调栈上的局部数组。异步 `ai_complete_action` 在调用内复制其参数。
- 自动调用会先打开宿主、装载目标 App、等待动作注册，再在 UI 线程调用处理器。卸载会取消进行中的动作，并隔离旧实例的回调。
- `ai_unregister_actions` 清理运行实例的绑定，不移除已安装 manifest 的静态发现信息。
- `ai_acquire_block(host, reason)` 返回本实例令牌；`ai_release_block(host, token)` 只释放本实例的令牌；`ai_get_availability` 查询可用性和代次。卸载自动回收遗留令牌，每实例最多 8 个。
- 所有 SDK AI 函数和 ELF 回调在 UI 线程使用。状态回调预留在 SDK 中；当前静态发现不在后台线程调用 ELF 状态函数，动作应在实际结果中返回业务状态。
- `metalio_app_json.h` 提供示例 App 的严格 JSON 读取，支持 Unicode，拒绝嵌入 NUL、非法代理项和超深嵌套。

示例已接入：收音机选台/播放/暂停/恢复、计算器计算、图片查看器切图、宠物互动。每个 manifest 给出具体 schema。新 App 接同一套声明与回调即可扩展。

## 5. 验证入口与边界

```powershell
node scripts/check-ai-availability-runtime.cjs
node scripts/check-ai-capabilities-runtime.cjs
node scripts/check-ai-ui-operations.cjs
node scripts/check-codex-ai-provider.cjs
node scripts/check-external-ai-json.cjs
python -m unittest discover -s external_apps/tests
```

宿主检查直接编译生产 C/C++ 实现，要求 Windows MSVC；registry/provider 检查依赖已获取的 cJSON 组件。registry 检查还编译完整计算器示例，验证计算、清空、模式与错误回执。PC 目标绑定测试位于 CodexRemote 的 `scripts/tests/codex-targeted-text.test.js`。

宿主测试和固件编译无法替代真机验证：顶部动画、音频互斥与恢复、Wi-Fi/SIM/蓝牙、拍照、电话、外部 ELF 生命周期以及 Codex 桌面真实发送，仍需设备联调。根目录 `sdkconfig` 已修正为 C5/SDIO，并通过标准 `scripts/package-esp32.cmd` 入口完成构建和打包；产物位于 `build/esp32/`。
