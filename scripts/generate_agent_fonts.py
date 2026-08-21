#!/usr/bin/env python3
"""Generate the Agent UI LVGL font assets.

The browser demo uses Microsoft YaHei UI first, so the firmware uses the same
regular and bold faces. Regular UI text and all bold body faces include GB2312.
Shared Segoe UI Emoji fallback fonts provide monochrome emoji at each UI text
size without duplicating the same glyph bitmaps for both weights.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from fontTools.ttLib import TTCollection, TTFont


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


def collect_font_characters(source: Path) -> str:
    font = TTFont(source)
    cmap = font.getBestCmap() or {}
    return "".join(chr(codepoint) for codepoint in sorted(cmap) if codepoint > 0x7E)


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
    use_color_info: bool = False,
    include_ascii: bool = True,
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
    ]
    if include_ascii:
        command.extend(("--range", ASCII_RANGE))
    command.extend((
        "--symbols", symbols,
        "--no-prefilter",
        "--no-kerning",
        "--output", str(output),
    ))
    if not compress:
        command.append("--no-compress")
    if use_color_info:
        command.append("--use-color-info")
    subprocess.run(command, check=True)
    output.write_bytes(output.read_bytes().rstrip(b"\r\n") + b"\n")
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
        "--emoji-font",
        type=Path,
        default=Path(r"C:\Windows\Fonts\seguiemj.ttf"),
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
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="FONT_NAME",
        help="generate only the named font; may be specified more than once",
    )
    args = parser.parse_args()
    for font in (args.regular_font, args.bold_font, args.emoji_font):
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
    emoji_symbols = collect_font_characters(args.emoji_font)

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
            (regular_font, "font_agent_small_18", 18, body_symbols, 2, True, False),
            (regular_font, "font_agent_medium_28", 28, body_symbols, 2, True, False),
            (regular_font, "font_agent_large_56", 56, display_symbols, 4, False, False),
            (bold_font, "font_agent_small_bold_18", 18, body_symbols, 2, True, False),
            # MediumBold renders dynamic values such as external App names, so
            # it needs the complete body character set. Two-bit compressed
            # antialiasing keeps the expanded font within the firmware budget
            # without changing its face, weight, or pixel size.
            (bold_font, "font_agent_medium_bold_28", 28, body_symbols, 2, True, False),
            (bold_font, "font_agent_large_bold_56", 56, body_symbols, 2, True, False),
            # Home carousel text is authored at its largest on-screen size.
            # LVGL then only scales it down during carousel motion, avoiding
            # the jagged 1.25x enlargement of the 56/28 px bitmap fonts.
            (bold_font, "font_agent_home_name_bold_35", 35,
             "0123456789Codex电话文件相机设置", 4, False, False),
            (bold_font, "font_agent_home_number_bold_70", 70,
             "0123456789", 4, False, False),
            (args.emoji_font, "font_agent_emoji_18", 18, emoji_symbols, 3, True, True),
            (args.emoji_font, "font_agent_emoji_28", 28, emoji_symbols, 3, True, True),
            (args.emoji_font, "font_agent_emoji_56", 56, emoji_symbols, 2, True, True),
        )
        output_names = {output[1] for output in outputs}
        unknown_names = set(args.only) - output_names
        if unknown_names:
            parser.error(f"unknown font name(s): {', '.join(sorted(unknown_names))}")
        for font, name, size, symbols, bpp, compress, use_color_info in outputs:
            if args.only and name not in args.only:
                continue
            run_converter(
                converter,
                font,
                args.output / f"{name}.c",
                name,
                size,
                symbols,
                bpp,
                compress,
                use_color_info,
                not use_color_info,
            )


if __name__ == "__main__":
    main()
