# Codex Remote transport

## 手动连接地址

公网模式下，服务器地址必须显式填写端口，端口范围为 `1–65535`。
未填写协议时使用 `ws://`；完整 URL 支持 `ws://` 和 `wss://`，并保留路径和查询参数。
所有格式均须填写端口，包括使用标准端口的 WSS 地址。

| 输入示例 | 实际连接地址 |
| --- | --- |
| `192.168.50.141:8765` | `ws://192.168.50.141:8765` |
| `example.com:9000` | `ws://example.com:9000` |
| `ws://example.com:9000/path` | `ws://example.com:9000/path` |
| `wss://example.com:443/path` | `wss://example.com:443/path` |

地址首尾空白会被移除。缺少端口、端口无效或地址格式不合法时，不发起连接，已有连接保持不变。
在公网模式点击连接时，通过格式校验且认证 Token 保存成功的完整地址会写入 NVS；
重新打开页面或重启设备后会自动回填。即使本次网络连接失败，地址也会保留。
WSS 使用系统证书包验证服务器证书。重连复用完整地址。
局域网自动发现使用服务返回的端口；切换到公网模式会取消局域网发现。

## Transport implementation

This ESP-IDF component owns the network transport used by the Boat Codex App.
The UI lives in `main/display/boat/apps/codex`; this component contains no
screen or LVGL code.

The device discovers the PC bridge over UDP port `8766`, obtains the advertised
WebSocket port, and connects with the response packet's source address. A saved
Bearer token is attached to the WebSocket handshake. Discovery continues while
no token is configured, but a WebSocket connection is not opened until the
token has been saved.

Discovery protocol version 1 uses these datagrams:

```json
{ "type": "codex-remote-discovery", "protocolVersion": 1 }
{ "type": "codex-remote-discovery-response", "protocolVersion": 1, "name": "PC-NAME", "wsPort": 8765 }
```

Callbacks run outside the LVGL thread. The Boat Codex App posts UI work through
`UiDispatcher`, keeping the WebSocket task non-blocking.
