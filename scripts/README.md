# 开发命令

常用入口位于仓库根目录的 `package.json`，也可直接运行 `node scripts/run.cjs <任务>`。

| 命令 | 操作 |
| --- | --- |
| `npm run package` | 构建并打包固件 |
| `npm run build` | 构建并打包固件 |
| `npm run flash -- -Port COM13 -DryRun` | 检查完整烧录参数 |
| `npm run flash:preserve -- -Port COM13 -DryRun` | 检查保留设置的应用烧录参数 |
| `npm run monitor -- -Port COM13` | 串口监控 |
| `npm run build:apps -- calculator` | 构建指定外部 App |
| `npm run build:apps` | 构建全部已有外部 App |

烧录命令去掉 `-DryRun` 后会写入设备。固件和外部 App 构建需要 ESP-IDF；设备任务沿用现有 PowerShell 流程，非 Windows 环境需要 PowerShell 及适用的 ESP-IDF 配置。原有脚本参数通过 `--` 原样传递。

## 目录

- `hardware/`：固件构建、打包、烧录和监控的实际实现。
- `tools/`：字体、音频、语言和资源生成工具。
- `Image_Converter/`、`ogg_converter/`、`p3_tools/`、`spiffs_assets/`：已有独立转换工具。
- `../patches/`：第三方驱动和组件补丁。

根目录保留的旧 PowerShell/Python 文件是兼容入口，实际逻辑在上述分类目录。新增实现直接放入相应目录。
