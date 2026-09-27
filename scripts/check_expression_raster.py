#!/usr/bin/env python3
"""Host-side check of the expression rasteriser.

Mirrors Emit()/AccumulateRow() in expression_player.cc against the generated
art header so geometry, anti-aliasing and symbol placement can be inspected
without flashing. Keep in sync with the C++ constants.
"""

from __future__ import annotations

import argparse
import math
import re
import struct
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
HEADER = (
    REPO / "main" / "display" / "agent_ui" / "components" / "expression_art.generated.h"
)

LOGICAL_EXTENT = 48
LOGICAL_CENTER = 24.0
UNIT = 600.0 / LOGICAL_EXTENT
CENTER_X, CENTER_Y = 300.0, 200.0
WIDTH, HEIGHT = 600, 400
SUBS = 4
COVER = 256 // SUBS
MIN_GAP = float(re.search(r"kMinEyeGap = ([\d.]+)f",
                          HEADER.read_text()).group(1))


def load_art():
    text = HEADER.read_text()
    body = text.split("kExpressions[kCount] = {", 1)[1].split("};", 1)[0]
    names = re.findall(r"// (\w+) <-", body)
    blocks = re.findall(
        r"(\d+),\s*(\{\{.*?\}\}),\s*\{([-\d., f]+)\},", body, re.S)
    parsed = []
    for count, points, center in blocks:
        coords = re.findall(r"\{([-\d.]+)f, ([-\d.]+)f\}", points)
        parsed.append({
            "points": [(float(x), float(y)) for x, y in coords],
            "center": tuple(float(v.rstrip("f").strip())
                            for v in center.split(",")),
            "radius": ((max(float(x) for x, _ in coords) - min(float(x) for x, _ in coords)) / 2,
                       (max(float(y) for _, y in coords) - min(float(y) for _, y in coords)) / 2),
        })
    return {name: (parsed[2 * i], parsed[2 * i + 1]) for i, name in enumerate(names)}


def load_glyphs():
    """Mirror of the generated glyph tables: spans into one flat point pool."""
    text = HEADER.read_text()
    points_body = text.split("kGlyphPoints[] = {", 1)[1].split("};", 1)[0]
    points = [(float(x), float(y)) for x, y in
              re.findall(r"\{([-\d.]+)f, ([-\d.]+)f\}", points_body)]
    body = text.split("kGlyphs[kGlyphCount] = {", 1)[1].split("\n};", 1)[0]
    blocks = re.findall(
        r"// (\w+) <-.*?(\d+),\s*(\{\{.*?\}\}),\s*\{([-\d., f]+)\},",
        body, re.S)
    glyphs = {}
    for name, shape_count, spans, center in blocks:
        parsed_spans = [(int(first), int(count)) for first, count in
                        re.findall(r"\{(\d+), (\d+)\}", spans)]
        shapes = [points[first:first + count] for first, count in parsed_spans]
        glyphs[name] = {
            "shapes": shapes,
            "center": tuple(float(v.rstrip("f").strip())
                            for v in center.split(",")),
        }
    return glyphs


class Transform:
    def __init__(self, rotation=0.0, tx=0.0, ty=0.0, energy_scale=1.0, energy_sag=0.0):
        rad = math.radians(rotation)
        self.cos, self.sin = math.cos(rad), math.sin(rad)
        self.tx, self.ty = tx, ty
        self.energy_scale, self.energy_sag = energy_scale, energy_sag

    def apply(self, ux, uy):
        dx = (ux - LOGICAL_CENTER) * UNIT
        dy = ((uy - LOGICAL_CENTER) * self.energy_scale + self.energy_sag) * UNIT
        return (CENTER_X + dx * self.cos - dy * self.sin + self.tx * UNIT,
                CENTER_Y + dx * self.sin + dy * self.cos + self.ty * UNIT)


def eye_polygon(eye, state, transform):
    cx, cy = eye["center"]
    sx, sy, ox, oy = state
    return [transform.apply(cx + (x - cx) * sx + ox, cy + (y - cy) * sy + oy)
            for x, y in eye["points"]]


def keep_apart(left_eye, left_state, right_eye, right_state):
    """Mirror of KeepEyesApart(): growth about each eye's centre eats the gap."""
    min_gap = MIN_GAP
    left_inner = left_eye["center"][0] + left_state[2] + left_eye["radius"][0] * left_state[0]
    right_inner = right_eye["center"][0] + right_state[2] - right_eye["radius"][0] * right_state[0]
    deficit = min_gap - (right_inner - left_inner)
    if deficit <= 0:
        return left_state, right_state
    return (left_state[0], left_state[1], left_state[2] - deficit / 2, left_state[3]), \
           (right_state[0], right_state[1], right_state[2] + deficit / 2, right_state[3])


def glyph_polygons(glyph, transform, scale=1.0):
    """Mirror of AddGlyph(): every loop scaled about the glyph's own centre."""
    cx, cy = glyph["center"]
    return [[transform.apply(cx + (x - cx) * scale, cy + (y - cy) * scale)
             for x, y in shape] for shape in glyph["shapes"]]


def ellipse(cx, cy, rx, ry, segments=24):
    return [(cx + math.cos(i / segments * 2 * math.pi) * rx,
             cy + math.sin(i / segments * 2 * math.pi) * ry) for i in range(segments)]


def ring(cx, cy, radius, thickness, steps=19):
    def arc(scale, points):
        return [(cx + math.cos(i / steps * 2 * math.pi) * scale,
                 cy + math.sin(i / steps * 2 * math.pi) * scale)
                for i in range(points)]
    outer = arc(radius, steps + 1)
    inner_r = max(0.4, radius - thickness)
    inner = [(cx + math.cos((steps - i) / steps * 2 * math.pi) * inner_r,
              cy + math.sin((steps - i) / steps * 2 * math.pi) * inner_r)
             for i in range(steps + 1)]
    return outer + inner


def diamond(transform, ux, uy, radius):
    """AddDiamond(): the radius is in logical units, like every other symbol."""
    return [transform.apply(ux, uy - radius), transform.apply(ux + radius, uy),
            transform.apply(ux, uy + radius), transform.apply(ux - radius, uy)]


def segment(x1, y1, x2, y2, thickness):
    dx, dy = x2 - x1, y2 - y1
    length = max(0.01, math.hypot(dx, dy))
    half = thickness * UNIT
    nx, ny = -dy / length * half, dx / length * half
    return [(x1 + nx, y1 + ny), (x2 + nx, y2 + ny), (x2 - nx, y2 - ny), (x1 - nx, y1 - ny)]


def add_span(coverage, x0, x1, start, end):
    a, b = max(start, x0), min(end, x1)
    if b <= a:
        return
    first, last = math.floor(a), math.ceil(b) - 1
    if first > last:
        return
    if first == last:
        coverage[first - x0] += (b - a) * COVER + 0.5
        return
    coverage[first - x0] += (first + 1 - a) * COVER + 0.5
    for x in range(first + 1, last):
        coverage[x - x0] += COVER
    coverage[last - x0] += (b - last) * COVER + 0.5


def rasterise(polygons, alpha=255):
    buffer = bytearray(WIDTH * HEIGHT)
    if not polygons:
        return buffer
    xs = [x for poly in polygons for x, _ in poly]
    ys = [y for poly in polygons for _, y in poly]
    x0, x1 = max(0, int(min(xs))), min(WIDTH - 1, int(max(xs)))
    y0, y1 = max(0, int(min(ys))), min(HEIGHT - 1, int(max(ys)))
    for y in range(y0, y1 + 1):
        coverage = [0] * (x1 - x0 + 1)
        for poly in polygons:
            points = poly
            top = min(p for _, p in points)
            bottom = max(p for _, p in points)
            if y + 1 < top or y > bottom:
                continue
            for sub in range(SUBS):
                scan = y + (sub + 0.5) / SUBS
                crossings = []
                n = len(points)
                for i in range(n):
                    ax, ay = points[i - 1]
                    bx, by = points[i]
                    if (ay <= scan < by) or (by <= scan < ay):
                        crossings.append(ax + (scan - ay) * (bx - ax) / (by - ay))
                crossings.sort()
                for i in range(0, len(crossings) - 1, 2):
                    add_span(coverage, x0, x1, crossings[i], crossings[i + 1])
        row = y * WIDTH
        for x in range(x0, x1 + 1):
            buffer[row + x] = min(255, int(coverage[x - x0])) * alpha // 255
    return buffer


def show(buffers):
    combined = bytearray(WIDTH * HEIGHT)
    for buffer in buffers:
        for i, value in enumerate(buffer):
            combined[i] = max(combined[i], value)
    return Image.frombytes("L", (WIDTH, HEIGHT), bytes(combined))


POSES = {
    "idle": [("normal", (1, 1, 0, 0)), ("normal", (1, 1, 0, 0))],
    "blink": [("normal", (1, 0.18, 0, 0)), ("normal", (1, 0.18, 0, 0))],
    "happy": [("happy", (1, 1.1, 0, -0.8)), ("happy", (1, 1.1, 0, -0.8))],
    "listening": [("focused", (1.12, 1.12, 0.85, -0.3)), ("focused", (0.94, 0.94, -0.85, 0.3))],
    "sleep+zzz": [("blink", (1, 1, 0, 0.3)), ("blink", (1, 1, 0, 0.3))],
    "charging": [("normal", (1.10, 1.10, 0, 0)), ("normal", (1.10, 1.10, 0, 0))],
    "dizzy+rings": [("disoriented", (0.7, 0.7, 1.2, 0)), ("disoriented", (0.7, 0.7, -1.2, 0))],
    "tired sag": [("normal", (1, 1, 0, 0)), ("normal", (1, 1, 0, 0))],
    "gaze": [("normal", (1, 1, 3.2, -2.7)), ("normal", (1, 1, 3.2, -2.7))],
    "complete": [("happy", (1, 1, 0, -1)), ("happy", (1, 1, 0, -1))],
    "surprised": [("surprised", (0.94, 1.12, 0, 0)), ("surprised", (0.94, 1.12, 0, 0))],
    "wink_left": [("wink_left", (1, 1, 0, 0)), ("wink_left", (1, 1, 0, 0))],
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("/tmp/raster_check.png"))
    parser.add_argument("--cols", type=int, default=4)
    parser.add_argument("--scale", type=float, default=0.5)
    parser.add_argument("--all", action="store_true",
                        help="render every imported pose instead of the action sheet")
    parser.add_argument("--glyphs", action="store_true",
                        help="render the whole-panel status graphics instead")
    parser.add_argument("--labels", action="store_true",
                        help="reserve a caption strip under every cell")
    args = parser.parse_args()

    art = load_art()
    glyphs = load_glyphs()
    # Read the rasteriser's own caps out of the player header: AddPolygon() drops
    # a shape that does not fit rather than growing the buffer.
    player = (HEADER.parent / "expression_player.h").read_text()
    max_shapes = int(re.search(r"kMaxShapes = (\d+)", player).group(1))
    max_points = int(re.search(r"kMaxShapePoints = (\d+)", player).group(1))
    for name, glyph in glyphs.items():
        if len(glyph["shapes"]) > max_shapes:
            raise SystemExit(f"{name}: {len(glyph['shapes'])} shapes, "
                             f"rasteriser holds {max_shapes}")
        too_long = [len(s) for s in glyph["shapes"] if len(s) > max_points]
        if too_long:
            raise SystemExit(f"{name}: shape needs {max(too_long)} points, "
                             f"rasteriser holds {max_points}")
    poses = POSES
    if args.all:
        # One neutral cell per imported pose: the whole vocabulary on screen.
        poses = {name: ((name, (1, 1, 0, 0)), (name, (1, 1, 0, 0))) for name in art}
    cells = []
    if args.glyphs:
        cells = [(name, rasterise(glyph_polygons(glyph, Transform())))
                 for name, glyph in glyphs.items()]
    for label, (left, right) in ([] if args.glyphs else poses.items()):
        transform = Transform(
            energy_scale=0.68 if "sag" in label else 1.0,
            energy_sag=1.15 if "sag" in label else 0.0,
        )
        left_eye, left_state = art[left[0]][0], left[1]
        right_eye, right_state = art[right[0]][1], right[1]
        left_state, right_state = keep_apart(left_eye, left_state,
                                             right_eye, right_state)
        polys = [eye_polygon(left_eye, left_state, transform),
                 eye_polygon(right_eye, right_state, transform)]
        if "zzz" in label:
            # Mid-climb frame of Emit()'s Action::Sleep Z, which grows as it rises.
            phase = 0.6
            half = 1.1 + phase * 1.7
            cx, cy = 41.5 + phase * 3.0, 18.0 - phase * 8.0
            thickness = 0.4 + phase * 0.3
            polys += [
                segment(*transform.apply(cx - half, cy - half),
                        *transform.apply(cx + half, cy - half), thickness),
                segment(*transform.apply(cx + half, cy - half),
                        *transform.apply(cx - half, cy + half), thickness),
                segment(*transform.apply(cx - half, cy + half),
                        *transform.apply(cx + half, cy + half), thickness),
            ]
        if "charging" in label:
            polys += [
                segment(*transform.apply(46.0, 9.0), *transform.apply(43.2, 13.2), 0.6),
                segment(*transform.apply(43.2, 13.2), *transform.apply(46.8, 13.8), 0.6),
                segment(*transform.apply(46.8, 13.8), *transform.apply(43.6, 18.0), 0.6),
            ]
        if "rings" in label:
            for eye, state in ((left_eye, left_state), (right_eye, right_state)):
                cx = eye["center"][0] + state[2]
                cy = eye["center"][1] + state[3]
                radius = max(eye["radius"]) * state[0] + 1.6
                polys.append(ring(*transform.apply(cx, cy), radius=radius * UNIT,
                                  thickness=1.2 * UNIT))
                polys.append(ellipse(*transform.apply(cx + 4.2, cy), 1.3 * UNIT, 1.3 * UNIT))
        if "complete" in label:
            polys.append(diamond(transform, 44.0, 12.5, 3.0))
        cells.append((label, rasterise(polys)))

    scale = args.scale
    cw, ch = int(WIDTH * scale), int(HEIGHT * scale)
    label_h = 22 if args.labels else 0
    cols = args.cols
    rows = (len(cells) + cols - 1) // cols
    sheet = Image.new("RGB", (cw * cols, (ch + label_h) * rows), (10, 12, 16))
    draw = ImageDraw.Draw(sheet) if label_h else None
    for index, (label, buffer) in enumerate(cells):
        image = Image.frombytes("L", (WIDTH, HEIGHT), bytes(buffer)).resize((cw, ch), Image.LANCZOS)
        tint = Image.merge("RGB", (
            image.point(lambda v: v),
            image.point(lambda v: v // 2),
            image.point(lambda v: v // 3),
        ))
        col, row = index % cols, index // cols
        base = Image.new("RGB", (cw, ch), (10, 12, 16))
        base.paste(tint, (0, 0), image)
        sheet.paste(base, (col * cw, row * (ch + label_h)))
        if draw is not None:
            draw.text((col * cw + 6, row * (ch + label_h) + ch + 4), label,
                      fill=(190, 195, 205))
    sheet.save(args.out)
    print(f"{len(cells)} cells -> {args.out}")


if __name__ == "__main__":
    main()
