# -*- coding: utf-8 -*-
r"""
本仓库所有 Python 工具的路径都从这里取 —— 其它脚本里不许再出现绝对路径。

三个可配置项，都用环境变量覆盖；不设则用下面的默认值：

  SI_SRC        System Informer 源码树（含 .rc 与 .c/.cpp）。
                词典流水线要从里面抽资源串、菜单串、选项页串，本仓库不带源码。
                默认 <仓库>\si-src

  SI_DIR        已安装的 System Informer 目录（含 SystemInformer.exe）。
                只有「真机端到端验证」用得到（要写它的 _zhcn 目录）。
                默认 C:\Program Files\SystemInformer

  SI_TEST_DIR   用于验证的 System Informer 副本目录（一个完整可运行的拷贝）。
                跑验证脚本时不碰你正在用的那份。默认 <仓库>\test-app

目录约定：

  <仓库>\dict\     词典与抽取出来的清单（**进版本库**，是流水线的输入）
  <仓库>\build\    流水线与编译的中间产物（不进版本库）
  <仓库>\dist\     编译输出（不进版本库）

词典流水线的顺序（每一步的产物都是下一步的输入）：

  dict\passolo.txt + dict\zh_dict.base.txt + tools\ui_zh.py + dict\ui_gap.tsv
        --[build_zh_dict.py]-->  build\zh_dict.merged.txt   （扁平表）
        --[organize.py]------->  build\zh_dict.categorized.txt（分区表，给人看）
                                 build\zh_dict.index.tsv     （逐条出处，审计用）
                                 dict\zh_dict.txt            （部署用的最终词典）
        --[build.ps1]--------->  dist\zh_dict.txt

  另有 dict\zh_dict.pattern.txt —— 通配符模式表，给「运行期拼出来的串」用
  （进程气泡、状态栏那类）。它不进上面的流水线，是手工维护的输入，
  由 tools\check_pattern.py 校验、build.ps1 原样拷到 dist\。
"""

import os

# ---------------------------------------------------------------- 根目录
TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)

DICT = os.path.join(REPO, "dict")
BUILD = os.path.join(REPO, "build")
DIST = os.path.join(REPO, "dist")
SHOTS = os.path.join(BUILD, "shots")

# ---------------------------------------------------------------- 可配置项
SI_SRC = os.environ.get("SI_SRC") or os.path.join(REPO, "si-src")
SI_DIR = os.environ.get("SI_DIR") or r"C:\Program Files\SystemInformer"
SI_TEST_DIR = os.environ.get("SI_TEST_DIR") or os.path.join(REPO, "test-app")

# ---------------------------------------------------------------- 流水线输入
PASSOLO = os.path.join(DICT, "passolo.txt")          # Passolo 导出的英中对照
BASE_DICT = os.path.join(DICT, "zh_dict.base.txt")   # 人工基线（第 1 段）
RC_LITERALS = os.path.join(DICT, "rc_literals.json")  # .rc 资源串（rc_survey.py 产出）
UI_GAP_TSV = os.path.join(DICT, "ui_gap.tsv")        # 源码 UI 串清单（ui_gap.py 产出）
PATTERN = os.path.join(DICT, "zh_dict.pattern.txt")  # 通配符模式表（手工维护）

# ---------------------------------------------------------------- 流水线产物
MERGED = os.path.join(BUILD, "zh_dict.merged.txt")
CATEGORIZED = os.path.join(BUILD, "zh_dict.categorized.txt")
INDEX_TSV = os.path.join(BUILD, "zh_dict.index.tsv")
ZH_DICT = os.path.join(DICT, "zh_dict.txt")          # 最终部署词典

# 运行期未命中：临时工作表
MISS_WORK = os.path.join(BUILD, "zh_dict.missed.work.tsv")


# ---------------------------------------------------------------- 部署与验证目标
def app_exe(directory=None):
    """某个 SI 目录下的 SystemInformer.exe 路径。"""
    return os.path.join(directory or SI_TEST_DIR, "SystemInformer.exe")


def zh_dir(directory=None):
    """中文层的落点目录（与 SystemInformer.exe 同级，安装脚本铺的就是它）。"""
    return os.path.join(directory or SI_DIR, "_zhcn")


def miss_files():
    r"""运行期未命中收集文件可能出现的位置，按可靠性排序。

    zh_hook.dll 把收集文件写在**词典所在目录**。词典在「构建期」与「部署期」
    不是同一个目录，所以把这些位置都扫一遍、按原文合并计数 —— 比硬编码
    某一个路径可靠得多。可以用 ZHHK_MISS 指定一个额外位置。
    """
    out = []
    extra = os.environ.get("ZHHK_MISS")
    if extra:
        out.append(extra)
    out.append(os.path.join(DIST, "zh_dict.missed.txt"))       # 构建/测试运行
    out.append(os.path.join(zh_dir(), "zh_dict.missed.txt"))   # 真实安装
    return out


def ensure(*dirs):
    """按需建目录（流水线脚本开头调用）。"""
    for d in dirs:
        os.makedirs(d, exist_ok=True)


def describe():
    """打一行环境摘要，方便排障时确认到底读了哪个目录。"""
    return "仓库=%s\n  SI_SRC=%s\n  SI_DIR=%s\n  SI_TEST_DIR=%s" % (REPO, SI_SRC, SI_DIR, SI_TEST_DIR)


if __name__ == "__main__":
    print(describe())
