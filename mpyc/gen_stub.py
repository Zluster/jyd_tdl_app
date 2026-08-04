#!/usr/bin/env python3
"""从 gen_mpy 的元数据（lv_mpy.json）生成 mpyc 代理层的 IDE 补全存根。

产出 env_stub.pyi：描述 m.env.lv 的完整 API（控件类/方法签名/枚举/函数），
仅供类型检查器（Pylance 等）使用，运行时零依赖零改动。

用法（开发机，构建 MicroPython 后运行一次；lvgl API 变更后重新生成）:
    python3 launcher/mpyc/gen_stub.py \
        [ports/unix/build-embed/lvgl/lv_mpy.json] [launcher/mpyc/env_stub.pyi]
"""

import json
import keyword
import sys
import os

# C 类型 -> Python 注解。标量映射真实类型（桥按原类型传输），其余 Any。
_TYPE_MAP = {
    "int": "int",
    "bool": "bool",
    "float": "float",
    "char*": "str",
    "NoneType": "None",
}


def py_type(c_type):
    return _TYPE_MAP.get(c_type, "Any")


def sanitize(name, fallback):
    if not name or not name.isidentifier():
        return fallback
    if keyword.iskeyword(name):
        return name + "_"
    return name


def emit_signature(member, skip_first):
    """生成 'name: type' 参数列表（不含 self）。"""
    args = member.get("args", [])
    if skip_first and args:
        args = args[1:]
    parts = []
    seen = set()
    for i, a in enumerate(args):
        name = sanitize(a.get("name"), "arg%d" % i)
        while name in seen:
            name += "_"
        seen.add(name)
        parts.append("%s: %s" % (name, py_type(a.get("type"))))
    ret = py_type(member.get("return_type"))
    return ", ".join(parts), ret


def emit_enum_class(out, name, members, indent):
    pad = " " * indent
    out.append("%sclass %s:" % (pad, name))
    body = [m for m in members if m.isidentifier()]
    if not body:
        out.append("%s    ..." % pad)
        return
    for m in body:
        out.append("%s    %s: int" % (pad, sanitize(m, m)))


def main():
    root = os.path.dirname(os.path.abspath(__file__))
    json_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "..", "..", "ports", "unix", "build-embed", "lvgl", "lv_mpy.json")
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "env_stub.pyi")

    d = json.load(open(json_path))
    out = [
        "# 自动生成：python3 launcher/mpyc/gen_stub.py（勿手改）",
        "# 描述 mpyc 代理 m.env.lv 的 API，仅用于 IDE 补全/类型检查。",
        "from typing import Any",
        "",
    ]

    # ---- 顶层枚举 ----
    for ename, e in sorted(d.get("enums", {}).items()):
        if not ename.isidentifier():
            continue
        emit_enum_class(out, "%s_cls" % ename, e.get("members", {}), 0)
        out.append("")

    # ---- 控件类（label 等继承 obj）----
    objects = d.get("objects", {})
    ordered = (["obj"] if "obj" in objects else []) + sorted(
        n for n in objects if n != "obj")
    for oname in ordered:
        if not oname.isidentifier():
            continue
        base = "" if oname == "obj" else "(obj_cls)"
        out.append("class %s_cls%s:" % (oname, base))
        out.append("    def __init__(self, parent: Any = ..., copy: Any = ...) -> None: ...")
        members = objects[oname].get("members", {})
        for mname, m in sorted(members.items()):
            name = sanitize(mname, None)
            if name is None:
                continue
            t = m.get("type")
            if t == "function":
                sig, ret = emit_signature(m, skip_first=True)
                out.append("    def %s(self%s%s) -> %s: ..."
                           % (name, ", " if sig else "", sig, ret))
            elif t == "enum_type":
                emit_enum_class(out, name, m.get("members", {}), 4)
            else:
                out.append("    %s: Any" % name)
        out.append("    def __getattr__(self, name: str) -> Any: ...")
        out.append("")

    # ---- 结构体（字段信息缺失，宽松处理）----
    structs = [s for s in d.get("structs", []) if isinstance(s, str) and s.isidentifier()]
    for sname in sorted(set(structs)):
        out.append("class %s_cls:" % sname)
        out.append("    def __init__(self, values: dict = ...) -> None: ...")
        out.append("    def __getattr__(self, name: str) -> Any: ...")
        out.append("    def __setattr__(self, name: str, value: Any) -> None: ...")
        out.append("")

    # ---- lv 模块本体 ----
    out.append("class _LvModule:")
    for ename in sorted(d.get("enums", {})):
        if ename.isidentifier():
            out.append("    %s: type[%s_cls]" % (ename, ename))
    for oname in ordered:
        if oname.isidentifier():
            out.append("    %s: type[%s_cls]" % (oname, oname))
    for sname in sorted(set(structs)):
        out.append("    %s: type[%s_cls]" % (sname, sname))
    for cname in d.get("int_constants", []):
        if isinstance(cname, str) and cname.isidentifier():
            out.append("    %s: int" % cname)
    for bname in d.get("blobs", []):
        if isinstance(bname, str) and bname.isidentifier():
            out.append("    %s: Any" % bname)
    for fname, f in sorted(d.get("functions", {}).items()):
        name = sanitize(fname, None)
        if name is None:
            continue
        sig, ret = emit_signature(f, skip_first=False)
        out.append("    def %s(self%s%s) -> %s: ..."
                   % (name, ", " if sig else "", sig, ret))
    out.append("    def __getattr__(self, name: str) -> Any: ...")
    out.append("")

    # ---- m.env 根命名空间 ----
    out.append("class Env:")
    out.append("    lv: _LvModule")
    out.append("    mpy_embed: Any")
    out.append("    def __getattr__(self, name: str) -> Any: ...")
    out.append("    def __setattr__(self, name: str, value: Any) -> None: ...")
    out.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(out))
    print("wrote %s (%d lines)" % (out_path, len(out)))


if __name__ == "__main__":
    main()
