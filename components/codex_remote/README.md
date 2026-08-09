# Codex Remote transport

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
