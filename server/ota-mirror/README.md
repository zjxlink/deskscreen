# 自建升级服务器（ota-mirror）

板子默认从 GitHub Release 在线升级。有的网络连不上 GitHub 的下载地址，这时可以在自己的服务器（比如家里的 NAS）上跑这个小服务，让板子先从这里升级，连不上再用 GitHub。

它做两件事：

1. 每小时检查一次 GitHub 最新 Release，有新版本就下载各板子的固件，校验 MD5 后存到 `data/vX.Y.Z/`，再写一份 `data/manifest.json`（里面的固件地址是相对路径，换域名不用改）
2. 在 18266 端口用 HTTP 提供这些文件。HTTPS 交给 NAS 的反向代理

服务只往外读 GitHub，不需要对外开放上传口，也不需要 GitHub 的任何密码。新版本发布后最多一小时同步到 NAS，板子每天检查一次。

## 部署

飞牛 fnOS 的详细步骤见 [docs/ota-server-fnos.md](../../docs/ota-server-fnos.md)。

其他 NAS（群晖、威联通、Unraid 等）一样：把这两个文件放进一个文件夹，执行 `docker compose up -d`。不想用 Docker 的话，直接 `DATA=./data python3 mirror.py` 也能跑（需要 Python 3.8 以上）。

## 设置项（环境变量）

| 名字 | 默认 | 说明 |
| --- | --- | --- |
| `REPO` | `zjxlink/deskscreen` | 从哪个仓库拉固件 |
| `INTERVAL` | `3600` | 检查间隔（秒） |
| `PORT` | `18266` | HTTP 端口 |
| `KEEP` | `3` | 保留最近几个版本 |
| `GH_PREFIX` | 空 | 家里连 GitHub 也慢时，填一个下载加速前缀，会加在 GitHub 地址前面 |

## 注意

- 板子校验 HTTPS 用的是固件里自带的根证书（`firmware/src/root_ca.h`），包含 Let's Encrypt（ISRG）、Sectigo/USERTrust、DigiCert 等常见的几家。NAS 用 Let's Encrypt 证书（群晖、威联通自带申请的就是）没有问题；其他证书连不上时，诊断里会显示 TLS 错误
- 局域网里也可以直接填 `http://NAS的IP:18266/manifest.json`，不走 HTTPS。固件下载后都会校验 MD5，下坏了不会装
- 自建服务器连不上、或者文件不对时，板子会自动改用 GitHub
