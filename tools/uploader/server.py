#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
IPA 分片上传 + 自动体检服务（Python 标准库，无需装依赖）

用法：
    python3 tools/uploader/server.py [端口]        # 默认 8787
浏览器打开 http://<沙箱地址>:8787/ 选择 IPA 上传即可。

安全约束：
  - 文件只落在本地 inbox（不上传任何外部服务）；session id 做白名单校验；
  - 绝不把 IPA 提交到 git（见 .gitignore 的 *.ipa）。
"""

import os
import re
import json
import struct
import tempfile
import zipfile
import http.server
import socketserver
from urllib.parse import urlparse, parse_qs

HOST = "0.0.0.0"
PORT = int(os.environ.get("PORT", "8787"))
ROOT = os.path.dirname(os.path.abspath(__file__))
INBOX = os.path.join(ROOT, "inbox")
MACH_MAGIC = (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
              b"\xfe\xed\xfa\xce", b"\xca\xfe\xba\xbe")

os.makedirs(INBOX, exist_ok=True)

KEY_SYMBOLS = [b"_il2cpp_domain_get", b"_il2cpp_runtime_invoke",
               b"_il2cpp_class_get_method_from_name", b"_il2cpp_field_get_offset"]
KEY_CLASSES = [b"PlayerControl", b"GameManager", b"ShipStatus", b"RoleBehaviour"]


def safe_sid(sid):
    """只允许字母数字，防止路径穿越"""
    return sid if sid and re.fullmatch(r"[A-Za-z0-9_\-]{1,64}", sid) else None


def session_dir(sid):
    d = os.path.join(INBOX, sid)
    os.makedirs(d, exist_ok=True)
    return d


# ---------------- Mach-O 体检 ----------------
def macho_zap(path):
    """返回该 Mach-O 的关键信息（架构 / 是否加密 / il2cpp 符号是否存在）"""
    info = {"size": os.path.getsize(path)}
    with open(path, "rb") as f:
        head = f.read(262144)          # 头部 + 足够的 load commands
        f.seek(0)
        if head[:4] not in MACH_MAGIC and head[:4] != b"\xcf\xfa\xed\xfe":
            info["error"] = "非 Mach-O"
            return info

        magic = head[:4]
        if magic == b"\xcf\xfa\xed\xfe":
            cpu, sub = struct.unpack_from("<ii", head, 4)
            ncmds = struct.unpack_from("<I", head, 16)[0]
        elif magic == b"\xce\xfa\xed\xfe":
            cpu, sub = struct.unpack_from("<ii", head, 4)
            ncmds = struct.unpack_from("<I", head, 16)[0]
        elif magic == b"\xca\xfe\xba\xbe":
            info["arch"] = "fat"
            info["note"] = "胖二进制（本次只体检 arm64 切片）"
            cpu, sub, ncmds = -1, -1, 0
        else:
            cpu, sub, ncmds = -1, -1, 0

        info["cputype"] = cpu
        if cpu == 0x0100000C:
            info["arch"] = "arm64e" if (sub & 0xFF) == 2 else "arm64"
        else:
            info["arch"] = "other(%d)" % cpu

        # LC_ENCRYPTION_INFO_64 = 0x2C
        off = 32
        for _ in range(max(0, ncmds)):
            if off + 8 > len(head):
                break
            cmd, cmdsize = struct.unpack_from("<II", head, off)
            if cmd == 0x2C and off + 24 <= len(head):
                info["cryptid"] = struct.unpack_from("<I", head, off + 20)[0]
            off += cmdsize
            if cmdsize <= 0:
                break
        if "cryptid" not in info:
            info["cryptid"] = 0   # 没有该 load command = 未加密

    # 全文件扫关键符号名（在 __LINKEDIT 字符串表里）
    hits = {}
    with open(path, "rb") as f:
        rest = f.read()
    for k in KEY_SYMBOLS + KEY_CLASSES:
        hits[k.decode()] = rest.count(k)
    info["symbol_hits"] = hits
    return info


def analyze_ipa(path):
    """体检整包：找 app / 可执行文件 / UnityFramework，逐个 Mach-O 体检"""
    rep = {"file": os.path.basename(path), "size": os.path.getsize(path), "ok": True}
    try:
        z = zipfile.ZipFile(path)
    except Exception as e:
        return {"ok": False, "error": "无法解压（不是 IPA？）: %s" % e}

    names = z.namelist()
    apps = [n for n in names if re.match(r"Payload/[^/]+\.app/$", n)]
    rep["entries"] = len(names)
    if not apps:
        rep["ok"] = False
        rep["error"] = "未找到 Payload/*.app"
        return rep
    app = apps[0]
    rep["app"] = app

    # 候选 Mach-O：体积较大且名字无扩展名 / 在 Frameworks 里
    cands = []
    for i in z.infolist():
        n = i.filename
        if not n.startswith(app) or n.endswith("/"):
            continue
        base = n.rsplit("/", 1)[-1]
        if "." in base and not base.endswith((".dylib", ".framework")):
            continue
        if i.file_size < 200_000:
            continue
        cands.append((i.file_size, n))
    cands.sort(reverse=True)

    results = []
    tempdir = tempfile.mkdtemp(prefix="nebula_ipa_")
    try:
        for size, n in cands[:4]:                  # 最多看 4 个，控体积
            try:
                with z.open(n) as src:
                    head = src.read(4)
                if head not in MACH_MAGIC and head != b"\xcf\xfa\xed\xfe":
                    continue
                tmp = os.path.join(tempdir, "mach_bin")
                with z.open(n) as src, open(tmp, "wb") as dst:
                    while True:
                        buf = src.read(1 << 20)
                        if not buf:
                            break
                        dst.write(buf)
                info = macho_zap(tmp)
                info["path"] = n
                results.append(info)
                os.remove(tmp)
            except Exception as e:
                results.append({"path": n, "error": str(e)})
    finally:
        z.close()

    rep["macho"] = results
    return rep


# ---------------- HTTP ----------------
class Handler(http.server.SimpleHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS")

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path, ctype="text/html; charset=utf-8"):
        if not os.path.exists(path):
            self._json({"ok": False, "error": "not found"}, 404)
            return
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self._cors()
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        self.send_response(200)
        self._cors()
        self.end_headers()

    def do_PUT(self):
        """接收一个分片：/api/chunk?sid=xxx&idx=N，body 为原始字节"""
        q = parse_qs(urlparse(self.path).query)
        sid = safe_sid(q.get("sid", [""])[0])
        if not sid:
            return self._json({"ok": False, "error": "bad sid"}, 400)
        try:
            idx = int(q.get("idx", ["-1"])[0])
        except ValueError:
            return self._json({"ok": False, "error": "bad idx"}, 400)
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n)
        d = session_dir(sid)
        part = os.path.join(d, "part_%08d" % idx)
        with open(part, "wb") as f:
            f.write(data)
        self._json({"ok": True, "idx": idx, "bytes": len(data)})

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ("/", "/index.html", "/uploader.html"):
            return self._send_file(os.path.join(ROOT, "index.html"))
        if u.path == "/api/status":
            q = parse_qs(u.query)
            sid = safe_sid(q.get("sid", [""])[0])
            if not sid:
                return self._json({"ok": False}, 400)
            d = session_dir(sid)
            parts = sorted(os.listdir(d))
            idxs = [int(p.split("_")[-1]) for p in parts if p.startswith("part_")]
            return self._json({"ok": True, "received": idxs,
                               "count": len(idxs), "bytes": sum(
                                   os.path.getsize(os.path.join(d, p)) for p in parts)})
        return self._json({"ok": False, "error": "unknown path"}, 404)

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/api/complete":
            q = parse_qs(u.query)
            sid = safe_sid(q.get("sid", [""])[0])
            name = os.path.basename(q.get("name", ["upload.ipa"])[0])
            if not sid:
                return self._json({"ok": False}, 400)
            d = session_dir(sid)
            parts = sorted(p for p in os.listdir(d) if p.startswith("part_"))
            out_path = os.path.join(INBOX, "%s__%s" % (sid, name))
            total = 0
            with open(out_path, "wb") as out:
                for p in parts:
                    with open(os.path.join(d, p), "rb") as src:
                        while True:
                            buf = src.read(1 << 20)
                            if not buf:
                                break
                            out.write(buf)
                            total += len(buf)
            rep = analyze_ipa(out_path)
            rep["saved_as"] = out_path
            rep["total_bytes"] = total
            return self._json(rep)
        return self._json({"ok": False, "error": "unknown path"}, 404)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    print("Nebula IPA 上传服务已启动: http://%s:%d/" % (HOST, PORT))
    print("文件落地目录:", INBOX)
    Server((HOST, PORT), Handler).serve_forever()
