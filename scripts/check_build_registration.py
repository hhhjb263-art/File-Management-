#!/usr/bin/env python3
"""构建注册检查：确保所有源文件都被构建系统引用。

背景（两次真实事故）：
  1) 新增 `server/src/meta/share_repository.cpp` 未登记进 `server/CMakeLists.txt`
     ⇒ Linux 链接期报 7 个 undefined reference（语法检查抓不到）。
  2) 新增 QML 未登记进 `resources/resources.qrc` ⇒ 不进 exe，运行时找不到。

用法（仓库根执行）：
    python scripts/check_build_registration.py
退出码：0 = 全部已登记；1 = 有遗漏（打印清单）。
建议在每次提交前运行，并加入 CI / pre-commit。
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    try:
        with io.open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except IOError:
        return ""


def walk_ext(base, exts):
    out = []
    for dirpath, _, filenames in os.walk(os.path.join(ROOT, base)):
        for name in sorted(filenames):
            if name.endswith(exts):
                full = os.path.join(dirpath, name)
                out.append(os.path.relpath(full, ROOT).replace("\\", "/"))
    return sorted(out)


def check_server():
    """server/src/**/*.cpp 必须出现在 server/CMakeLists.txt 里。"""
    manifest = read(os.path.join(ROOT, "server/CMakeLists.txt"))
    missing = []
    for rel in walk_ext("server/src", (".cpp",)):
        name = os.path.basename(rel)
        if name not in manifest and rel not in manifest:
            missing.append(rel)
    return missing


def check_qt_sources():
    """src/**/*.{cpp,h} 必须被某个 .pri / File.pro / config.pri 引用。"""
    manifest = ""
    for rel in walk_ext("src", (".pri",)):
        manifest += read(os.path.join(ROOT, rel))
    manifest += read(os.path.join(ROOT, "File.pro"))
    manifest += read(os.path.join(ROOT, "config.pri"))
    missing = []
    for rel in walk_ext("src", (".cpp", ".h")):
        if os.path.basename(rel) not in manifest and rel not in manifest:
            missing.append(rel)
    return missing


def check_qml():
    """qml/**/*.qml 必须登记进 resources/resources.qrc，否则不进 exe。"""
    manifest = read(os.path.join(ROOT, "resources/resources.qrc"))
    missing = []
    for rel in walk_ext("qml", (".qml",)):
        if os.path.basename(rel) not in manifest:
            missing.append(rel)
    return missing


def main():
    groups = [
        ("服务端 .cpp → server/CMakeLists.txt", check_server()),
        ("客户端 src/** → *.pri / File.pro", check_qt_sources()),
        ("QML → resources/resources.qrc", check_qml()),
    ]
    failed = False
    for title, missing in groups:
        if missing:
            failed = True
            print("[FAIL] %s —— %d 个未登记：" % (title, len(missing)))
            for m in missing:
                print("       - %s" % m)
        else:
            print("[ OK ] %s" % title)
    if failed:
        print("\n请把上面列出的文件登记进对应的构建清单后再提交。")
        return 1
    print("\n构建注册检查通过：所有源文件均已被引用。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
