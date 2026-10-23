#!/usr/bin/env python3
"""DeskScreen 自建升级服务器：定时从 GitHub 拉最新 Release 的固件，存到本地并提供下载。

放在 NAS 上用 Docker 跑（见同目录的 README.md）。它做两件事：
  1. 每隔 INTERVAL 秒检查 GitHub 最新 Release 的 manifest.json，有新版本就下载各板子的固件，
     校验 MD5 和大小后存到 DATA/vX.Y.Z/，再写一份地址改成相对路径的 DATA/manifest.json
  2. 在 PORT 端口用 HTTP 提供 DATA 目录里的文件，交给 NAS 的反向代理加上 HTTPS

板子的设置页「自建升级服务器」填 https://你的域名/manifest.json 即可。
只用 Python 标准库。环境变量：
  REPO      GitHub 仓库，默认 zjxlink/deskscreen
  INTERVAL  检查间隔（秒），默认 3600
  PORT      HTTP 端口，默认 18266
  DATA      存放目录，默认 /data
  KEEP      保留最近几个版本，默认 3
  GH_PREFIX 下载 GitHub 文件时加在地址前面的加速前缀（可选，如 https://ghproxy.example.com/），默认不加
"""
import hashlib
import http.server
import json
import os
import re
import shutil
import sys
import threading
import time
import urllib.request

REPO = os.environ.get("REPO", "zjxlink/deskscreen")
INTERVAL = int(os.environ.get("INTERVAL", "3600"))
PORT = int(os.environ.get("PORT", "18266"))
DATA = os.environ.get("DATA", "/data")
KEEP = int(os.environ.get("KEEP", "3"))
GH_PREFIX = os.environ.get("GH_PREFIX", "")

VERSION_RE = re.compile(r"v\d+\.\d+\.\d+")


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def fetch(url, timeout=60):
    req = urllib.request.Request(GH_PREFIX + url, headers={"User-Agent": "deskscreen-ota-mirror"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def current_version():
    try:
        with open(os.path.join(DATA, "manifest.json")) as f:
            return json.load(f).get("version")
    except (OSError, ValueError):
        return None


def sync():
    upstream = json.loads(fetch(f"https://github.com/{REPO}/releases/latest/download/manifest.json"))
    version = upstream.get("version", "")
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"版本号不对：{version!r}")
    if version == current_version():
        return False

    log(f"发现新版本 {version}，开始下载")
    tmp = os.path.join(DATA, f".{version}.tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    boards = {}
    for board, info in upstream["boards"].items():
        name = os.path.basename(info["url"])
        data = fetch(info["url"], timeout=300)
        if len(data) != info["size"] or hashlib.md5(data).hexdigest() != info["md5"]:
            raise ValueError(f"{name} 校验失败（大小 {len(data)}，期望 {info['size']}）")
        with open(os.path.join(tmp, name), "wb") as f:
            f.write(data)
        # 相对 manifest.json 的地址，板子会自己拼成完整地址，换域名也不用改
        boards[board] = {"url": f"{version}/{name}", "md5": info["md5"], "size": info["size"]}
        log(f"  {board}: {name} {len(data)} 字节")

    final = os.path.join(DATA, version)
    shutil.rmtree(final, ignore_errors=True)
    os.rename(tmp, final)
    # 固件都到位后才换 manifest，板子不会看到下了一半的版本
    manifest_tmp = os.path.join(DATA, ".manifest.json.tmp")
    with open(manifest_tmp, "w") as f:
        json.dump({"version": version, "boards": boards}, f, indent=2)
    os.replace(manifest_tmp, os.path.join(DATA, "manifest.json"))
    log(f"已更新到 {version}")
    prune(version)
    return True


def version_key(v):
    return tuple(int(x) for x in v[1:].split("."))


def prune(latest):
    versions = sorted((d for d in os.listdir(DATA) if VERSION_RE.fullmatch(d)), key=version_key)
    for old in versions[:-KEEP]:
        if old != latest:
            shutil.rmtree(os.path.join(DATA, old), ignore_errors=True)
            log(f"删除旧版本 {old}")


def sync_loop():
    while True:
        try:
            if not sync():
                log(f"已是最新版本 {current_version()}")
        except Exception as e:  # 网络不好时下次再试，服务照常提供已有的文件
            log(f"同步失败：{e}")
        time.sleep(INTERVAL)


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DATA, **kwargs)

    def end_headers(self):
        if self.path.split("?")[0].endswith("manifest.json"):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404)  # 不列目录

    def log_message(self, fmt, *args):
        log(f"{self.address_string()} {fmt % args}")


def main():
    os.makedirs(DATA, exist_ok=True)
    if len(sys.argv) > 1 and sys.argv[1] == "--once":
        sync()
        return
    threading.Thread(target=sync_loop, daemon=True).start()
    log(f"提供 {DATA}，端口 {PORT}，仓库 {REPO}")
    http.server.ThreadingHTTPServer(("", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
