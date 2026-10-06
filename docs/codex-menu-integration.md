# Codex menu state integration

进入 Codex 页面后，收到可用任务列表时自动打开第一个已绑定任务。退出再进入时，
优先恢复上次任务，按 host 和任务 ID 匹配当前位置；若该任务已不在列表且不再是当前任务，
则回到第一个可用任务。记忆在本次开机期间保留。未连接或列表为空时等待同步，
页面停留期间的普通列表刷新不会触发自动切换，仍可通过菜单手动选择其他任务。

The device does not invent task titles, binding, model values, or running
state. The PC bridge is the source of truth and sends `codex_state` version 1.
`selectedSlot` is either `null` or a slot from 0 through 5; unbound slots stay
disabled and display `待同步` until a real state snapshot arrives.

The device sends `codex_action` with a unique `request_id` for task selection,
new task, model, effort, and Fast changes. A control remains pending until a
matching `codex_action_result` succeeds. Selection and setting values are
redrawn from the returned state or the next `codex_state`; local taps never
optimistically change a task. `new_task` only opens the native PC task page;
it does not bind a device slot.

The optional `capabilities` object controls which actions are enabled:
`selectTask`, `newTask`, `setModel`, `setEffort`, and `setFast`. Missing
capabilities are treated as unsupported for backward compatibility. The
screen-status ring preference is stored in NVS namespace `codex`, key
`status_ring`. It animates for working, waiting, or recording state, remains static
for errors, and is stopped when the Codex view is deleted.

## 菜单布局

菜单展示由 `codex_menu_ui.{h,cc}` 负责，状态和操作回调保留在
`codex_view.cc`。布局参照 `design/agent/index.html` 与 `styles.css`：

- 右侧抽屉宽 540 像素，顶部避开 62 像素系统状态栏。
- 抽屉底部固定 84 像素的任务、模型、连接标签；上方内容单独纵向滚动。
- 任务页顶部为新建任务，下方单列显示六个任务，每个按钮独占一行并可上下滚动。任务标题使用公共中号粗体（28 像素），长标题单行省略；任务序号和状态在下一行左右排列。
- 模型页显示当前任务、模型选择、推理强度与 Fast 开关。推理强度使用公共滑动条，支持点击轨道和拖动；右侧只显示当前档位。轨道高 24 像素，触摸区域高 76 像素，滑块两端保留空间。
- 连接页首先显示 PC 桥接与 Codex 的连接状态，随后为网络方式、认证与连接按钮；继续上滑可以到达状态光圈和任务提醒开关。
- 点击抽屉左侧遮罩关闭菜单。主题颜色、开关、选择控件、设置行、输入框与按钮由公共组件提供。

抽屉标签、任务卡片、下拉输入、离散档位滑动条和细线图标放在 `ui_components`，
业务视图只组装布局并绑定数据。未同步的数据继续显示“待同步”，不会以演示值代替。

推理强度选项取当前模型支持档位与“轻、中、高、极高、Ultra”的交集。点击轨道时在按下阶段计算档位，
避免 LVGL 松手时的选档被公共坐标保护撤销；仍保留松手坐标异常回到 `(0, 0)` 的保护。
拖动期间只更新本地预览，松手后提交一次最终档位；相同档位不发请求。
后台状态回传只缓存确认值，触摸期间不重设范围、位置、标签和禁用状态。
任务、模型、选项或连接可用性变化会取消本次手势，松手后应用新状态。
请求期间滑块停留在松手位置并显示“同步中”，确认或失败后回填电脑实际值。
模型下拉菜单在正常后台状态刷新时保持打开，选项变化或控件不可用时关闭。

## 任务消息与状态光圈

PC 桥接通过 `codex_state` 提供 `streamId` 与当前任务的 `conversation`，包含 `hostId`、`threadId`、`ready` 和最近 3 条消息。固件校验任务身份和快照版本；切换时替换消息列表、隐藏前一任务审批弹窗，后台任务不会写入当前对话。重新进入页面发送 `codex_sync` 获取已有历史。

`codex_status_ring` 使用共享主题颜色，在菜单和审批面板之上呈现 10 像素粗的纯色滚动线条。720 × 720 屏幕上的运动范围为 `(4, 66)` 至 `(715, 715)`，整体位于 62 像素顶部状态栏下方。开关仍保存到 NVS；动画计时器由组件持有并随销毁清理，松开语音按钮不影响光环。运行/等待/录音时滚动，错误时显示同样粗细的静态边框，空闲或断线时隐藏。

## 菜单刷新与模型选择

模型菜单只显示 GPT-6 和 GPT-5.6 系列的可见模型，当前为 Astra、Sol、Terra、Luna。
档位映射为 low=轻、medium=中、high=高、xhigh=极高、ultra=Ultra。
Luna 当前不支持 ultra，因此显示四档。当前模型或档位不在设备菜单中时仍可选择允许的项，
不把电脑实际值伪装成另一项。Micro 相对调档根据每步确认值前进，兼容电脑端显示或隐藏 max 的导航序列。

触摸读取保留无触点报告前的最后坐标，确保 LVGL 在松手时能准确选择下拉菜单项。
UI 读取快照不等待互斥锁；短暂争用期间保留上一帧触摸状态，避免伪造松手。

菜单只刷新当前标签，且只在实际显示数据、能力或操作状态发生变化时更新控件。
后台消息和快照版本变化不触发菜单重绘；重复点击当前标签不重排或重置滚动位置。
菜单打开时暂缓同一任务的 Markdown 消息重排，关闭时恢复最新三条消息；任务切换仍立即替换对话。
PC 元数据轮询先比较文件属性，文件未变化时不再反复读取全文。
状态光圈的运动段为 480 像素，使用纯色和 10 像素粗细。
