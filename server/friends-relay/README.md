# 好友中转服务（friends-relay）

几台桌面屏组成「圈子」，互相戳一戳、留言、发照片、看谁在桌前。板子都在各自家里的路由器后面，互相连不上，所以消息经过这个小服务转发。手机打开它的网页也算一台「设备」，没有第二台板子时可以用手机测试。

- 只用 Python 标准库，一个文件 `relay.py`，加上手机网页 `static/index.html`
- 数据存在 `data/state.json`（设备、圈子、收件箱）和 `data/photos/`，消息和照片默认保留 7 天
- 在 18267 端口用 HTTP 提供服务，HTTPS 交给 NAS 的反向代理
- 不需要账号密码：每台设备第一次连上时自动注册，拿到一个 token；加圈子靠 6 位邀请码（10 分钟内有效，只能用一次）

## 部署

飞牛 fnOS 的详细步骤见 [docs/friends-server-fnos.md](../../docs/friends-server-fnos.md)。

其他 NAS 一样：把 `relay.py`、`docker-compose.yml` 和 `static` 文件夹放进同一个文件夹，执行 `docker compose up -d`。不用 Docker 的话，`DATA=./data python3 relay.py` 也能跑（Python 3.8 以上）。

## 设置项（环境变量）

| 名字 | 默认 | 说明 |
| --- | --- | --- |
| `PORT` | `18267` | HTTP 端口 |
| `DATA` | `/data` | 数据目录 |
| `TTL_DAYS` | `7` | 消息和照片保留几天 |

## 限制

- 一台设备最多加入 4 个圈子，一个圈子最多 8 个成员
- 留言最多 60 个字，名字最多 10 个字，照片最大 400 KB（手机网页会先缩到 320×240）
- 每台设备每分钟最多发 30 条
- 超过 90 秒没同步算离线，离线的人不显示「在桌前」

## 接口

所有接口都是 JSON，除了注册以外都要带 `Authorization: Bearer <token>`。

| 接口 | 说明 |
| --- | --- |
| `POST /api/register` `{name, kind}` | 注册，返回 `{id, token}`；`kind` 是 `board` 或 `phone` |
| `POST /api/rename` `{name}` | 改名字 |
| `POST /api/circles` `{name}` | 建一个圈子 |
| `POST /api/invite` `{circle}` | 生成邀请码 |
| `POST /api/join` `{code}` | 用邀请码加入 |
| `POST /api/leave` `{circle, member?}` | 退出圈子；圈主带 `member` 是把这个人移出去 |
| `POST /api/send` `{circle, to?, kind, text?, photo?}` | `kind` 是 `poke` 或 `text`；不带 `to` 发给圈子里所有人；`photo` 是 base64 的 JPEG |
| `POST /api/read` `{ids}` | 标记已读，发的人会收到已读回执 |
| `GET /api/sync?since=&wait=&here=` | 同步圈子、成员（在线、在桌前）和 `since` 之后的新消息。`wait` 最多 25 秒长轮询；`here=1/0` 报告自己在不在桌前 |
| `GET /p/<id>.jpg` | 照片 |
| `GET /health` | 健康检查 |
