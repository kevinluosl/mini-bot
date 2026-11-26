#!/usr/bin/env python3
"""构建独立的 cam 固件：入口、默认配置和源码都在 mini-bot。"""
import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BOARD = 'deepbot-s3-cam-n16r8'
WAKE_WORD = 'wn9_xiaomingtongxue_tts2'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board', choices=[BOARD, 'mini-bot'])
    parser.add_argument('--name', required=True, choices=[BOARD, 'mini-bot'])
    parser.add_argument('--language', required=True, choices=['zh-CN'])
    parser.add_argument('--wake-word', required=True, choices=[WAKE_WORD])
    args = parser.parse_args()
    idf_path = os.environ.get('IDF_PATH')
    idf = Path(idf_path) / 'tools/idf.py' if idf_path else None
    if idf is None or not idf.is_file() or not os.environ.get('IDF_PYTHON_ENV_PATH'):
        parser.error('请先 source ~/esp/esp-idf/export.sh')
    defaults = ['sdkconfig.defaults']
    if (ROOT / 'sdkconfig.local.defaults').is_file():
        defaults.append('sdkconfig.local.defaults')
    # 保持设备上报的板型名称不变，本地默认配置不进入仓库。
    return subprocess.call([
        sys.executable, str(idf), '-C', str(ROOT), '-B', str(ROOT / 'build'),
        '-D', 'IDF_TARGET=esp32s3', '-D', f'BOARD_NAME={BOARD}',
        '-D', f'SDKCONFIG_DEFAULTS={";".join(defaults)}', 'build',
    ], cwd=ROOT)


if __name__ == '__main__':
    sys.exit(main())
