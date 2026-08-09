#!/usr/bin/env python3
"""Generate the Agent UI LVGL font assets.

The browser demo uses Microsoft YaHei UI first, so the firmware uses the same
regular and bold faces. The large bold AI conversation font includes GB2312;
less critical bold faces stay limited to UI copy so the application remains
inside the flash partition.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from fontTools.ttLib import TTCollection


ASCII_RANGE = "0x20-0x7e"
STRING_LITERAL = re.compile(r'"((?:\\.|[^"\\])*)"')
DISPLAY_TEXT = """
0123456789:：%+-./×
Agent System Codex AI Wi-Fi Bluetooth ESP32-P4
主页电话文件相机设置常规网络蓝牙语言关于启动待机电源
我在随时可以开始今天想先完成哪件事有什么想法告诉我
需要帮你处理什么新的任务从一句话开始准备好了我们开始吧
有问题就问我今天也一起做点有意思的事吧
正在启动系统加载服务连接设备准备界面即将完成
呼叫挂断拨号中停止任务结束录音按住说话拍照
"""
# These settings labels are rendered with the bold UI fonts. Keep their
# coverage explicit so a wording change or stale source scan cannot silently
# fall back to the thinner regular face.
REQUIRED_UI_SYMBOLS = "记麦"


def gb2312_characters() -> str:
    characters: set[str] = set()
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                characters.add(bytes((lead, trail)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(sorted(characters, key=ord))


def collect_literal_characters(paths: list[Path]) -> str:
    characters: set[str] = set()
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for match in STRING_LITERAL.finditer(text):
            for character in match.group(1):
                if ord(character) > 0x7E:
                    characters.add(character)
    return "".join(sorted(characters, key=ord))


def extract_collection_face(source: Path, index: int, output: Path) -> Path:
    if source.suffix.lower() not in {".ttc", ".otc"}:
        return source
    collection = TTCollection(source)
    if index < 0 or index >= len(collection.fonts):
        raise ValueError(f"font collection index {index} is invalid for {source}")
    collection.fonts[index].save(output)
    return output


def run_converter(
    converter: str,
    font: Path,
    output: Path,
    name: str,
    size: int,
    symbols: str,
    bpp: int = 4,
    compress: bool = False,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        converter,
        "--yes",
        "lv_font_conv",
        "--font",
        str(font),
        "--format",
        "lvgl",
        "--lv-include",
        "lvgl.h",
        "--lv-font-name",
        name,
        "--bpp",
        str(bpp),
        "--size",
        str(size),
        "--range",
        ASCII_RANGE,
        "--symbols",
        symbols,
        "--no-prefilter",
        "--no-kerning",
        "--output",
        str(output),
    ]
    if not compress:
        command.append("--no-compress")
    subprocess.run(command, check=True)
    print(f"{name}: {output.stat().st_size} bytes")


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir.parent
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--regular-font",
        type=Path,
        default=Path(r"C:\Windows\Fonts\msyh.ttc"),
    )
    parser.add_argument(
        "--bold-font",
        type=Path,
        default=Path(r"C:\Windows\Fonts\msyhbd.ttc"),
    )
    parser.add_argument(
        "--collection-index",
        type=int,
        default=1,
        help="TTC face index; Microsoft YaHei UI is index 1",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=project_dir / "main" / "display" / "font",
    )
    args = parser.parse_args()
    for font in (args.regular_font, args.bold_font):
        if not font.is_file():
            parser.error(f"font not found: {font}")

    source_paths = list(
        (project_dir / "main" / "display" / "agent_ui").rglob("*.cc")
    )
    source_paths.append(project_dir / "main" / "i18n" / "i18n_strings_gen.h")
    ui_symbols = "".join(
        sorted(
            set(collect_literal_characters(source_paths) + REQUIRED_UI_SYMBOLS),
            key=ord,
        )
    )
    body_symbols = "".join(
        sorted(set(ui_symbols + gb2312_characters()), key=ord)
    )
    display_symbols = "".join(
        sorted({character for character in DISPLAY_TEXT if ord(character) > 0x7E}, key=ord)
    )

    converter = shutil.which("npx.cmd") or shutil.which("npx")
    if converter is None:
        parser.error("npx was not found; install Node.js before generating fonts")

    with tempfile.TemporaryDirectory(prefix="agent-fonts-") as temp_dir:
        temp = Path(temp_dir)
        try:
            regular_font = extract_collection_face(
                args.regular_font, args.collection_index, temp / "regular.ttf"
            )
            bold_font = extract_collection_face(
                args.bold_font, args.collection_index, temp / "bold.ttf"
            )
        except ValueError as error:
            parser.error(str(error))

        outputs = (
            (regular_font, "font_agent_small_18", 18, body_symbols, 4, False),
            (regular_font, "font_agent_medium_28", 28, body_symbols, 4, False),
            (regular_font, "font_agent_large_56", 56, display_symbols, 4, False),
            (bold_font, "font_agent_small_bold_18", 18, ui_symbols, 4, False),
            (bold_font, "font_agent_medium_bold_28", 28, ui_symbols, 4, False),
            (bold_font, "font_agent_large_bold_56", 56, body_symbols, 2, True),
            # Home carousel text is authored at its largest on-screen size.
            # LVGL then only scales it down during carousel motion, avoiding
            # the jagged 1.25x enlargement of the 56/28 px bitmap fonts.
            (bold_font, "font_agent_home_name_bold_35", 35,
             "0123456789Codex电话文件相机设置", 4, False),
            (bold_font, "font_agent_home_number_bold_70", 70,
             "0123456789", 4, False),
        )
        for font, name, size, symbols, bpp, compress in outputs:
            run_converter(
                converter,
                font,
                args.output / f"{name}.c",
                name,
                size,
                symbols,
                bpp,
                compress,
            )


if __name__ == "__main__":
    main()
