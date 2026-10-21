#!/usr/bin/env python3
"""DeskScreen 好友中转服务：几台桌面屏（和手机网页）组成「圈子」，互相戳一戳、留言、看谁在桌前。

两台板子在不同的家里，都在路由器后面，互相连不上，所以消息都经过这个服务转发。
放在 NAS 上用 Docker 跑（见同目录的 README.md），由 NAS 的反向代理加上 HTTPS。

设备：一台板子或一个手机网页就是一个设备，注册时拿到一个 token，以后请求都带
  Authorization: Bearer <token>
圈子：一个设备可以加入几个圈子；圈子里任何成员都能生成一次性邀请码（6 位数字，10 分钟有效）。
消息：发到整个圈子或圈子里的某一个人。每个设备有自己的收件箱（最近 200 条），用递增的序号同步：
  板子每隔几秒 GET /api/sync?since=序号，手机网页用 wait=25 长轮询，有新消息马上返回。
在桌前：同步时带 here=1/0（板子用雷达判断），超过 90 秒没同步算离线，离线就不显示在桌前。

只用 Python 标准库。环境变量：
  PORT     HTTP 端口，默认 18267
  DATA     存放目录，默认 /data（state.json 和 photos/）
  TTL_DAYS 消息和照片保留几天，默认 7
"""
import base64
import hashlib
import http.server
import json
import os
import re
import secrets
import sys
import threading
import time
import urllib.parse

PORT = int(os.environ.get("PORT", "18267"))
DATA = os.environ.get("DATA", "/data")
TTL_DAYS = float(os.environ.get("TTL_DAYS", "7"))
STATIC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

NAME_MAX = 10          # 名字最多几个字
TEXT_MAX = 60          # 留言最多几个字，和板子的留言板一样
PHOTO_MAX = 400 * 1024  # 手机网页会先缩到 320x240 再传，正常只有一二十 KB
INBOX_MAX = 200
ONLINE_S = 90          # 多久没同步算离线
INVITE_S = 600
MAX_CIRCLES = 4        # 一个设备最多加入几个圈子（板子上只留了这么多）
MAX_MEMBERS = 8
SEND_PER_MIN = 30
WAIT_MAX = 25

lock = threading.Condition()
state = {"seq": 0, "devices": {}, "circles": {}}
invites = {}  # code -> {circle, by, exp}；重启就失效，不用存
pver = 0      # 有人的在桌前状态变了就加一，叫醒长轮询
last_save = 0.0
dirty = False


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def new_id(n=8):
    # 小写字母和数字，去掉容易看错的
    alphabet = "abcdefghjkmnpqrstuvwxyz23456789"
    return "".join(secrets.choice(alphabet) for _ in range(n))


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def clip(text, n):
    """去掉首尾空白和控制字符，按字符数截断"""
    text = re.sub(r"[\x00-\x1f\x7f]", " ", str(text or "")).strip()
    return text[:n]


def load():
    global state
    path = os.path.join(DATA, "state.json")
    try:
        with open(path, encoding="utf-8") as f:
            state = json.load(f)
        log(f"已读取 {len(state['devices'])} 个设备、{len(state['circles'])} 个圈子")
    except FileNotFoundError:
        pass
    os.makedirs(os.path.join(DATA, "photos"), exist_ok=True)


def save(force=False):
    """调用时要拿着 lock。最多每 5 秒写一次盘，NAS 的硬盘少折腾"""
    global last_save, dirty
    dirty = True
    if not force and time.time() - last_save < 5:
        return
    path = os.path.join(DATA, "state.json")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, ensure_ascii=False)
    os.replace(tmp, path)
    last_save = time.time()
    dirty = False


def housekeeping():
    """后台：把没写盘的改动写下去，删掉过期的消息、照片和邀请码"""
    while True:
        time.sleep(5)
        with lock:
            if dirty:
                save(force=True)
            now = time.time()
            for code in [c for c, v in invites.items() if v["exp"] < now]:
                del invites[code]
        if int(time.time()) % 3600 < 5:
            cleanup()


def cleanup():
    cutoff = time.time() - TTL_DAYS * 86400
    with lock:
        for d in state["devices"].values():
            d["inbox"] = [e for e in d["inbox"] if e["ts"] >= cutoff]
        save()
    pdir = os.path.join(DATA, "photos")
    for name in os.listdir(pdir):
        p = os.path.join(pdir, name)
        try:
            if os.path.getmtime(p) < cutoff:
                os.remove(p)
        except OSError:
            pass


# ---------- 数据操作（都要拿着 lock） ----------

def bump():
    """成员或在桌前状态变了：叫醒所有长轮询（调用时要拿着 lock）"""
    global pver
    pver += 1
    lock.notify_all()


def push(dev_id, event):
    """往一个设备的收件箱里放一条，返回这条的序号"""
    d = state["devices"].get(dev_id)
    if not d:
        return
    state["seq"] += 1
    e = dict(event, seq=state["seq"])
    d["inbox"].append(e)
    del d["inbox"][:-INBOX_MAX]
    lock.notify_all()


def is_online(d, now):
    return now - d.get("seen", 0) < ONLINE_S


def circle_view(c, me, now):
    members = []
    for mid in c["members"]:
        d = state["devices"].get(mid)
        if not d:
            continue
        online = is_online(d, now)
        members.append({
            "id": mid, "name": d["name"], "kind": d["kind"],
            "online": online, "here": online and d.get("here", False), "me": mid == me,
        })
    return {"id": c["id"], "name": c["name"], "owner": c["owner"], "members": members}


def my_circles(dev_id):
    return [c for c in state["circles"].values() if dev_id in c["members"]]


def notify_circle(c, actor, what):
    """成员变了：告诉圈子里的其他人（板子据此刷新成员列表）"""
    name = state["devices"][actor]["name"] if actor in state["devices"] else ""
    for mid in c["members"]:
        if mid != actor:
            push(mid, {"type": "circle", "circle": c["id"], "circle_name": c["name"],
                       "from": actor, "from_name": name, "text": what, "ts": int(time.time())})


class ApiError(Exception):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status = status
        self.msg = msg


def find_device(token):
    if not token:
        raise ApiError(401, "缺少 token")
    h = token_hash(token)
    for dev_id, d in state["devices"].items():
        if d["token"] == h:
            return dev_id, d
    raise ApiError(401, "token 无效，设备可能被删掉了，请重新注册")


def get_circle(dev_id, cid):
    c = state["circles"].get(cid)
    if not c or dev_id not in c["members"]:
        raise ApiError(404, "不在这个圈子里")
    return c


def api_register(body, _dev):
    name = clip(body.get("name"), NAME_MAX) or "桌面屏"
    kind = "board" if body.get("kind") == "board" else "web"
    token = secrets.token_urlsafe(24)
    dev_id = new_id()
    with lock:
        state["devices"][dev_id] = {"name": name, "kind": kind, "token": token_hash(token),
                                    "created": int(time.time()), "seen": 0, "here": False, "inbox": [],
                                    "sends": []}
        save()
    log(f"注册 {kind} {dev_id} {name}")
    return {"id": dev_id, "token": token, "name": name, "seq": state["seq"]}


def api_rename(body, dev):
    dev_id, d = dev
    name = clip(body.get("name"), NAME_MAX)
    if not name:
        raise ApiError(400, "名字不能为空")
    with lock:
        d["name"] = name
        bump()
        save()
    return {"ok": True, "name": name}


def api_create(body, dev):
    dev_id, _ = dev
    name = clip(body.get("name"), NAME_MAX) or "我们家"
    with lock:
        if len(my_circles(dev_id)) >= MAX_CIRCLES:
            raise ApiError(400, f"最多加入 {MAX_CIRCLES} 个圈子")
        cid = new_id(6)
        state["circles"][cid] = {"id": cid, "name": name, "owner": dev_id, "members": [dev_id],
                                 "created": int(time.time())}
        save()
        return {"circle": circle_view(state["circles"][cid], dev_id, time.time())}


def api_invite(body, dev):
    dev_id, _ = dev
    with lock:
        c = get_circle(dev_id, body.get("circle"))
        if len(c["members"]) >= MAX_MEMBERS:
            raise ApiError(400, f"圈子最多 {MAX_MEMBERS} 个成员")
        code = None
        while not code or code in invites:
            code = f"{secrets.randbelow(1000000):06d}"
        invites[code] = {"circle": c["id"], "by": dev_id, "exp": time.time() + INVITE_S}
    return {"code": code, "expires_in": INVITE_S, "circle": c["name"]}


def api_join(body, dev):
    dev_id, _ = dev
    code = re.sub(r"\D", "", str(body.get("code", "")))
    with lock:
        inv = invites.get(code)
        if not inv or inv["exp"] < time.time():
            raise ApiError(400, "邀请码不对或者过期了")
        c = state["circles"].get(inv["circle"])
        if not c:
            raise ApiError(400, "这个圈子已经解散了")
        if dev_id not in c["members"]:
            if len(my_circles(dev_id)) >= MAX_CIRCLES:
                raise ApiError(400, f"最多加入 {MAX_CIRCLES} 个圈子")
            if len(c["members"]) >= MAX_MEMBERS:
                raise ApiError(400, f"圈子最多 {MAX_MEMBERS} 个成员")
            c["members"].append(dev_id)
            notify_circle(c, dev_id, "加入了圈子")
            bump()
        del invites[code]
        save()
        return {"circle": circle_view(c, dev_id, time.time())}


def api_leave(body, dev):
    dev_id, _ = dev
    with lock:
        c = get_circle(dev_id, body.get("circle"))
        target = body.get("member") or dev_id
        if target != dev_id and c["owner"] != dev_id:
            raise ApiError(403, "只有创建圈子的人能移出别人")
        if target not in c["members"]:
            raise ApiError(404, "不是这个圈子的成员")
        c["members"].remove(target)
        if target == dev_id:
            notify_circle(c, dev_id, "退出了圈子")
        else:
            push(target, {"type": "circle", "circle": c["id"], "circle_name": c["name"], "from": dev_id,
                          "from_name": state["devices"][dev_id]["name"], "text": "把你移出了圈子",
                          "ts": int(time.time())})
            notify_circle(c, target, "被移出了圈子")
        bump()
        if not c["members"]:
            del state["circles"][c["id"]]
        elif c["owner"] == target:
            c["owner"] = c["members"][0]
        save()
    return {"ok": True}


def api_send(body, dev):
    dev_id, d = dev
    kind = body.get("kind")
    if kind not in ("poke", "text"):
        raise ApiError(400, "kind 只能是 poke 或 text")
    text = clip(body.get("text"), TEXT_MAX) if kind == "text" else ""
    photo = None
    if kind == "text" and body.get("photo"):
        try:
            data = base64.b64decode(body["photo"], validate=True)
        except ValueError:
            raise ApiError(400, "照片格式不对")
        if len(data) > PHOTO_MAX or not data.startswith(b"\xff\xd8"):
            raise ApiError(400, "照片要 JPEG，最大 400KB")
        photo = new_id(16) + ".jpg"
        with open(os.path.join(DATA, "photos", photo), "wb") as f:
            f.write(data)
    if kind == "text" and not text and not photo:
        raise ApiError(400, "留言是空的")
    now = time.time()
    with lock:
        c = get_circle(dev_id, body.get("circle"))
        d["sends"] = [t for t in d.get("sends", []) if now - t < 60]
        if len(d["sends"]) >= SEND_PER_MIN:
            raise ApiError(429, "发得太快了，歇一会儿")
        d["sends"].append(now)
        to = body.get("to") or ""
        if to and to not in c["members"]:
            raise ApiError(404, "对方不在这个圈子里")
        targets = [to] if to else [m for m in c["members"] if m != dev_id]
        msg_id = new_id(10)
        event = {"type": kind, "id": msg_id, "circle": c["id"], "circle_name": c["name"], "from": dev_id,
                 "from_name": d["name"], "to": to, "text": text, "ts": int(now)}
        if photo:
            event["photo"] = "/p/" + photo
        for t in targets:
            if t != dev_id:
                push(t, event)
        # 自己的收件箱里也放一条（标记 mine），手机网页的记录、已读数靠它
        push(dev_id, dict(event, mine=True, recipients=len([t for t in targets if t != dev_id])))
        save()
    return {"id": msg_id, "recipients": len(targets)}


def api_read(body, dev):
    """收到的留言看过了：告诉发的人"""
    dev_id, d = dev
    ids = body.get("ids") or []
    if not isinstance(ids, list):
        raise ApiError(400, "ids 要是数组")
    with lock:
        for msg_id in ids[:50]:
            for e in d["inbox"]:
                if e.get("id") == msg_id and not e.get("mine") and not e.get("read"):
                    e["read"] = True
                    push(e["from"], {"type": "read", "id": msg_id, "circle": e["circle"], "from": dev_id,
                                     "from_name": d["name"], "ts": int(time.time())})
        save()
    return {"ok": True}


def api_sync(body, dev, query):
    dev_id, d = dev
    since = int(query.get("since", ["0"])[0] or 0)
    wait = min(float(query.get("wait", ["0"])[0] or 0), WAIT_MAX)
    here = query.get("here", [None])[0]
    deadline = time.time() + wait
    with lock:
        d["seen"] = time.time()
        if here in ("0", "1") and d.get("here") != (here == "1"):
            d["here"] = here == "1"
            bump()  # 别人的长轮询马上看到
        start_pver = pver
        while True:
            events = [e for e in d["inbox"] if e["seq"] > since]
            if events or pver != start_pver or time.time() >= deadline or since > state["seq"] or since == 0:
                break
            lock.wait(timeout=max(0.1, deadline - time.time()))
        now = time.time()
        d["seen"] = now
        circles = [circle_view(c, dev_id, now) for c in my_circles(dev_id)]
        # 第一次同步（since=0）只要序号，不把以前的消息再弹一遍；手机网页要看记录时带 history=1
        if since == 0 and query.get("history", ["0"])[0] != "1":
            events = []
        return {"seq": state["seq"], "me": {"id": dev_id, "name": d["name"], "kind": d["kind"]},
                "circles": circles, "events": events[-50:]}


ROUTES = {
    "/api/register": api_register,
    "/api/rename": api_rename,
    "/api/circles": api_create,
    "/api/invite": api_invite,
    "/api/join": api_join,
    "/api/leave": api_leave,
    "/api/send": api_send,
    "/api/read": api_read,
}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # 板子复用连接，不用每次都 TLS 握手
    server_version = "deskscreen-friends"

    def log_message(self, fmt, *args):
        pass

    def send_json(self, status, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.cors()
        self.end_headers()
        self.wfile.write(data)

    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

    def send_file(self, path, ctype, cache):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            return self.send_json(404, {"error": "没有这个文件"})
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.cors()
        self.end_headers()
        self.wfile.write(data)

    def token(self):
        auth = self.headers.get("Authorization", "")
        return auth[7:].strip() if auth.startswith("Bearer ") else ""

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.cors()
        self.end_headers()

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path in ("/", "/index.html"):
            return self.send_file(os.path.join(STATIC, "index.html"), "text/html; charset=utf-8", "no-cache")
        m = re.fullmatch(r"/p/([a-z0-9]{16}\.jpg)", url.path)
        if m:
            return self.send_file(os.path.join(DATA, "photos", m.group(1)), "image/jpeg", "max-age=604800")
        if url.path == "/api/sync":
            return self.run_api(lambda dev: api_sync({}, dev, urllib.parse.parse_qs(url.query)))
        if url.path == "/health":
            return self.send_json(200, {"ok": True, "seq": state["seq"]})
        self.send_json(404, {"error": "没有这个地址"})

    def do_POST(self):
        url = urllib.parse.urlsplit(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        if n > PHOTO_MAX * 2:
            self.rfile.read(n)
            return self.send_json(413, {"error": "太大了"})
        raw = self.rfile.read(n) if n else b""
        try:
            body = json.loads(raw or b"{}")
            if not isinstance(body, dict):
                raise ValueError
        except ValueError:
            return self.send_json(400, {"error": "要 JSON"})
        if url.path == "/api/sync":
            return self.run_api(lambda dev: api_sync(body, dev, urllib.parse.parse_qs(url.query)))
        fn = ROUTES.get(url.path)
        if not fn:
            return self.send_json(404, {"error": "没有这个地址"})
        if fn is api_register:
            return self.run_api(lambda _dev: api_register(body, None), auth=False)
        self.run_api(lambda dev: fn(body, dev))

    def run_api(self, fn, auth=True):
        try:
            dev = None
            if auth:
                with lock:
                    dev = find_device(self.token())
            self.send_json(200, fn(dev))
        except ApiError as e:
            self.send_json(e.status, {"error": e.msg})
        except Exception as e:  # noqa: BLE001  一个请求出错不能把服务带崩
            log(f"出错 {self.path}: {e!r}")
            self.send_json(500, {"error": "服务器出错了"})


def main():
    load()
    threading.Thread(target=housekeeping, daemon=True).start()
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    srv.daemon_threads = True
    log(f"好友中转服务启动，端口 {PORT}，数据在 {DATA}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    with lock:
        save(force=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
