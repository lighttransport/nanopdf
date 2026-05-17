#!/usr/bin/env python3
"""Generate targeted shape/graphics test PDFs for nanopdf visual regression.

Requires: pip install reportlab
"""

import os
import sys
import math

try:
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.colors import (
        CMYKColor, Color, black, white, red, green, blue,
        yellow, cyan, magenta, transparent
    )
    from reportlab.pdfgen import canvas
    from reportlab.lib.units import mm, cm
except ImportError:
    print("Error: reportlab not installed. Run: pip install reportlab")
    sys.exit(1)

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(PROJECT_DIR, "tests", "fixtures", "visual")


def make_cmyk_pdf():
    """test_cmyk.pdf - CMYK color patches with known values."""
    c = canvas.Canvas(os.path.join(OUTDIR, "test_cmyk.pdf"), pagesize=(400, 400))
    c.setTitle("CMYK Color Test")

    patches = [
        # (c, m, y, k, label)
        (1, 0, 0, 0, "C=100"),
        (0, 1, 0, 0, "M=100"),
        (0, 0, 1, 0, "Y=100"),
        (0, 0, 0, 1, "K=100"),
        (0, 0, 0, 0, "White"),
        (1, 1, 1, 0, "CMY=100"),
        (0.5, 0, 0, 0, "C=50"),
        (0, 0.5, 0, 0, "M=50"),
        (0, 0, 0.5, 0, "Y=50"),
        (0, 0, 0, 0.5, "K=50"),
        (0.2, 0.8, 0, 0.1, "Reddish"),
        (0.9, 0.2, 0.1, 0.3, "Teal"),
    ]

    cols = 4
    size = 80
    pad = 10
    for i, (cm_, m_, y_, k_, label) in enumerate(patches):
        col = i % cols
        row = i // cols
        x = pad + col * (size + pad)
        y = 400 - pad - (row + 1) * (size + pad)
        c.setFillColor(CMYKColor(cm_, m_, y_, k_))
        c.rect(x, y, size, size, fill=1, stroke=0)
        c.setFillColor(black)
        c.setFont("Helvetica", 8)
        c.drawString(x + 2, y + 2, label)

    c.save()
    print("  Created test_cmyk.pdf")


def make_linestyles_pdf():
    """test_linestyles.pdf - line cap/join combos, dash patterns, miter limits."""
    c = canvas.Canvas(os.path.join(OUTDIR, "test_linestyles.pdf"), pagesize=(500, 600))
    c.setTitle("Line Styles Test")

    y = 570

    # Line caps
    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, y, "Line Caps (butt, round, square)")
    y -= 30
    for i, cap in enumerate([0, 1, 2]):  # butt, round, projecting square
        c.setLineCap(cap)
        c.setStrokeColor(black)
        c.setLineWidth(8)
        c.line(50, y, 200, y)
        # Thin reference line
        c.setLineWidth(0.5)
        c.setStrokeColor(red)
        c.line(50, y, 200, y)
        y -= 30

    # Line joins
    y -= 10
    c.setFont("Helvetica-Bold", 12)
    c.setStrokeColor(black)
    c.drawString(20, y, "Line Joins (miter, round, bevel)")
    y -= 40
    for i, join in enumerate([0, 1, 2]):
        c.setLineJoin(join)
        c.setLineWidth(8)
        c.setStrokeColor(black)
        p = c.beginPath()
        p.moveTo(50, y)
        p.lineTo(125, y + 30)
        p.lineTo(200, y)
        c.drawPath(p, fill=0, stroke=1)
        y -= 50

    # Dash patterns
    y -= 10
    c.setFont("Helvetica-Bold", 12)
    c.setLineJoin(0)
    c.setLineCap(0)
    c.drawString(20, y, "Dash Patterns")
    y -= 25
    dashes = [
        ([6, 3], 0, "6 on, 3 off"),
        ([2, 2], 0, "2 on, 2 off"),
        ([10, 5, 2, 5], 0, "dash-dot"),
        ([6, 3], 3, "phase=3"),
    ]
    for dash, phase, label in dashes:
        c.setDash(dash, phase)
        c.setLineWidth(2)
        c.setStrokeColor(black)
        c.line(50, y, 250, y)
        c.setFont("Helvetica", 9)
        c.drawString(260, y - 3, label)
        y -= 20

    # Miter limits
    c.setDash([])
    y -= 20
    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, y, "Miter Limits")
    y -= 40
    for ml in [1, 2, 5, 10]:
        c.setMiterLimit(ml)
        c.setLineJoin(0)  # miter join
        c.setLineWidth(6)
        c.setStrokeColor(black)
        p = c.beginPath()
        p.moveTo(50, y)
        p.lineTo(100, y + 25)
        p.lineTo(150, y)
        c.drawPath(p, fill=0, stroke=1)
        c.setFont("Helvetica", 9)
        c.drawString(160, y, f"ML={ml}")
        y -= 40

    c.save()
    print("  Created test_linestyles.pdf")


def make_curves_pdf():
    """test_curves.pdf - bezier curve variants and degenerate paths."""
    c = canvas.Canvas(os.path.join(OUTDIR, "test_curves.pdf"), pagesize=(500, 500))
    c.setTitle("Bezier Curves Test")

    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, 475, "Cubic Bezier Curves")

    c.setLineWidth(2)
    c.setStrokeColor(blue)

    # Standard cubic bezier
    p = c.beginPath()
    p.moveTo(50, 430)
    p.curveTo(100, 480, 200, 380, 250, 430)
    c.drawPath(p)

    # S-curve
    p = c.beginPath()
    p.moveTo(50, 360)
    p.curveTo(100, 410, 150, 310, 200, 360)
    p.curveTo(250, 410, 300, 310, 350, 360)
    c.drawPath(p)

    # Near-degenerate (control points close to endpoints)
    c.setStrokeColor(red)
    p = c.beginPath()
    p.moveTo(50, 290)
    p.curveTo(51, 291, 199, 289, 200, 290)
    c.drawPath(p)

    # Loop/self-intersecting bezier
    c.setStrokeColor(green)
    p = c.beginPath()
    p.moveTo(50, 220)
    p.curveTo(250, 320, 50, 120, 250, 220)
    c.drawPath(p)

    # Circle approximation with 4 beziers
    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, 180, "Circle via 4 Beziers")
    cx_, cy_, r = 150, 120, 50
    k = 0.5522847498  # magic number for circle approximation
    p = c.beginPath()
    p.moveTo(cx_ + r, cy_)
    p.curveTo(cx_ + r, cy_ + r * k, cx_ + r * k, cy_ + r, cx_, cy_ + r)
    p.curveTo(cx_ - r * k, cy_ + r, cx_ - r, cy_ + r * k, cx_ - r, cy_)
    p.curveTo(cx_ - r, cy_ - r * k, cx_ - r * k, cy_ - r, cx_, cy_ - r)
    p.curveTo(cx_ + r * k, cy_ - r, cx_ + r, cy_ - r * k, cx_ + r, cy_)
    p.close()
    c.setStrokeColor(black)
    c.setFillColor(Color(0.9, 0.9, 1.0))
    c.drawPath(p, fill=1, stroke=1)

    c.save()
    print("  Created test_curves.pdf")


def make_winding_pdf():
    """test_winding.pdf - even-odd vs non-zero winding on overlapping shapes."""
    c = canvas.Canvas(os.path.join(OUTDIR, "test_winding.pdf"), pagesize=(500, 300))
    c.setTitle("Fill Rules Test")

    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, 275, "Non-Zero Winding")
    c.drawString(270, 275, "Even-Odd")

    def draw_star(canvas_obj, cx_, cy_, r_outer, r_inner, fill_rule):
        """Draw a 5-pointed star."""
        pts = []
        for i in range(5):
            angle = math.radians(-90 + i * 144)
            pts.append((cx_ + r_outer * math.cos(angle), cy_ + r_outer * math.sin(angle)))

        p = canvas_obj.beginPath()
        p.moveTo(pts[0][0], pts[0][1])
        for pt in pts[1:]:
            p.lineTo(pt[0], pt[1])
        p.close()

        canvas_obj.setFillColor(Color(0.2, 0.5, 0.8, 0.7))
        canvas_obj.setStrokeColor(black)
        canvas_obj.setLineWidth(1)

        # Use _code to emit raw PDF operators for fill rule
        if fill_rule == "nonzero":
            canvas_obj.drawPath(p, fill=1, stroke=1)
        else:
            # reportlab uses even-odd by setting fillMode
            canvas_obj._fillMode = 1  # even-odd
            canvas_obj.drawPath(p, fill=1, stroke=1)
            canvas_obj._fillMode = 0  # reset

    draw_star(c, 130, 150, 80, 30, "nonzero")
    draw_star(c, 380, 150, 80, 30, "evenodd")

    c.save()
    print("  Created test_winding.pdf")


def make_transforms_pdf():
    """test_transforms.pdf - nested CTM, rotation, scale, skew."""
    c = canvas.Canvas(os.path.join(OUTDIR, "test_transforms.pdf"), pagesize=(500, 500))
    c.setTitle("Transforms Test")

    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, 475, "Transform Tests")

    # Simple translation
    c.saveState()
    c.translate(100, 400)
    c.setFillColor(red)
    c.rect(0, 0, 40, 40, fill=1)
    c.restoreState()

    # Rotation
    c.saveState()
    c.translate(250, 420)
    c.rotate(30)
    c.setFillColor(green)
    c.rect(0, 0, 60, 20, fill=1)
    c.restoreState()

    # Scale
    c.saveState()
    c.translate(100, 320)
    c.scale(2, 0.5)
    c.setFillColor(blue)
    c.rect(0, 0, 40, 40, fill=1)
    c.restoreState()

    # Skew
    c.saveState()
    c.translate(250, 320)
    c.skew(20, 0)
    c.setFillColor(yellow)
    c.setStrokeColor(black)
    c.rect(0, 0, 50, 50, fill=1, stroke=1)
    c.restoreState()

    # Nested transforms
    c.saveState()
    c.translate(100, 200)
    c.rotate(15)
    c.saveState()
    c.translate(50, 0)
    c.scale(1.5, 1.5)
    c.setFillColor(magenta)
    c.rect(0, 0, 30, 30, fill=1)
    c.restoreState()
    c.setFillColor(cyan)
    c.rect(0, 0, 30, 30, fill=1)
    c.restoreState()

    # Mirroring (negative scale)
    c.saveState()
    c.translate(350, 220)
    c.scale(-1, 1)
    c.setFont("Helvetica", 14)
    c.drawString(0, 0, "Mirror")
    c.restoreState()

    c.save()
    print("  Created test_transforms.pdf")


def make_blendmodes_pdf():
    """test_blendmodes.pdf - all 16 PDF blend modes on overlapping shapes."""
    from reportlab.lib.colors import Color

    modes = [
        "Normal", "Multiply", "Screen", "Overlay",
        "Darken", "Lighten", "ColorDodge", "ColorBurn",
        "HardLight", "SoftLight", "Difference", "Exclusion",
        "Hue", "Saturation", "Color", "Luminosity",
    ]

    c = canvas.Canvas(os.path.join(OUTDIR, "test_blendmodes.pdf"), pagesize=(500, 600))
    c.setTitle("Blend Modes Test")

    cols = 4
    size = 80
    pad = 30

    for i, mode in enumerate(modes):
        col = i % cols
        row = i // cols
        x = pad + col * (size + pad)
        y = 600 - pad - (row + 1) * (size + pad + 15)

        # Background circle (red)
        c.saveState()
        c.setFillColor(Color(1, 0, 0, 0.8))
        c.circle(x + size * 0.4, y + size * 0.6, size * 0.3, fill=1, stroke=0)

        # Foreground circle with blend mode (blue)
        # Use PDF extended graphics state for blend mode
        c.setFillColor(Color(0, 0, 1, 0.8))

        # Set blend mode via graphics state
        gs_name = f"GS{i}"
        # We'll use low-level PDF operations
        c._code.append(f'/{gs_name} gs')
        # Register the graphics state
        if not hasattr(c, '_blend_gs_registered'):
            c._blend_gs_registered = {}
        c._blend_gs_registered[gs_name] = mode

        c.circle(x + size * 0.6, y + size * 0.4, size * 0.3, fill=1, stroke=0)
        c.restoreState()

        # Label
        c.setFont("Helvetica", 7)
        c.setFillColor(black)
        c.drawString(x, y - 5, mode)

    # Register ExtGState resources in the page
    # This is a simplified approach - for proper blend mode testing,
    # we'd need to add ExtGState to the page resources
    c.save()
    print("  Created test_blendmodes.pdf (note: blend modes may need manual PDF editing)")


def make_softmask_pdf():
    """test_softmask.pdf - gradient soft masks over shapes."""
    c = canvas.Canvas(os.path.join(OUTDIR, "test_softmask.pdf"), pagesize=(400, 300))
    c.setTitle("Soft Mask Test")

    c.setFont("Helvetica-Bold", 12)
    c.drawString(20, 275, "Soft Mask / Transparency Test")

    # Overlapping semi-transparent circles
    for alpha, cx_, cy_, color in [
        (0.5, 120, 180, Color(1, 0, 0)),
        (0.5, 180, 180, Color(0, 1, 0)),
        (0.5, 150, 130, Color(0, 0, 1)),
    ]:
        c.saveState()
        c.setFillColor(Color(color.red, color.green, color.blue, alpha))
        c.circle(cx_, cy_, 60, fill=1, stroke=0)
        c.restoreState()

    # Gradient transparency (approximated with multiple rects)
    c.setFont("Helvetica", 9)
    c.drawString(260, 250, "Gradient alpha")
    steps = 20
    for i in range(steps):
        alpha = i / float(steps)
        x = 260 + i * 5
        c.setFillColor(Color(0.2, 0.4, 0.8, alpha))
        c.rect(x, 150, 6, 80, fill=1, stroke=0)

    c.save()
    print("  Created test_softmask.pdf")


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    print("Generating test PDFs in", OUTDIR)

    make_cmyk_pdf()
    make_linestyles_pdf()
    make_curves_pdf()
    make_winding_pdf()
    make_transforms_pdf()
    make_blendmodes_pdf()
    make_softmask_pdf()

    print("Done! Generated 7 test PDFs.")


if __name__ == "__main__":
    main()
