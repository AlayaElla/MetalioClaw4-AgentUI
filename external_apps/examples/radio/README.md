# 收音机电台配置

收音机 1.0.0 从系统提供的 App 私有目录读取 `stations.json`：

`/sdcard/metalio/app-data/com.metalio.radio/stations.json`

首次启动时，系统会把包内 `assets/stations.json` 复制到这个位置。之后可以直接编辑该文件；保存后退出并重新进入收音机即可重新加载。升级收音机 App 不会覆盖已经存在的配置。

配置格式：

```json
{
  "default": 1,
  "stations": [
    {
      "name": "中国之声",
      "url": "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"
    }
  ]
}
```

- `default` 是从 1 开始的默认电台编号；省略时使用第一个电台。
- `stations` 必须包含 1 至 48 个电台。
- `name` 不能为空，UTF-8 长度不能超过 96 字节。
- `url` 必须以 `http://` 或 `https://` 开头，长度不能超过 1024 字节。
- 整个配置不能超过 8 KB。
- 配置无效时收音机会显示错误，不会静默回退并覆盖用户文件。
