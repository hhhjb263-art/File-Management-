# -*- coding: utf-8 -*-
"""生成云匣 CloudVault 的 ER 图（黑白细线风格，纯 SVG，无外部依赖）。

实体 = 矩形（内含属性，PK 下划线、FK 标记），关系 = 菱形 + 基数数字。
输出：output/er/*.svg 与 output/云匣CloudVault-ER图集.html
"""
import os

INK = "#1a1a1a"
LINE = "#3a3a3a"
MUT = "#8c8c8c"
FONT = "Segoe UI,Microsoft YaHei,PingFang SC,sans-serif"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "output")


def cw(ch, size=12.0):
    o = ord(ch)
    if o >= 0x2E80:
        return size * 1.02
    if ch in "iIlj|.,:;!'":
        return size * 0.30
    if ch.isupper():
        return size * 0.66
    return size * 0.55


def tw(s, size=12.0):
    return sum(cw(c, size) for c in s)


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Ent:
    def __init__(self, name, x, y, attrs, sub=""):
        self.name, self.x, self.y, self.attrs, self.sub = name, x, y, attrs, sub
        self.nw = max([tw(name, 12.5)] + [tw(a[0], 11.5) + tw(a[2], 9.5) + 6 for a in attrs if a[2]])
        self.tw_ = max(tw(a[1], 10.5) for a in attrs) if attrs else 0
        self.w = max(120, 18 + self.nw + 26 + self.tw_ + 12)
        self.h = 25 + 7 + len(attrs) * 17 + 7

    def cx(self):
        return self.x + self.w / 2.0

    def cy(self):
        return self.y + self.h / 2.0

    def svg(self):
        o = ['<g>',
             '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#ffffff" stroke="%s" stroke-width="1"/>'
             % (self.x, self.y, self.w, self.h, LINE),
             '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1"/>'
             % (self.x, self.y + 25, self.x + self.w, self.y + 25, LINE),
             '<text x="%.1f" y="%.1f" font-family="%s" font-size="12.5" font-weight="500" fill="%s" '
             'text-anchor="middle">%s</text>'
             % (self.cx(), self.y + 17, FONT, INK, esc(self.name))]
        if self.sub:
            o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="9.5" fill="%s" '
                     'text-anchor="middle">%s</text>'
                     % (self.cx(), self.y + 33, FONT, MUT, esc(self.sub)))
        for i, (an, at, ak) in enumerate(self.attrs):
            ry = self.y + 25 + 7 + i * 17 + 12
            tx = self.x + 12
            o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="11.5" fill="%s">%s</text>'
                     % (tx, ry, FONT, INK, esc(an)))
            nwid = tw(an, 11.5)
            if "PK" in ak:
                o.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="0.9"/>'
                         % (tx, ry + 2.5, tx + nwid, ry + 2.5, INK))
            if ak:
                o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="9.5" fill="%s">%s</text>'
                         % (tx + nwid + 5, ry, FONT, MUT, esc(ak)))
            o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="10.5" fill="%s" '
                     'text-anchor="end">%s</text>'
                     % (self.x + self.w - 12, ry, FONT, MUT, esc(at)))
        o.append('</g>')
        return "".join(o)


def attach(box, tx, ty):
    dx, dy = tx - box.cx(), ty - box.cy()
    if abs(dx) * box.h >= abs(dy) * box.w:
        return ((box.x + box.w, box.cy()), "r") if dx > 0 else ((box.x, box.cy()), "l")
    return ((box.cx(), box.y + box.h), "b") if dy > 0 else ((box.cx(), box.y), "t")


def vertex(dcx, dcy, hw, hh, px, py):
    dx, dy = px - dcx, py - dcy
    if abs(dx) * hh >= abs(dy) * hw:
        return (dcx + hw, dcy) if dx > 0 else (dcx - hw, dcy)
    return (dcx, dcy + hh) if dy > 0 else (dcx, dcy - hh)


def path_ortho(p, side, v, vs):
    """从实体边 p（side）到菱形顶点 v（vs），轴对齐折线。"""
    px, py = p
    vx, vy = v
    if side in ("l", "r") and vs in ("l", "r"):
        mx = (px + vx) / 2.0
        return "M%.1f %.1f L%.1f %.1f L%.1f %.1f L%.1f %.1f" % (px, py, mx, py, mx, vy, vx, vy)
    if side in ("t", "b") and vs in ("t", "b"):
        my = (py + vy) / 2.0
        return "M%.1f %.1f L%.1f %.1f L%.1f %.1f L%.1f %.1f" % (px, py, px, my, vx, my, vx, vy)
    return "M%.1f %.1f L%.1f %.1f" % (px, py, vx, vy)


class Rel:
    def __init__(self, a, b, verb, ca, cb, dx=0.0, dy=0.0, sidea=None, sideb=None):
        self.a, self.b, self.verb, self.ca, self.cb = a, b, verb, ca, cb
        self.dx, self.dy, self.sidea, self.sideb = dx, dy, sidea, sideb


def render(title, ents, rels, width, note="", pad_top=54):
    emap = {e.name: e for e in ents}
    hw = max(30.0, max([tw(r.verb, 11.0) / 2.0 + 15 for r in rels]) if rels else 30.0)
    hh = 17.0
    body = []
    bottom = 0
    for e in ents:
        body.append(e.svg())
        bottom = max(bottom, e.y + e.h)
    for r in rels:
        A, B = emap[r.a], emap[r.b]
        dcx = (A.cx() + B.cx()) / 2.0 + r.dx
        dcy = (A.cy() + B.cy()) / 2.0 + r.dy
        pa, sa = attach(A, dcx, dcy)
        pb, sb = attach(B, dcx, dcy)
        if r.sidea:
            sa = r.sidea
            pa = {"l": (A.x, A.cy()), "r": (A.x + A.w, A.cy()),
                  "t": (A.cx(), A.y), "b": (A.cx(), A.y + A.h)}[sa]
        if r.sideb:
            sb = r.sideb
            pb = {"l": (B.x, B.cy()), "r": (B.x + B.w, B.cy()),
                  "t": (B.cx(), B.y), "b": (B.cx(), B.y + B.h)}[sb]
        va = vertex(dcx, dcy, hw, hh, pa[0], pa[1])
        vb = vertex(dcx, dcy, hw, hh, pb[0], pb[1])
        vsa = "l" if va[0] < dcx - 1 else ("r" if va[0] > dcx + 1 else ("t" if va[1] < dcy else "b"))
        vsb = "l" if vb[0] < dcx - 1 else ("r" if vb[0] > dcx + 1 else ("t" if vb[1] < dcy else "b"))
        body.append('<path d="%s" fill="none" stroke="%s" stroke-width="1"/>'
                    % (path_ortho(pa, sa, va, vsa), LINE))
        body.append('<path d="%s" fill="none" stroke="%s" stroke-width="1"/>'
                    % (path_ortho(pb, sb, vb, vsb), LINE))
        body.append('<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="#ffffff" '
                    'stroke="%s" stroke-width="1"/>'
                    % (dcx, dcy - hh, dcx + hw, dcy, dcx, dcy + hh, dcx - hw, dcy, LINE))
        body.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="11" fill="%s" '
                    'text-anchor="middle">%s</text>'
                    % (dcx, dcy + 3.5, FONT, INK, esc(r.verb)))
        for (p, v, card) in ((pa, va, r.ca), (pb, vb, r.cb)):
            if not card:
                continue
            t = 0.34
            lx = p[0] + (v[0] - p[0]) * t
            ly = p[1] + (v[1] - p[1]) * t
            anc = "start" if v[0] >= p[0] else "end"
            dyf = -5 if abs(v[1] - p[1]) > abs(v[0] - p[0]) else 0
            dxf = -6 if anc == "end" else 6
            if abs(v[1] - p[1]) > abs(v[0] - p[0]):
                dxf = 0
                if v[1] > p[1]:
                    dyf = 12
            body.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="11" font-weight="500" '
                        'fill="%s" text-anchor="%s">%s</text>'
                        % (lx + dxf, ly + dyf, FONT, INK, "middle" if dxf == 0 else anc, esc(card)))
        bottom = max(bottom, dcy + hh + 6)
    H = int(bottom + 34)
    head = ['<text x="20" y="26" font-family="%s" font-size="13" font-weight="500" fill="%s">%s</text>'
            % (FONT, INK, esc(title))]
    if note:
        head.append('<text x="20" y="43" font-family="%s" font-size="10.5" fill="%s">%s</text>'
                    % (FONT, MUT, esc(note)))
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">'
            '<rect width="100%%" height="100%%" fill="#ffffff"/>%s%s</svg>'
            % (width, H, width, H, "".join(head), "".join(body)))


# ---------------------------------------------------------------- 图定义
D = []

D.append(dict(
    fn="00-global", title="图 0  全局数据模型（17 张服务端表 + 2 张客户端结构）",
    note="实体=表，菱形=关系，两端数字=基数；PK 加下划线，FK 标注外键。owner_id 为跨库逻辑外键，指向 users.db。",
    width=1740,
    ents=[
        Ent("users", 40, 400, [("id", "INTEGER", "PK")]),
        Ent("sessions", 400, 60, [("id", "INTEGER", "PK")]),
        Ent("dir_node", 400, 180, [("owner_id", "INTEGER", "PK,FK")]),
        Ent("tag", 400, 300, [("id", "INTEGER", "PK")]),
        Ent("file_node", 400, 420, [("id", "INTEGER", "PK")]),
        Ent("upload_session", 400, 620, [("upload_id", "INTEGER", "PK")]),
        Ent("file_dir", 780, 60, [("file_id", "INTEGER", "PK,FK")]),
        Ent("file_tag", 780, 180, [("file_id", "INTEGER", "PK,FK")]),
        Ent("shares", 780, 300, [("id", "INTEGER", "PK")]),
        Ent("file_version", 780, 420, [("id", "INTEGER", "PK")]),
        Ent("file_chunk", 780, 540, [("file_id", "INTEGER", "PK,FK")]),
        Ent("upload_chunk", 780, 660, [("upload_id", "INTEGER", "PK,FK")]),
        Ent("upload_dir", 780, 780, [("upload_id", "INTEGER", "PK,FK")]),
        Ent("upload_flags", 780, 900, [("upload_id", "INTEGER", "PK,FK")]),
        Ent("chunk", 1140, 540, [("hash", "TEXT", "PK")]),
        Ent("file_version_chunk", 1440, 420, [("version_id", "INTEGER", "PK,FK")]),
        Ent("file_scan", 1440, 660, [("content_hash", "TEXT", "PK")]),
        Ent("sync_pair", 1140, 900, [("id", "TEXT", "PK")]),
        Ent("sync_index", 1440, 900, [("pair_id", "TEXT", "PK,FK")]),
    ],
    rels=[
        Rel("users", "sessions", "拥有", "1", "N", dy=-10),
        Rel("users", "dir_node", "拥有", "1", "N"),
        Rel("users", "tag", "拥有", "1", "N"),
        Rel("users", "file_node", "拥有", "1", "N"),
        Rel("users", "upload_session", "发起", "1", "N", dy=30),
        Rel("dir_node", "file_dir", "容纳", "1", "N"),
        Rel("tag", "file_tag", "关联", "1", "N", dy=-52),
        Rel("file_node", "file_dir", "位于", "1", "1", dy=16),
        Rel("file_node", "file_tag", "被打标", "1", "N", dy=24),
        Rel("file_node", "shares", "被分享", "1", "N", dy=42),
        Rel("file_node", "file_version", "拥有历史", "1", "N", dy=40),
        Rel("file_node", "file_chunk", "由分块组成", "1", "N", dy=48),
        Rel("file_version", "file_version_chunk", "快照", "1", "N"),
        Rel("file_chunk", "chunk", "引用", "N", "1"),
        Rel("chunk", "file_version_chunk", "被引用", "1", "N"),
        Rel("chunk", "file_scan", "被扫描", "1", "0..1"),
        Rel("upload_session", "upload_chunk", "已收分块", "1", "N"),
        Rel("upload_session", "upload_dir", "目标目录", "1", "1"),
        Rel("upload_session", "upload_flags", "会话标志", "1", "1"),
        Rel("sync_pair", "sync_index", "本地索引", "1", "N"),
    ]))

D.append(dict(
    fn="01-auth", title="图 1  账号与会话（users.db）", width=900,
    note="一个用户签发多个会话；sessions 只存 token 的 SHA-256，token_version 用于改密后批量失效。",
    ents=[
        Ent("users", 40, 70, [
            ("id", "INTEGER", "PK"), ("username", "TEXT", "UK"), ("display_name", "TEXT", ""),
            ("password_hash", "TEXT", ""), ("password_salt", "TEXT", ""), ("iterations", "INTEGER", ""),
            ("created_at", "INTEGER", ""), ("last_login_at", "INTEGER", ""),
            ("failed_attempts", "INTEGER", ""), ("locked_until", "INTEGER", ""),
            ("status", "TEXT", ""), ("token_version", "INTEGER", ""), ("quota_bytes", "INTEGER", "")]),
        Ent("sessions", 520, 70, [
            ("id", "INTEGER", "PK"), ("token_hash", "TEXT", "UK"), ("user_id", "INTEGER", "FK"),
            ("created_at", "INTEGER", ""), ("expires_at", "INTEGER", ""),
            ("last_seen_at", "INTEGER", ""), ("user_agent", "TEXT", ""),
            ("revoked", "INTEGER", ""), ("token_version", "INTEGER", "")]),
    ],
    rels=[Rel("users", "sessions", "拥有", "1", "N")]))

D.append(dict(
    fn="02-file-dir", title="图 2  文件与目录", width=1000,
    note="目录用 (owner_id, path) 复合主键表达层级；file_dir 以 file_id 作主键兼外键，与 file_node 一对一。",
    ents=[
        Ent("users", 40, 60, [("id", "INTEGER", "PK")]),
        Ent("dir_node", 40, 180, [
            ("owner_id", "INTEGER", "PK,FK"), ("path", "TEXT", "PK"), ("created_at", "INTEGER", ""),
            ("deleted", "INTEGER", ""), ("deleted_at", "INTEGER", "")]),
        Ent("file_node", 400, 60, [
            ("id", "INTEGER", "PK"), ("name", "TEXT", ""), ("size", "INTEGER", ""),
            ("content_hash", "TEXT", ""), ("chunk_count", "INTEGER", ""), ("created_at", "INTEGER", ""),
            ("deleted", "INTEGER", ""), ("owner_id", "INTEGER", "FK"), ("deleted_at", "INTEGER", "")]),
        Ent("file_dir", 760, 430, [("file_id", "INTEGER", "PK,FK"), ("dir", "TEXT", "")]),
    ],
    rels=[Rel("users", "dir_node", "拥有", "1", "N"),
          Rel("users", "file_node", "拥有", "1", "N"),
          Rel("file_node", "file_dir", "位于", "1", "1"),
          Rel("dir_node", "file_dir", "容纳", "1", "N")]))

D.append(dict(
    fn="03-chunk", title="图 3  内容分块存储", width=1000,
    note="file_node 与 chunk 概念上是多对多，由 file_chunk 用 seq 分解为有序一对多；ref_count 归零即可回收。",
    ents=[
        Ent("file_node", 40, 80, [("id", "INTEGER", "PK"), ("content_hash", "TEXT", ""),
                                  ("chunk_count", "INTEGER", ""), ("size", "INTEGER", "")]),
        Ent("chunk", 40, 320, [("hash", "TEXT", "PK"), ("size", "INTEGER", ""),
                               ("ref_count", "INTEGER", ""), ("created_at", "INTEGER", "")]),
        Ent("file_chunk", 640, 190, [("file_id", "INTEGER", "PK,FK"), ("seq", "INTEGER", "PK"),
                                     ("chunk_hash", "TEXT", "FK")]),
    ],
    rels=[Rel("file_node", "file_chunk", "由分块组成", "1", "N"),
          Rel("chunk", "file_chunk", "被引用", "1", "N", dy=10)]))

D.append(dict(
    fn="04-version", title="图 4  版本历史", width=1060,
    note="file_version 只存历史快照，当前内容永远在 file_node；两处 1:N 共享同一批 chunk 行，回滚零拷贝。",
    ents=[
        Ent("file_node", 40, 70, [("id", "INTEGER", "PK"), ("content_hash", "TEXT", ""),
                                  ("chunk_count", "INTEGER", "")]),
        Ent("file_version", 40, 300, [
            ("id", "INTEGER", "PK"), ("file_id", "INTEGER", "FK"), ("version_no", "INTEGER", ""),
            ("content_hash", "TEXT", ""), ("size", "INTEGER", ""), ("chunk_count", "INTEGER", ""),
            ("device", "TEXT", ""), ("note", "TEXT", ""), ("created_at", "INTEGER", "")]),
        Ent("file_version_chunk", 620, 330, [
            ("version_id", "INTEGER", "PK,FK"), ("seq", "INTEGER", "PK"), ("chunk_hash", "TEXT", "FK")]),
        Ent("chunk", 620, 90, [("hash", "TEXT", "PK"), ("ref_count", "INTEGER", "")]),
    ],
    rels=[Rel("file_node", "file_version", "拥有历史", "1", "N"),
          Rel("file_version", "file_version_chunk", "快照分块", "1", "N"),
          Rel("chunk", "file_version_chunk", "共享内容", "1", "N", dx=-40)]))

D.append(dict(
    fn="05-upload", title="图 5  分块上传会话", width=1100,
    note="主键即外键扩展出 upload_dir / upload_flags 两张一对一表；upload_chunk.sha256 是合并时逐块重算比对的依据。",
    ents=[
        Ent("users", 40, 70, [("id", "INTEGER", "PK")]),
        Ent("upload_session", 420, 70, [
            ("upload_id", "INTEGER", "PK"), ("name", "TEXT", ""), ("size", "INTEGER", ""),
            ("chunk_size", "INTEGER", ""), ("file_hash", "TEXT", ""), ("status", "TEXT", ""),
            ("created_at", "INTEGER", ""), ("updated_at", "INTEGER", ""), ("owner_id", "INTEGER", "FK")]),
        Ent("upload_chunk", 40, 360, [("upload_id", "INTEGER", "PK,FK"), ("seq", "INTEGER", "PK"),
                                      ("size", "INTEGER", ""), ("sha256", "TEXT", ""),
                                      ("created_at", "INTEGER", "")]),
        Ent("upload_dir", 420, 360, [("upload_id", "INTEGER", "PK,FK"), ("dir", "TEXT", "")]),
        Ent("upload_flags", 780, 360, [("upload_id", "INTEGER", "PK,FK"), ("overwrite", "INTEGER", "")]),
    ],
    rels=[Rel("users", "upload_session", "发起", "1", "N"),
          Rel("upload_session", "upload_chunk", "已收分块", "1", "N"),
          Rel("upload_session", "upload_dir", "目标目录", "1", "1"),
          Rel("upload_session", "upload_flags", "会话标志", "1", "1", dx=20, dy=10)]))

D.append(dict(
    fn="06-tag", title="图 6  标签", width=1000,
    note="file_node 与 tag 是多对多，由连接表 file_tag 分解为两个一对多；库内无物理外键，删标签须手写级联。",
    ents=[
        Ent("users", 40, 70, [("id", "INTEGER", "PK")]),
        Ent("tag", 420, 70, [("id", "INTEGER", "PK"), ("owner_id", "INTEGER", "FK"),
                             ("name", "TEXT", ""), ("color", "TEXT", ""),
                             ("created_at", "INTEGER", "")]),
        Ent("file_node", 40, 330, [("id", "INTEGER", "PK"), ("name", "TEXT", ""),
                                   ("owner_id", "INTEGER", "FK")]),
        Ent("file_tag", 760, 200, [("file_id", "INTEGER", "PK,FK"), ("tag_id", "INTEGER", "PK,FK")]),
    ],
    rels=[Rel("users", "tag", "拥有", "1", "N"),
          Rel("file_node", "file_tag", "被打标", "1", "N"),
          Rel("tag", "file_tag", "关联", "1", "N")]))

D.append(dict(
    fn="07-share", title="图 7  分享链接", width=1060,
    note="shares 没有 owner_id 列，归属靠连 file_node 取 owner_id 推导；code 只存 sha256(token+code)。",
    ents=[
        Ent("users", 40, 70, [("id", "INTEGER", "PK")]),
        Ent("file_node", 400, 70, [("id", "INTEGER", "PK"), ("name", "TEXT", ""),
                                   ("owner_id", "INTEGER", "FK")]),
        Ent("shares", 760, 70, [
            ("id", "INTEGER", "PK"), ("token", "TEXT", "UK"), ("file_id", "INTEGER", "FK"),
            ("code_hash", "TEXT", ""), ("expires_at", "INTEGER", ""),
            ("max_downloads", "INTEGER", ""), ("downloads", "INTEGER", ""),
            ("created_at", "INTEGER", ""), ("revoked", "INTEGER", "")]),
    ],
    rels=[Rel("users", "file_node", "拥有", "1", "N"),
          Rel("file_node", "shares", "被分享", "1", "N")]))

D.append(dict(
    fn="08-scan", title="图 8  文件安全扫描", width=1000,
    note="file_scan 以内容哈希为主键，与 chunk、file_node 均为一对零或一；未收录（verdict=0）不等于安全。",
    ents=[
        Ent("chunk", 40, 80, [("hash", "TEXT", "PK"), ("size", "INTEGER", "")]),
        Ent("file_node", 40, 280, [("id", "INTEGER", "PK"), ("content_hash", "TEXT", "")]),
        Ent("file_scan", 640, 150, [
            ("content_hash", "TEXT", "PK"), ("verdict", "INTEGER", ""),
            ("engines_malicious", "INTEGER", ""), ("engines_suspicious", "INTEGER", ""),
            ("engines_total", "INTEGER", ""), ("detail", "TEXT", ""),
            ("scanned_at", "INTEGER", "")]),
    ],
    rels=[Rel("chunk", "file_scan", "判定缓存", "1", "0..1"),
          Rel("file_node", "file_scan", "判定缓存", "1", "0..1", dy=10)]))

D.append(dict(
    fn="09-sync", title="图 9  客户端同步（非服务端库）", width=900,
    note="sync_pair 存 settings.ini 的 JSON，sync_index 存独立 sqlite；pair_id 是跨存储的逻辑外键。",
    ents=[
        Ent("sync_pair", 40, 70, [("id", "TEXT", "PK"), ("local_path", "TEXT", ""),
                                  ("remote_path", "TEXT", ""), ("mode", "TEXT", ""),
                                  ("exclude_dirs", "TEXT", ""), ("enabled", "INTEGER", "")]),
        Ent("sync_index", 520, 70, [("pair_id", "TEXT", "PK,FK"), ("rel_path", "TEXT", "PK"),
                                    ("is_dir", "INTEGER", ""), ("size", "INTEGER", ""),
                                    ("mtime", "TEXT", ""), ("hash", "TEXT", ""),
                                    ("rev", "TEXT", "")]),
    ],
    rels=[Rel("sync_pair", "sync_index", "本地索引", "1", "N")]))

# ---------------------------------------------------------------- 图例
LEGEND = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 420" width="900" height="420">
<rect width="100%" height="100%" fill="#ffffff"/>
<text x="20" y="26" font-family="{f}" font-size="13" font-weight="500" fill="{ink}">符号说明（全套图统一使用）</text>
<rect x="20" y="50" width="170" height="70" fill="#ffffff" stroke="{ln}" stroke-width="1"/>
<line x1="20" y1="75" x2="190" y2="75" stroke="{ln}" stroke-width="1"/>
<text x="105" y="67" font-family="{f}" font-size="12.5" font-weight="500" fill="{ink}" text-anchor="middle">实体名</text>
<text x="32" y="94" font-family="{f}" font-size="11.5" fill="{ink}">主键列</text>
<line x1="32" y1="96.5" x2="76" y2="96.5" stroke="{ink}" stroke-width="0.9"/>
<text x="81" y="94" font-family="{f}" font-size="9.5" fill="{mu}">PK</text>
<text x="178" y="94" font-family="{f}" font-size="10.5" fill="{mu}" text-anchor="end">INTEGER</text>
<text x="32" y="111" font-family="{f}" font-size="11.5" fill="{ink}">外键列</text>
<text x="76" y="111" font-family="{f}" font-size="9.5" fill="{mu}">FK</text>
<text x="178" y="111" font-family="{f}" font-size="10.5" fill="{mu}" text-anchor="end">TEXT</text>
<text x="210" y="70" font-family="{f}" font-size="11.5" fill="{ink}">矩形 = 实体（表）</text>
<text x="210" y="92" font-family="{f}" font-size="11.5" fill="{ink}">PK：主键，加下划线</text>
<text x="210" y="114" font-family="{f}" font-size="11.5" fill="{ink}">FK：外键；UK：唯一键</text>
<polygon points="450,85 500,105 450,125 400,105" fill="#ffffff" stroke="{ln}" stroke-width="1"/>
<text x="450" y="109" font-family="{f}" font-size="11" fill="{ink}" text-anchor="middle">关系</text>
<text x="520" y="102" font-family="{f}" font-size="11.5" fill="{ink}">菱形 = 关系（动词）</text>
<text x="520" y="122" font-family="{f}" font-size="11.5" fill="{ink}">基线两端标注基数</text>
<text x="210" y="160" font-family="{f}" font-size="11.5" fill="{ink}">1 : 1</text>
<text x="270" y="160" font-family="{f}" font-size="11.5" fill="{mu}">一对一</text>
<text x="210" y="182" font-family="{f}" font-size="11.5" fill="{ink}">1 : N / 1 : 0..1</text>
<text x="330" y="182" font-family="{f}" font-size="11.5" fill="{mu}">一对多 / 一对零或一</text>
<text x="210" y="204" font-family="{f}" font-size="11.5" fill="{ink}">M : N</text>
<text x="270" y="204" font-family="{f}" font-size="11.5" fill="{mu}">多对多，由连接表分解为两个 1 : N</text>
<text x="20" y="240" font-family="{f}" font-size="11.5" font-weight="500" fill="{ink}">约定</text>
<text x="20" y="262" font-family="{f}" font-size="11" fill="{mu}">1. 实体名 = 真实表名，列名 = 真实列名（snake_case），不另起业务名。</text>
<text x="20" y="282" font-family="{f}" font-size="11" fill="{mu}">2. 库内没有物理 FOREIGN KEY 约束，所有 FK 都是代码保证的逻辑外键（级联需手写）。</text>
<text x="20" y="302" font-family="{f}" font-size="11" fill="{mu}">3. owner_id 指向 users.id，users 在独立库 users.db，属跨库逻辑外键。</text>
<text x="20" y="322" font-family="{f}" font-size="11" fill="{mu}">4. 时间列 created_at / expires_at / deleted_at / scanned_at 全部为 Unix 毫秒。</text>
<text x="20" y="342" font-family="{f}" font-size="11" fill="{mu}">5. deleted=0 正常；deleted=1 且 deleted_at &gt; 0 在回收站；deleted=1 且 deleted_at=0 为硬删墓碑。</text>
<text x="20" y="362" font-family="{f}" font-size="11" fill="{mu}">6. 图 9 属客户端本地存储，不落服务端数据库。</text>
</svg>""".format(f=FONT, ink=INK, ln=LINE, mu=MUT)


def main():
    erdir = os.path.join(OUT, "er")
    os.makedirs(erdir, exist_ok=True)
    svgs = []
    for d in D:
        s = render(d["title"], d["ents"], d["rels"], d["width"], d["note"])
        svgs.append((d["fn"], d["title"], s))
        with open(os.path.join(erdir, d["fn"] + ".svg"), "w", encoding="utf-8") as f:
            f.write(s)
    legend = LEGEND
    with open(os.path.join(erdir, "legend.svg"), "w", encoding="utf-8") as f:
        f.write(legend)

    tabs = ('<button class="tab" data-i="0" aria-selected="true">符号说明</button>' +
            "".join('<button class="tab" data-i="%d">%s</button>' % (i + 1, t.split("  ")[0])
                    for i, (_, t, _) in enumerate(svgs)))
    panels = '<div class="panel on" data-i="0">%s</div>' % legend
    for i, (_, _, s) in enumerate(svgs):
        panels += '<div class="panel" data-i="%d">%s</div>' % (i + 1, s)

    html = """<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<title>云匣 CloudVault · 系统 ER 图集</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:24px 28px 60px;background:#fff;color:#1a1a1a;
     font-family:"Segoe UI","Microsoft YaHei","PingFang SC",sans-serif;font-size:13px}
h1{font-size:16px;font-weight:500;margin:0 0 4px}
.sub{color:#8c8c8c;font-size:12px;margin:0 0 16px}
.tabs{display:flex;flex-wrap:wrap;gap:2px;border-bottom:1px solid #dcdcdc;margin-bottom:18px}
.tab{background:none;border:0;border-bottom:2px solid transparent;padding:7px 11px;font-size:12.5px;
     color:#6b6b6b;cursor:pointer;font-family:inherit;margin-bottom:-1px}
.tab:hover{color:#1a1a1a}
.tab[aria-selected=true]{color:#1a1a1a;border-bottom-color:#1a1a1a;font-weight:500}
.panel{display:none;overflow:auto}
.panel.on{display:block}
.panel svg{border:1px solid #e6e6e6;background:#fff;max-width:none}
</style></head><body>
<h1>云匣 CloudVault · 系统 ER 图集</h1>
<p class="sub">9 个模块 ER 图 + 1 张全局数据模型图，符号与命名全图统一</p>
<div class="tabs" id="tabs">%s</div>
%s
<script>
var tabs=[].slice.call(document.querySelectorAll('.tab'));
var panels=[].slice.call(document.querySelectorAll('.panel'));
tabs.forEach(function(t){t.onclick=function(){
  var i=t.getAttribute('data-i');
  tabs.forEach(function(x){x.setAttribute('aria-selected',x===t?'true':'false')});
  panels.forEach(function(p){p.classList.toggle('on',p.getAttribute('data-i')===i)});
}});
</script></body></html>""" % (tabs, panels)

    p = os.path.join(OUT, "云匣CloudVault-ER图集.html")
    with open(p, "w", encoding="utf-8") as f:
        f.write(html)
    print("OK", p, len(svgs) + 1, "diagrams")


if __name__ == "__main__":
    main()
