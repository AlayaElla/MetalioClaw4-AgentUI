#!/usr/bin/env python3
"""Vectorise the IrisOLED robotic-eye bitmaps into the Agent UI expression art.

IrisOLED ships 24 hand-designed 128x64 single-colour bitmaps (eyes drawn as dark
pixels, because the SSD1306 renders inverted). Blitting them onto the 600x400
Agent UI expression surface would upscale them 4.7x and reintroduce the blocky
look the dot-matrix player already suffers from, so instead every bitmap is
traced into closed outlines, rounded with Chaikin corner cutting, simplified
with Douglas-Peucker, and normalised into the player's 48-unit logical space.
The runtime keeps its procedural
animation (gaze offset, lid squash for blinks, energy sag, dirty-rect repaint)
and only takes the silhouettes from here.

Source art: https://github.com/orji123/Irisoled (MIT, Copyright (c) 2025
Chijindu-Orji Iseh-Ntah). The generated header repeats that notice.

Usage:
  python3 scripts/trace_expression_art.py [--source DIR] [--output FILE]
"""

from __future__ import annotations

import argparse
from collections import namedtuple
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO.parent / "Irisoled" / "extras" / "eye expressions" / "bitmap"
DEFAULT_GLYPH_SOURCE = (
    REPO.parent / "Irisoled" / "extras" / "special expressions" / "bitmap"
)
DEFAULT_OUTPUT = (
    REPO / "main" / "display" / "agent_ui" / "components" / "expression_art.generated.h"
)

# Logical drawing space of expression_player.cc (kGridScale maps 48 units to the
# 56-cell grid, kPivot is its centre).
# kUnitScale in the player maps this 48-unit space onto the 600px surface, and
# (24, 24) is the centre of the 600x400 expression canvas.
LOGICAL_EXTENT = 48.0
LOGICAL_CENTER = 24.0
# Pair width in logical units. The procedural eyes span 6..42 units (450 px on
# the 600x400 surface), so 36 keeps the imported pair at the same hero size.
TARGET_PAIR_WIDTH = 36.0
# Expression whose pair width defines the shared scale.
REFERENCE_EXPRESSION = "Normal"
# Normal's own eye gap. Poses drawn with the eyes nearly touching (surprised,
# look-*) get spread to this so the pair never reads as one slab of orange.
MIN_EYE_GAP = 2.8
# How much of each pose's vertical offset survives the import (see to_logical).
VERTICAL_PLACEMENT = 0.45
MAX_POINTS_PER_EYE = 34
# Corner cuts per traced loop, then the simplification tolerance in source
# pixels. A larger tolerance trades the last bit of silhouette for fewer points
# on the device, where every point costs scanline work.
SMOOTH_ITERATIONS = 3
SIMPLIFY_EPSILON = 0.8

# The "special expressions" are whole-panel status graphics (battery, turn
# arrows, gears, warning, wordmark) rather than eye pairs. A glyph replaces the
# face for the duration of its action, so it is fitted into its own box and
# centred on both axes.
GLYPH_TARGET_WIDTH = 30.0
GLYPH_TARGET_HEIGHT = 20.0
MAX_GLYPH_SHAPES = 12
MAX_POINTS_PER_GLYPH_SHAPE = 56

# Bitmap file name -> C++ identifier.
EXPRESSIONS = [
    ("Normal", "normal"),
    ("Blink", "blink"),
    ("Blink Up", "blink_up"),
    ("Blink Down", "blink_down"),
    ("Wink Left", "wink_left"),
    ("Wink Right", "wink_right"),
    ("Happy", "happy"),
    ("Sad", "sad"),
    ("Angry", "angry"),
    ("Furious", "furious"),
    ("Excited", "excited"),
    ("Focused", "focused"),
    ("Surprised", "surprised"),
    ("Scared", "scared"),
    ("Worried", "worried"),
    ("Despair", "despair"),
    ("Disoriented", "disoriented"),
    ("Bored", "bored"),
    ("Sleepy", "sleepy"),
    ("Alert", "alert"),
    ("Look Left", "look_left"),
    ("Look Right", "look_right"),
    ("Look Up", "look_up"),
    ("Look Down", "look_down"),
]

GLYPHS = [
    ("Battery", "battery"),
    ("Battery Full", "battery_full"),
    ("Battery Low", "battery_low"),
    ("Left Signal", "left_signal"),
    ("Logo", "logo"),
    ("Mode", "mode"),
    ("Right Signal", "right_signal"),
    ("Warning", "warning"),
]


def loops_from_mask(mask: np.ndarray) -> list[list[tuple[int, int]]]:
    """Chain the directed boundary edges of every filled blob into closed loops."""
    height, width = mask.shape
    edges: dict[tuple[int, int], list[tuple[int, int]]] = {}

    def add(start: tuple[int, int], end: tuple[int, int]) -> None:
        edges.setdefault(start, []).append(end)

    for y in range(height):
        for x in range(width):
            if not mask[y, x]:
                continue
            if y == 0 or not mask[y - 1, x]:
                add((x, y), (x + 1, y))
            if x == width - 1 or not mask[y, x + 1]:
                add((x + 1, y), (x + 1, y + 1))
            if y == height - 1 or not mask[y + 1, x]:
                add((x + 1, y + 1), (x, y + 1))
            if x == 0 or not mask[y, x - 1]:
                add((x, y + 1), (x, y))

    loops: list[list[tuple[int, int]]] = []
    while edges:
        start = next(iter(edges))
        loop = [start]
        current = start
        while True:
            candidates = edges[current]
            nxt = candidates.pop()
            if not candidates:
                del edges[current]
            if nxt == start:
                break
            if nxt not in edges:
                break
            loop.append(nxt)
            current = nxt
        loops.append(loop)
    return [loop for loop in loops if len(loop) > 3]


def simplify(points: list[tuple[int, int]], epsilon: float) -> list[tuple[int, int]]:
    """Douglas-Peucker: the pixel stair-steps collapse into a handful of corners."""
    if len(points) < 3:
        return points
    arr = np.asarray(points, dtype=float)
    keep = np.zeros(len(arr), dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(arr) - 1)]
    while stack:
        first, last = stack.pop()
        if last <= first + 1:
            continue
        a, b = arr[first], arr[last]
        segment = b - a
        length = float(np.hypot(*segment)) or 1.0
        relative = arr[first + 1:last] - a
        distance = np.abs(
            relative[:, 0] * segment[1] - relative[:, 1] * segment[0]
        ) / length
        pivot = int(np.argmax(distance)) + first + 1
        if distance.max() > epsilon:
            keep[pivot] = True
            stack.append((first, pivot))
            stack.append((pivot, last))
    return [point for point, held in zip(points, keep) if held]


def drop_collinear(points: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Keep one vertex per direction change instead of every staircase pixel."""
    kept: list[tuple[int, int]] = []
    for point in points:
        while len(kept) > 1 and _cross(kept[-2], kept[-1], point) == 0:
            kept.pop()
        kept.append(point)
    while len(kept) > 2 and _cross(kept[-2], kept[-1], kept[0]) == 0:
        kept.pop()
    while len(kept) > 2 and _cross(kept[-1], kept[0], kept[1]) == 0:
        kept.pop(0)
    return kept


def _cross(previous, corner, following) -> int:
    return ((corner[0] - previous[0]) * (following[1] - corner[1])
            - (corner[1] - previous[1]) * (following[0] - corner[0]))


def smooth(points: list[tuple[int, int]], epsilon: float,
           iterations: int) -> list[tuple[float, float]]:
    """Round a pixel staircase into arcs before the point budget is spent.

    Douglas-Peucker alone reads the staircase as a handful of straight runs and
    leaves hard 45-degree corners on every rounded shape. Chaikin corner cutting
    first replaces each corner with two points a quarter of the way along its
    neighbouring edges, so the simplification then samples a curve that already
    bends instead of a polygon that only approximates one.
    """
    polygon = drop_collinear(points)
    for _ in range(iterations):
        if len(polygon) < 3:
            break
        cut: list[tuple[float, float]] = []
        for index, point in enumerate(polygon):
            following = polygon[(index + 1) % len(polygon)]
            cut.append((point[0] * 0.75 + following[0] * 0.25,
                        point[1] * 0.75 + following[1] * 0.25))
            cut.append((point[0] * 0.25 + following[0] * 0.75,
                        point[1] * 0.25 + following[1] * 0.75))
        polygon = cut
    return simplify(polygon, epsilon)


def trace_eyes(path: Path) -> list[list[tuple[int, int]]]:
    """Return the left and right eye outline of one IrisOLED bitmap."""
    mask = np.asarray(Image.open(path).convert("L"), dtype=np.uint8) < 128
    height, width = mask.shape
    eyes: list[list[tuple[int, int]]] = []
    for loop in loops_from_mask(mask):
        # The inverted background is a single region touching the frame; its
        # outer contour is not a shape, so drop it and keep the eye outlines.
        if any(x in (0, width) or y in (0, height) for x, y in loop):
            continue
        eyes.append(smooth(loop, SIMPLIFY_EPSILON, SMOOTH_ITERATIONS))
    if len(eyes) != 2:
        raise ValueError(f"{path.name}: expected 2 eyes, traced {len(eyes)}")
    for eye in eyes:
        if len(eye) > MAX_POINTS_PER_EYE:
            raise ValueError(
                f"{path.name}: eye needs {len(eye)} points, raise MAX_POINTS_PER_EYE"
            )
    return sorted(eyes, key=lambda eye: sum(p[0] for p in eye) / len(eye))


def loop_area(loop):
    total = 0.0
    for index, point in enumerate(loop):
        previous = loop[index - 1]
        total += previous[0] * point[1] - point[0] * previous[1]
    return abs(total) / 2.0


def loop_contains(loop, point):
    """Even-odd ray test."""
    x, y = point
    inside = False
    for index in range(len(loop)):
        ax, ay = loop[index - 1]
        bx, by = loop[index]
        if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
            inside = not inside
    return inside


def loop_inside(inner, outer):
    """Every vertex of `inner` has to fall inside `outer`.

    A single sample point is not enough: the battery shell and its inner edge
    share a centre, so testing one point each makes them contain each other.
    """
    return all(loop_contains(outer, point) for point in inner)


def bridge_holes(shapes, path: Path):
    """Fold every hole loop into the loop that directly contains it.

    The player unions its polygons and only applies even-odd inside a single
    one, so a hole has to share a polygon with its shell - otherwise the battery
    shell fills solid and the warning triangle loses its exclamation mark. The
    cut out to the hole and back is walked twice in opposite directions, so it
    cancels in the crossing count and leaves no seam.
    """
    parents = []
    for index, loop in enumerate(shapes):
        parent = -1
        for other, candidate in enumerate(shapes):
            if other == index or not loop_inside(loop, candidate):
                continue
            if parent < 0 or loop_area(candidate) < loop_area(shapes[parent]):
                parent = other
        parents.append(parent)

    def depth(index):
        level = 0
        seen = {index}
        while parents[index] >= 0:
            index = parents[index]
            if index in seen:
                raise ValueError(f"{path.name}: loops nest inside each other")
            seen.add(index)
            level += 1
        return level

    holes = {}
    shells = []
    for index, loop in enumerate(shapes):
        if depth(index) % 2 == 0:
            shells.append(index)
        else:
            holes.setdefault(parents[index], []).append(index)

    bridged = []
    for shell in shells:
        loop = list(shapes[shell])
        for hole in holes.get(shell, ()):
            loop += [shapes[shell][0]] + list(shapes[hole]) + [shapes[hole][0]]
        bridged.append(loop)
    if not bridged:
        raise ValueError(f"{path.name}: no outer loops after bridging")
    return bridged


def trace_glyph(path: Path) -> list[list[tuple[int, int]]]:
    """Every outline of one special-expression bitmap, holes bridged into their
    shell.

    The source art is 1-bit, so a shape's interior is whatever the even-odd
    nesting says it is: the battery shell and its inner edge, the gear hubs, the
    warning triangle's exclamation mark.
    """
    mask = np.asarray(Image.open(path).convert("L"), dtype=np.uint8) < 128
    height, width = mask.shape
    shapes = []
    for loop in loops_from_mask(mask):
        if any(x in (0, width) or y in (0, height) for x, y in loop):
            continue
        simplified = smooth(loop, SIMPLIFY_EPSILON, SMOOTH_ITERATIONS)
        if len(simplified) >= 3:
            shapes.append(simplified)
    if not shapes:
        raise ValueError(f"{path.name}: traced no shapes")
    shapes = bridge_holes(shapes, path)
    if len(shapes) > MAX_GLYPH_SHAPES:
        raise ValueError(
            f"{path.name}: {len(shapes)} shapes, raise MAX_GLYPH_SHAPES"
        )
    for shape in shapes:
        if len(shape) > MAX_POINTS_PER_GLYPH_SHAPE:
            raise ValueError(
                f"{path.name}: shape needs {len(shape)} points, raise "
                f"MAX_POINTS_PER_GLYPH_SHAPE"
            )
    return shapes


def derive_glyph_scale(glyphs: list[list[list[tuple[int, int]]]]) -> float:
    """One shared scale so the glyphs keep their relative sizes."""
    scale = float("inf")
    for shapes in glyphs:
        xs = [p[0] for shape in shapes for p in shape]
        ys = [p[1] for shape in shapes for p in shape]
        scale = min(scale,
                    GLYPH_TARGET_WIDTH / (max(xs) - min(xs)),
                    GLYPH_TARGET_HEIGHT / (max(ys) - min(ys)))
    return scale


def derive_scale(all_eyes: list[list[list[tuple[int, int]]]],
                 reference: int) -> tuple[float, float]:
    """One shared scale plus the vertical register every pose keeps.

    The horizontal centre is per expression instead: the IrisOLED look-* poses
    bake the gaze offset into the pair position, and the player already drives
    that procedurally, so re-centring keeps every pose the same size on screen.
    """
    reference_xs = [p[0] for eye in all_eyes[reference] for p in eye]
    scale = TARGET_PAIR_WIDTH / (max(reference_xs) - min(reference_xs))
    all_ys = [p[1] for eyes in all_eyes for eye in eyes for p in eye]
    return scale, (min(all_ys) + max(all_ys)) / 2.0


def to_logical(point, scale, center_x, center_y, pose_center_y, placement):
    """Map one source pixel into the player's logical space.

    The source bitmaps lift happy eyes to the top of the panel and drop sad ones
    to the bottom. On a surface that shows only eyes the full offset reads as a
    layout bug, so the vertical placement is damped towards the shared eye line
    while the silhouette - which carries the expression - keeps its size.
    """
    x = (point[0] - center_x) * scale + LOGICAL_CENTER
    y = ((point[1] - pose_center_y) +
         (pose_center_y - center_y) * placement) * scale + LOGICAL_CENTER
    return x, y


def format_header(records, scale, center_y, glyph_records,
                  glyph_scale) -> str:
    lines = [
        "#pragma once",
        "",
        "// Generated by scripts/trace_expression_art.py - do not edit by hand.",
        "//",
        "// Silhouettes traced from the IrisOLED robotic-eye bitmaps.",
        "// IrisOLED is MIT licensed:",
        "//   Copyright (c) 2025 Chijindu-Orji Iseh-Ntah",
        "//   https://github.com/orji123/Irisoled",
        "//",
        "// Coordinates are in the expression player's 48-unit logical space",
        f"// (reference pair width {TARGET_PAIR_WIDTH:.0f} units, every pose",
        "// re-centred on x, spread to a minimum eye gap, and sharing one",
        f"// vertical register; source scale {scale:.6f} px->units,",
        f"// source centre y {center_y:.3f} px).",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace agent_ui::expression_art {",
        "",
        "inline constexpr int kLogicalExtent = 48;",
        "inline constexpr float kLogicalCenter = 24.0f;",
        f"inline constexpr int kMaxPointsPerEye = {MAX_POINTS_PER_EYE};",
        f"inline constexpr float kMinEyeGap = {MIN_EYE_GAP:.3f}f;",
        "",
        "struct Eye {",
        "    uint8_t count;",
        "    float points[kMaxPointsPerEye][2];",
        "    float center[2];",
        "    float radius[2];",
        "};",
        "",
        "struct Expression {",
        "    Eye left;",
        "    Eye right;",
        "};",
        "",
        "enum Name : uint8_t {",
    ]
    for _, identifier in EXPRESSIONS:
        lines.append(f"    {identifier},")
    lines += [
        "    kCount",
        "};",
        "",
        "inline constexpr Expression kExpressions[kCount] = {",
    ]
    for (source_name, identifier), eyes in records:
        lines.append(f"    // {identifier} <- {source_name}.bmp")
        lines.append("    {")
        for eye in eyes:
            points = ", ".join(
                f"{{{x:.3f}f, {y:.3f}f}}" for x, y in eye.formatted
            )
            lines.append("        {")
            lines.append(f"            {len(eye.points)},")
            lines.append(f"            {{{points}}},")
            lines.append(
                f"            {{{eye.center[0]:.3f}f, {eye.center[1]:.3f}f}},"
            )
            lines.append(
                f"            {{{eye.radius[0]:.3f}f, {eye.radius[1]:.3f}f}},"
            )
            lines.append("        },")
        lines.append("    },")
    lines += [
        "};",
        "",
        "// Special-expression glyphs traced from the IrisOLED status bitmaps.",
        "// A glyph replaces the whole face while its action plays, so it carries",
        "// a flat polygon list rather than an eye pair. The player unions its",
        "// polygons and only applies even-odd inside one, so every hole loop is",
        "// already bridged into its shell here. Each glyph is fitted into a",
        f"// {GLYPH_TARGET_WIDTH:.0f}x{GLYPH_TARGET_HEIGHT:.0f} unit box at one"
        " shared scale and centred on both axes.",
        f"inline constexpr int kMaxGlyphShapes = {MAX_GLYPH_SHAPES};",
        f"inline constexpr int kMaxPointsPerGlyphShape = "
        f"{MAX_POINTS_PER_GLYPH_SHAPE};",
        "",
        "struct GlyphPoint {",
        "    float x;",
        "    float y;",
        "};",
        "",
        "struct GlyphShape {",
        "    uint16_t first;",
        "    uint16_t count;",
        "};",
        "",
        "struct Glyph {",
        "    uint8_t shape_count;",
        "    GlyphShape shapes[kMaxGlyphShapes];",
        "    float center[2];",
        "    float radius[2];",
        "};",
        "",
        "enum GlyphName : uint8_t {",
    ]
    for _, identifier in GLYPHS:
        lines.append(f"    {identifier},")
    lines += [
        "    kGlyphCount",
        "};",
        "",
        "inline constexpr GlyphPoint kGlyphPoints[] = {",
    ]
    for _, glyph in glyph_records:
        for point in glyph.points:
            lines.append(f"    {{{point[0]:.3f}f, {point[1]:.3f}f}},")
    lines += [
        "};",
        "",
        f"inline constexpr float kGlyphSourceScale = {glyph_scale:.6f}f;  "
        "// source px -> logical units",
        "",
        "inline constexpr Glyph kGlyphs[kGlyphCount] = {",
    ]
    for (source_name, identifier), glyph in glyph_records:
        spans = ", ".join(f"{{{shape.first}, {shape.count}}}"
                          for shape in glyph.shapes)
        lines.append(f"    // {identifier} <- {source_name}.bmp")
        lines.append("    {")
        lines.append(f"        {len(glyph.shapes)},")
        lines.append(f"        {{{spans}}},")
        lines.append(
            f"        {{{glyph.center[0]:.3f}f, {glyph.center[1]:.3f}f}},"
        )
        lines.append(
            f"        {{{glyph.radius[0]:.3f}f, {glyph.radius[1]:.3f}f}},"
        )
        lines.append("    },")
    lines += [
        "};",
        "",
        "}  // namespace agent_ui::expression_art",
        "",
    ]
    return "\n".join(lines)


ShapeRef = namedtuple("ShapeRef", "first count")


class EyeRecord:
    def __init__(self, points):
        self.points = points
        self.formatted = points
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        self.center = ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
        self.radius = ((max(xs) - min(xs)) / 2.0, (max(ys) - min(ys)) / 2.0)


class GlyphRecord:
    """One glyph: the [first, count) of each polygon inside the shared pool."""

    def __init__(self, shapes, base):
        self.shapes = []
        first = base
        for shape in shapes:
            self.shapes.append(ShapeRef(first, len(shape)))
            first += len(shape)
        self.points = [point for shape in shapes for point in shape]
        xs = [p[0] for p in self.points]
        ys = [p[1] for p in self.points]
        self.center = ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
        self.radius = ((max(xs) - min(xs)) / 2.0, (max(ys) - min(ys)) / 2.0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--glyph-source", type=Path, default=DEFAULT_GLYPH_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    raw = []
    for source_name, identifier in EXPRESSIONS:
        path = args.source / f"{source_name}.bmp"
        if not path.is_file():
            parser.error(f"missing source bitmap: {path}")
        raw.append((source_name, identifier, trace_eyes(path)))

    glyph_raw = []
    for source_name, identifier in GLYPHS:
        path = args.glyph_source / f"{source_name}.bmp"
        if not path.is_file():
            parser.error(f"missing source bitmap: {path}")
        glyph_raw.append((source_name, identifier, trace_glyph(path)))

    reference = [name for name, _ in EXPRESSIONS].index(REFERENCE_EXPRESSION)
    scale, center_y = derive_scale([entry[2] for entry in raw], reference)

    records = []
    for source_name, identifier, eyes in raw:
        xs = [p[0] for eye in eyes for p in eye]
        ys = [p[1] for eye in eyes for p in eye]
        center_x = (min(xs) + max(xs)) / 2.0
        pose_center_y = (min(ys) + max(ys)) / 2.0
        pair = [[to_logical(p, scale, center_x, center_y, pose_center_y,
                            VERTICAL_PLACEMENT) for p in eye] for eye in eyes]
        gap = min(x for x, _ in pair[1]) - max(x for x, _ in pair[0])
        if gap < MIN_EYE_GAP:
            spread = (MIN_EYE_GAP - gap) / 2.0
            pair[0] = [(x - spread, y) for x, y in pair[0]]
            pair[1] = [(x + spread, y) for x, y in pair[1]]
        records.append(((source_name, identifier),
                        [EyeRecord(eye) for eye in pair]))

    glyph_scale = derive_glyph_scale([entry[2] for entry in glyph_raw])
    glyph_records = []
    pool = 0
    for source_name, identifier, shapes in glyph_raw:
        xs = [p[0] for shape in shapes for p in shape]
        ys = [p[1] for shape in shapes for p in shape]
        center_x = (min(xs) + max(xs)) / 2.0
        center_y = (min(ys) + max(ys)) / 2.0
        logical = [[((p[0] - center_x) * glyph_scale + LOGICAL_CENTER,
                     (p[1] - center_y) * glyph_scale + LOGICAL_CENTER)
                    for p in shape] for shape in shapes]
        glyph_records.append(((source_name, identifier),
                              GlyphRecord(logical, pool)))
        pool += sum(len(shape) for shape in logical)

    args.output.write_text(
        format_header(records, scale, center_y, glyph_records, glyph_scale),
        encoding="utf-8")
    total = sum(len(p) for _, eyes in records for p in [e.points for e in eyes])
    glyph_points = sum(len(glyph.points) for _, glyph in glyph_records)
    print(f"{len(records)} expressions + {len(glyph_records)} glyphs -> "
          f"{args.output}")
    print(f"total points {total}, scale {scale:.6f}, "
          f"source centre y {center_y:.3f}")
    print(f"glyph points {glyph_points}, glyph scale {glyph_scale:.6f}")


if __name__ == "__main__":
    main()
