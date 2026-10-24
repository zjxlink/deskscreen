# 在飞牛 fnOS 上部署自建升级服务器

板子默认从 GitHub 在线升级，有的网络（比如公司网络）连不上 GitHub 的下载地址。照这份文档在家里的飞牛 NAS 上跑一个小服务，板子就会先从 `https://ota.example.com` 升级，连不上再用 GitHub。文档里的 `ota.example.com` 都换成你自己的域名。

整体是这样：

```
GitHub Release ──(每小时拉一次)──> 飞牛 NAS 上的 deskscreen-ota 容器（端口 18266，HTTP）
                                          │
                              反向代理 https://ota.example.com
                                          │
                                        板子
```

容器只往外读 GitHub，不需要对外开放上传口，也不需要 GitHub 的任何密码。下面的操作在任意一台和 NAS 同一局域网的电脑上，用浏览器就能做完。

## 准备

- 飞牛 fnOS，已经装好「Docker」应用（应用中心里搜 Docker）
- 你现有的反向代理和 `ota.example.com` 的 HTTPS 证书
- 需要的两个文件，在仓库的 [`server/ota-mirror`](../server/ota-mirror) 文件夹里：
  - [`mirror.py`](../server/ota-mirror/mirror.py)：同步和提供下载的程序
  - [`docker-compose.yml`](../server/ota-mirror/docker-compose.yml)：容器配置

在 GitHub 上打开这两个文件，点右上角的下载按钮（Download raw file）存到电脑上。

## 第 1 步：把文件放到 NAS 上

1. 浏览器打开飞牛的管理页面（一般是 `http://NAS的IP:5666`），进入「文件管理」
2. 新建文件夹，比如 `docker/deskscreen-ota`
3. 把 `mirror.py` 和 `docker-compose.yml` 上传到这个文件夹里

端口默认是 **18266**。想换别的端口，上传前用记事本打开 `docker-compose.yml`，把 `"18266:18266"` 左边的数字改掉就行（右边和 `PORT` 不用动），后面反向代理也用改后的端口。

## 第 2 步：创建容器

1. 打开「Docker」应用，进入「Compose」，点「新增项目」
2. 项目名称填 `deskscreen-ota`，路径选刚才的 `docker/deskscreen-ota` 文件夹
3. 它会读到文件夹里的 `docker-compose.yml`，勾选「创建项目后立即启动」，确定
4. 第一次会下载 `python:3.12-alpine` 镜像（约 20 MB）。如果一直下载不动，在 Docker 的「设置 › 镜像仓库」里加一个国内的镜像加速地址，再重新启动项目

启动后在「容器」里找到 `deskscreen-ota`，看日志，应该有类似这样的几行：

```
提供 /data，端口 18266，仓库 zjxlink/deskscreen
发现新版本 v0.15.3，开始下载
  box3: firmware-box3.bin 2720896 字节
  desk_v1: firmware-desk_v1.bin 2571200 字节
已更新到 v0.15.3
```

文件夹里会多出 `data/manifest.json` 和 `data/v0.15.3/`。之后每小时检查一次，有新版本会自动下载，旧版本只保留最近 3 个。

如果日志里是「同步失败：…」，多半是 NAS 连 GitHub 慢。容器会一小时后自动重试；一直不行的话，在 `docker-compose.yml` 里把 `GH_PREFIX` 那行前面的 `#` 去掉，填一个 GitHub 下载加速地址，然后在 Compose 里重新构建/启动项目。

## 第 3 步：局域网里先试一下

在电脑浏览器打开 `http://NAS的IP:18266/manifest.json`，能看到一段 JSON，里面有 `"version": "v0.15.3"` 之类，就说明容器正常。打开 `http://NAS的IP:18266/` 是 404，这是故意的（不列出文件）。

打不开的话，检查飞牛的防火墙有没有放行 18266 端口。

## 第 4 步：反向代理

在你现有的反向代理里加一条：

| 项目 | 填什么 |
| --- | --- |
| 域名 | `ota.example.com` |
| 协议 | HTTPS（用你的证书） |
| 转发到 | `http://NAS的局域网IP:18266`（反向代理就跑在这台 NAS 上时也可以填 `http://127.0.0.1:18266`） |

不需要 WebSocket，也不需要特别的请求头。固件约 3 MB，如果反向代理限制了响应大小，放宽到 10 MB 以上。

然后在浏览器打开 `https://ota.example.com/manifest.json`，能看到同样的 JSON 就好了。最好用手机流量（不连家里 WiFi）也试一次，确认外网能访问。

### 证书要注意

板子只认固件里自带的几家根证书：**Let's Encrypt（ISRG Root X1/X2）、Sectigo/USERTrust、DigiCert Global Root G2/G3**。在浏览器地址栏点锁形图标查看证书，最上面一级（根证书）是这几个之一就没问题。飞牛、Lucky、Nginx Proxy Manager 自动申请的 Let's Encrypt 证书都可以。

如果根证书是别的（比如 `DigiCert Global Root CA`、`GlobalSign`），板子会连不上自建服务器，退回用 GitHub。把根证书名字告诉我，加进固件就行。

## 第 5 步：板子上设置

板子装上支持自建服务器的固件后：

1. 打开设置网页 `http://deskscreen.local`，进入「系统 › 固件升级」
2. 「自建升级服务器」填 `https://ota.example.com/manifest.json`，点底部的「保存」
3. 点「检查更新」，状态里出现「来自自建服务器」就成功了

自建服务器连不上、证书不认或者文件不对时，板子会自动改用 GitHub，状态里会同时写出两边失败的原因。

## 以后

- 发布新版本时什么都不用做：容器一小时内会自动同步，板子每天检查一次，凌晨 3～5 点自动安装
- 想立刻同步：在 Docker 里重启 `deskscreen-ota` 容器，启动时会马上检查一次
- 更新同步程序：用新的 `mirror.py` 替换 NAS 文件夹里的旧文件，再重启容器
