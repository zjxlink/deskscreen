# 在飞牛 fnOS 上部署好友中转服务

「好友圈子」让几台桌面屏互相戳一戳、留言、看谁在桌前。板子在各自家里，互相连不上，所以要在一台大家都能访问的机器上跑一个转发消息的小服务。照这份文档把它放到家里的飞牛 NAS 上，再用反向代理开一个 HTTPS 域名（下面用 `friends.zjxlink.cn` 举例）。

```
板子 A ─┐                                             ┌─ 板子 B（别人家）
        ├── https://friends.zjxlink.cn ── 反向代理 ── NAS 上的 deskscreen-friends 容器（端口 18267）
手机网页 ┘                                             └─ 手机网页
```

手机浏览器打开同一个地址就是一台「设备」，只有一台板子时用手机当另一台来测试。

## 准备

- 飞牛 fnOS，已经装好「Docker」应用
- 你现有的反向代理和 `friends.zjxlink.cn` 的 HTTPS 证书（Let's Encrypt 就行，见下面「证书要注意」）
- 仓库 [`server/friends-relay`](../server/friends-relay) 文件夹里的三样东西：
  - [`relay.py`](../server/friends-relay/relay.py)：转发服务
  - [`docker-compose.yml`](../server/friends-relay/docker-compose.yml)：容器配置
  - [`static/index.html`](../server/friends-relay/static/index.html)：手机网页

在 GitHub 上打开这几个文件，点右上角的下载按钮（Download raw file）存到电脑上。

## 第 1 步：把文件放到 NAS 上

1. 浏览器打开飞牛的管理页面，进入「文件管理」
2. 新建文件夹 `docker/deskscreen-friends`，在里面再建一个 `static` 文件夹
3. 把 `relay.py` 和 `docker-compose.yml` 上传到 `docker/deskscreen-friends`，把 `index.html` 上传到 `docker/deskscreen-friends/static`

端口默认是 **18267**（升级服务器用的是 18266，不冲突）。要换端口就改 `docker-compose.yml` 里 `"18267:18267"` 左边的数字。

## 第 2 步：创建容器

1. 打开「Docker」应用，进入「Compose」，点「新增项目」
2. 项目名称填 `deskscreen-friends`，路径选 `docker/deskscreen-friends`
3. 勾选「创建项目后立即启动」，确定。镜像和升级服务器一样是 `python:3.12-alpine`，已经下载过就不用再等

在「容器」里看 `deskscreen-friends` 的日志，应该有一行：

```
好友中转服务启动，端口 18267，数据在 /data
```

文件夹里会多一个 `data` 文件夹，设备和消息存在里面。重启、升级容器都不会丢。

## 第 3 步：局域网里先试一下

电脑浏览器打开 `http://NAS的IP:18267/health`，看到 `{"ok": true, "seq": 0}` 就说明容器正常。打开 `http://NAS的IP:18267/` 能看到「好友圈子」手机网页。

## 第 4 步：反向代理

在反向代理里加一条：

| 项目 | 填什么 |
| --- | --- |
| 域名 | `friends.zjxlink.cn` |
| 协议 | HTTPS（用你的证书） |
| 转发到 | `http://NAS的局域网IP:18267`（反向代理在这台 NAS 上时也可以填 `http://127.0.0.1:18267`） |

还要注意两项（多数反向代理默认就满足，Nginx Proxy Manager 也是）：

- **读取超时 30 秒以上**：手机网页用长轮询，一个请求会等最多 25 秒
- **请求体 1 MB 以上**：手机发照片时一次上传几十 KB 到几百 KB

不需要 WebSocket。

然后用手机流量（不连家里 WiFi）打开 `https://friends.zjxlink.cn/`，能看到网页就好了。

### 证书要注意

板子只认固件里自带的根证书：**Let's Encrypt（ISRG Root X1/X2）、Sectigo/USERTrust、DigiCert Global Root G2/G3**。升级服务器 `ota.zjxlink.cn` 能用，这里用同样的证书就没问题。

## 第 5 步：板子上设置

1. 打开设置网页 `http://deskscreen.local`，进入「服务 › 好友圈子」
2. 「中转服务地址」填 `https://friends.zjxlink.cn`，「我的名字」填别人在圈子里看到的名字（最多 10 个字），想让别人看到你在不在桌前就打开「告诉圈子我在桌前」，点底部「保存」
3. 板子会自动注册，翻到「好友」页（小橘页后面）。概览里「好友圈子」显示「已连接」就成功了
4. 点「管理圈子」打开板子的管理网页（链接里带着板子的身份，别发给别人），在里面建一个圈子，比如「我们家」

## 第 6 步：用手机当第二台设备测试

1. 手机浏览器打开 `https://friends.zjxlink.cn/`，填一个名字（比如「小雨」）注册
2. 在板子的「好友」页点「邀请」，屏幕上出现 6 位邀请码；在手机网页「加入圈子」里输入
3. 手机上点板子那一行的「戳」：板子响一声，全屏显示「小雨 戳了你一下」，10 秒后自己关掉
4. 手机上写一句话或者选一张照片发送：板子全屏显示留言（有照片会显示缩略图），点一下关掉，手机上那条变成「已读」
5. 手机上打开「我在桌前」：板子好友页和时钟页城市那一行显示「小雨在桌前」
6. 板子好友页点某个人戳 TA，点「发留言 ›」选一句常用的话发出去，手机上马上能看到

想模拟更多台设备：手机网页底部点「再开一个测试身份」，新开的页面是另一个人，同一个浏览器能开好几个。

## 以后

- 别人家的板子：同样在「服务 › 好友圈子」里填这个地址，然后用你板子上（或手机网页里）生成的邀请码加入
- 更新服务：用新的 `relay.py` 或 `static/index.html` 替换 NAS 文件夹里的旧文件，再重启容器，数据不会丢
- 想清空所有数据：停掉容器，删掉 `data` 文件夹，再启动。所有板子会自动重新注册，但要重新建圈子、邀请
