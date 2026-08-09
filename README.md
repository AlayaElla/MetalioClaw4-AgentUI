# Metalio Claw4 Agent UI 固件

本仓库是 Metalio Claw4（ESP32-P4 主控）的独立 ESP-IDF 固件工程，包含板级
驱动、音频、显示、Agent UI、网络协议和本地 ESP-IDF 组件。PC bridge 不属于本仓库，
设备连接协议中的端口、消息和认证约定应与对应的 PC bridge 版本保持一致。

## 目录结构

```text
CMakeLists.txt       ESP-IDF 工程入口
sdkconfig            Metalio Claw4 的已验证配置
dependencies.lock    ESP-IDF Component Manager 锁定文件
main/                应用、板级驱动、协议、音频和 Agent UI
components/          本地组件：codex_remote、json、uart-uhci、ui_dispatcher
partitions/           分区表（打包脚本使用 partitions/v1/32m.csv）
scripts/              资源生成工具及构建/烧录/监视脚本
LICENSE               固件许可证
```

`managed_components/` 和 `build/` 是 ESP-IDF 在本机生成的目录，不属于源代码，
不会随仓库发布。大体积语音、模型、SD 卡和工厂测试资源也不在公开导出中。

## 环境要求

- ESP-IDF **v6.0.2**，目标芯片 ESP32-P4；不要用其他版本替代。
- ESP-IDF 提供的 Python 环境、CMake、Ninja 和 esptool。
- Windows PowerShell、Linux 或 macOS 均可构建；烧录和串口监视需要目标设备及其
  USB Serial/JTAG 端口。

准备好 ESP-IDF 后，在当前 shell 导出环境。Windows 示例：

```powershell
& C:\path\to\esp-idf\export.ps1
idf.py --version
```

应确认显示 6.0.2，再执行后续命令。Component Manager 会根据
`main/idf_component.yml` 和 `dependencies.lock` 下载受管组件；不要手动提交
`managed_components/`。

## 构建

直接使用 ESP-IDF：

```powershell
idf.py build
```

或使用脚本（脚本会先执行完整构建并检查 32 MB 分区表中的 14 MiB factory app）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-esp32.ps1 -IdfPath C:\path\to\esp-idf
```

打包结果位于本仓库的 `build/esp32/`：

```text
Agent-ESP32P4-full.bin       32 MiB 完整镜像（含分区表；空分区填充为 0xFF）
Agent-ESP32P4-app.bin        仅 factory app 镜像
Agent-ESP32P4-firmware.zip   上述镜像的归档
```

ESP-IDF 临时产物仍位于 `build/` 根目录。打包成功只证明本机源码和工具链通过了
脚本检查，不代表镜像已经烧录或设备已经完成真机验证。

## 安全烧录

烧录前先确认端口、芯片型号、分区表和镜像来源。建议先使用 `-DryRun` 检查目标：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash-esp32.ps1 -Port COM10 -IdfPath C:\path\to\esp-idf -DryRun
```

确认无误后，完整镜像写入 `0x0`，会覆盖分区表及数据分区：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash-esp32.ps1 -Port COM10 -IdfPath C:\path\to\esp-idf
```

如果只需更新应用并保留设备设置、NVS 等数据，使用 app-only 脚本。它会验证
factory app 位于 `0x200000` 且不超过 14 MiB：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash-esp32-preserve-settings.ps1 -Port COM10 -IdfPath C:\path\to\esp-idf -DryRun
powershell -ExecutionPolicy Bypass -File .\scripts\flash-esp32-preserve-settings.ps1 -Port COM10 -IdfPath C:\path\to\esp-idf
```

`-DryRun` 不会写入设备；去掉它才会调用 esptool。请不要把包含设备 Token、Wi-Fi
凭据、NVS 或完整 flash dump 的文件提交到仓库。

## 串口监视

监视脚本使用已经构建的 `build/agent.elf`，默认波特率 115200：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\monitor-esp32.ps1 -Port COM10 -IdfPath C:\path\to\esp-idf
```

退出 `idf.py monitor` 使用 `Ctrl+]`。串口日志和实际设备行为属于硬件验证证据，
不能用构建、打包或 dry-run 输出替代。

## 公开导出中的资源边界

公开仓库只保留可审计的源码、配置、分区表、脚本和许可证。以下本地资源被有意排除：

- `main/factory-test-assets/`：仅用于可选的 `factory_test` SPIFFS 镜像。
  CMake 在目录存在时才生成该镜像；目录缺失会给出 warning 并跳过，不影响正常 AgentUI
  固件构建。公开导出不包含其中的音频文件；若要发布工厂测试镜像，必须由拥有授权的环境
  提供资源，不能伪造占位文件。
- `managed_components/`、`build/`、`wakeword/srmodels.bin`、`sd_images/`、
  `esp_claw_bin/`、厂商二进制镜像以及语言/提示音 OGG。

缺少可选 `wakeword/srmodels.bin` 时，工程使用 Component Manager 提供的默认模型；
缺少工厂测试资源时不要误以为已经完成了完整发布构建。

公开版不包含语言/提示音 OGG。`main/assets/lang_config.h` 为缺失的提示音提供空的
`std::string_view`，统一播放入口会安全跳过空资源，因此构建和显示、网络、语音主流程
仍可验证，但相关提示音会静音。需要提示音的发布包必须在获得授权后，将资源放入对应
语言目录再构建；不要提交来源或许可不明的音频。

## 许可证与第三方组件

固件许可证见 [`LICENSE`](LICENSE)。`components/uart-uhci/idf_component.yml` 保留
其 Apache-2.0 元数据；ESP-IDF、LVGL、ESP-SR、音频编解码器及其他 Component Manager
依赖各自适用的许可证和 NOTICE，公开发布前应逐项核对其再分发条件。
