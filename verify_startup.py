#!/usr/bin/env python3
"""静态核验 mini-bot 启动所需的镜像、配置和独立编译路径；不连接设备。"""
import csv
import json
from pathlib import Path
import re
import shlex
import sys


ROOT = Path(__file__).resolve().parent
BUILD = ROOT / 'build'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_json(name):
    return json.loads((BUILD / name).read_text(encoding='utf-8'))


def verify():
    project = load_json('project_description.json')
    sdk = Path(project['idf_path']).resolve()
    require(Path(project['project_path']).resolve() == ROOT, '构建目录不属于当前工程')
    require(Path(project['config_file']).resolve() == ROOT / 'sdkconfig', '构建使用了外部 sdkconfig')
    require(project['target'] == 'esp32s3', '目标不是 ESP32-S3')
    config = set((ROOT / 'sdkconfig').read_text().splitlines())
    expected = {
        'CONFIG_BOARD_TYPE_DEEPBOT_S3_CAM_N16R8=y', 'CONFIG_LANGUAGE_ZH_CN=y',
        'CONFIG_USE_AFE_WAKE_WORD=y', 'CONFIG_SR_WN_WN9_XIAOMINGTONGXUE_TTS2=y',
        'CONFIG_SPIRAM_MODE_OCT=y', 'CONFIG_USE_DEVICE_AEC=y',
        'CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE=524288', 'CONFIG_OV3660_SUPPORT=y',
        '# CONFIG_CAMERA_PSRAM_DMA is not set', '# CONFIG_SEND_WAKE_WORD_DATA is not set',
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/16m.csv"',
    }
    require(expected <= config, f'启动配置不符合 cam 默认值：{sorted(expected - config)}')

    def check_path(name):
        path = Path(name).resolve()
        require(path.is_relative_to(ROOT) or path.is_relative_to(sdk), f'外部工程依赖：{path}')
        return path

    for path in project['build_component_paths']:
        check_path(path)
    commands = load_json('compile_commands.json')
    sources = {check_path(item['file']) for item in commands}
    local_path = re.escape(ROOT.name) + r'(?=[/="\s]|$)'
    sibling_path = re.compile(re.escape(str(ROOT.parent) + '/') + '(?!' + local_path + ')')
    for item in commands:
        command = item['command']
        require(not sibling_path.search(command), f'编译命令引用了相邻工程：{item["file"]}')
        # 检查相对包含目录与符号链接；不能只扫描绝对路径文本。
        words = iter(shlex.split(command))
        for word in words:
            if word in {'-I', '-isystem', '-iquote', '-include'}:
                check_path(Path(item['directory']) / next(words))
            elif word.startswith('-I') and len(word) > 2:
                check_path(Path(item['directory']) / word[2:])
    require(not sibling_path.search((BUILD / 'build.ninja').read_text()), '链接或构建规则引用了相邻工程')
    require((ROOT / 'main/boards/deepbot-s3-cam-n16r8/deepbot_s3_cam_board.cc') in sources,
            'cam 板没有参与编译')
    require(not any('78__esp-ml307' in str(p) or '78__uart-uhci' in str(p) for p in sources),
            '编译仍带入蜂窝网络实现')

    flash = load_json('flasher_args.json')
    require(flash['flash_settings'] == {'flash_mode': 'dio', 'flash_freq': '80m', 'flash_size': '16MB'},
            'Flash 模式、频率或容量不正确')
    with (ROOT / 'partitions/16m.csv').open() as f:
        partitions = {row[0].strip(): [x.strip() for x in row[1:5]]
                      for row in csv.reader(line for line in f if not line.startswith('#')) if row}
    require(partitions['ota_0'] == ['app', 'ota_0', '0x20000', '0x3f0000'], 'app 分区不符合 cam 布局')
    require(partitions['assets'] == ['data', 'spiffs', '0x800000', '8M'], 'assets 分区不符合 cam 布局')
    for segment, address, limit in (('app', '0x20000', 0x3F0000), ('assets', '0x800000', 8 << 20)):
        entry = flash[segment]
        require(entry['offset'] == address and flash['flash_files'][address] == entry['file'],
                f'{segment} 写入地址错误')
        data = (BUILD / entry['file']).read_bytes()
        require(0 < len(data) <= limit, f'{segment} 镜像为空或超限')
        if segment == 'app':
            require(data[0] == 0xE9, 'app 不是 ESP 固件镜像')
        else:
            require(b'srmodels.bin' in data and b'wn9_xiaomingtongxue_tts2' in data, '唤醒模型未打包')
        print(f'{segment}: {entry["file"]} @ {address}, {len(data):,} 字节')
    print(f'启动静态校验通过；{len(sources)} 个编译源文件均来自 mini-bot 或 ESP-IDF。')


if __name__ == '__main__':
    try:
        verify()
    except (ValueError, OSError, KeyError) as exc:
        sys.exit(f'启动静态校验失败：{exc}')
