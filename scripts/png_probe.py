#!/usr/bin/env python3
"""PNG 像素探针 —— 用于 UI 截图的对齐/间距客观测量。

只依赖标准库（zlib）。用途举例：
  · 测量某一行文字的实际左右边界（判断对齐是否生效）
  · 测量控件高度、间距、颜色是否落在预期范围

用法：
  python scripts/png_probe.py <png> ink --y 122,142 [--bg 161A21] [--tol 40]
      输出该 y 区间内"非背景像素"的 x 范围与每段连续区间
  python scripts/png_probe.py <png> row --y 122,142 --bg 161A21
      输出每列的墨迹计数（找文字块边界）
  python scripts/png_probe.py <png> color --x 10 --y 10
      输出某像素的 RGB
"""
import sys
import zlib
import struct


def decode_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "不是 PNG 文件"
    pos = 8
    idat = bytearray()
    width = height = bitdepth = colortype = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bitdepth, colortype, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            assert bitdepth == 8 and interlace == 0, "仅支持 8 位非隔行 PNG"
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length

    channels = {0: 1, 2: 3, 4: 2, 6: 4}[colortype]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:    # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, out


def px(img, x, y):
    w, h, ch, buf = img
    o = (y * w + x) * ch
    return buf[o], buf[o + 1], buf[o + 2]


def parse_y(s):
    a, b = s.split(",")
    return int(a), int(b)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    path, mode = sys.argv[1], sys.argv[2]
    args = {k.lstrip("-"): v for k, v in zip(sys.argv[3::2], sys.argv[4::2])}
    img = decode_png(path)
    w, h, _, _ = img

    bg = args.get("bg")
    tol = int(args.get("tol", 40))
    bgc = None
    if bg:
        bgc = tuple(int(bg[i:i + 2], 16) for i in (0, 2, 4))

    if mode == "color":
        x, y = int(args["x"]), int(args["y"])
        print("(%d,%d) = #%02X%02X%02X" % (x, y, *px(img, x, y)))
        return 0

    y0, y1 = parse_y(args["y"])
    if mode == "ink":
        # 每列在 y 区间内是否出现过"非背景"像素
        cols = []
        for x in range(w):
            hit = False
            for y in range(y0, min(y1, h)):
                r, g, b = px(img, x, y)
                if bgc:
                    if abs(r - bgc[0]) + abs(g - bgc[1]) + abs(b - bgc[2]) > tol:
                        hit = True
                        break
                else:
                    if (r + g + b) > 3 * 60:
                        hit = True
                        break
            cols.append(hit)
        runs, start = [], None
        for x, hit in enumerate(cols):
            if hit and start is None:
                start = x
            elif not hit and start is not None:
                runs.append((start, x - 1))
                start = None
        if start is not None:
            runs.append((start, w - 1))
        runs = [r for r in runs if r[1] - r[0] >= 1]
        print("y=[%d,%d)  墨迹区间(x): %s" % (y0, y1, runs))
        for a, b in runs:
            print("   x=%d..%d  宽=%d" % (a, b, b - a + 1))
        return 0

    if mode == "row":
        counts = []
        for x in range(w):
            c = 0
            for y in range(y0, min(y1, h)):
                r, g, b = px(img, x, y)
                if bgc:
                    if abs(r - bgc[0]) + abs(g - bgc[1]) + abs(b - bgc[2]) > tol:
                        c += 1
                elif (r + g + b) > 3 * 60:
                    c += 1
            counts.append(c)
        print(" ".join("%d" % c for c in counts))
        return 0

    print("未知模式:", mode)
    return 1


if __name__ == "__main__":
    sys.exit(main())
