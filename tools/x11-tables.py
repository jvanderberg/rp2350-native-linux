#!/usr/bin/env python3
"""x11-tables.py: print the constant tables in extras/lib/x11.h, so that
the library needs no floating point when it runs (the board's C has none
in hardware):

  xi_sin_q    sin over a quarter turn, 256 steps, 16.16 fixed point
  xi_gamma    coverage (0-256) to glyph alpha, with gamma 0.82, which
              makes the thin strokes of small unhinted text a little
              heavier
  xi_cur_*    the smooth hand and I-beam cursors: grey level and alpha
              of each pixel (premultiplied), drawn as unions of capsules
              and rounded boxes measured by signed distance, with an
              edge and a light shadow

Run it and paste its output over the tables between the BEGIN/END TABLES
lines of x11.h.
"""
import math


def table(name, ctype, vals, per=12):
    out = ["static const %s %s[%d] = {" % (ctype, name, len(vals))]
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    out.append("};")
    return "\n".join(out)


def capsule(px, py, a, b, r):
    dx, dy = b[0] - a[0], b[1] - a[1]
    l2 = max(dx * dx + dy * dy, 1e-6)
    t = min(max(((px - a[0]) * dx + (py - a[1]) * dy) / l2, 0.0), 1.0)
    return math.hypot(px - (a[0] + t * dx), py - (a[1] + t * dy)) - r


def rbox(px, py, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    qx, qy = abs(px - cx) - hx, abs(py - cy) - hy
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


EDGE = 1.0


def shape(art, px, py):
    if art == "hand":
        d = min(
            capsule(px, py, (12.1, 9.8), (12.75, 16.0), 0.8),
            capsule(px, py, (15.9, 14.25), (15.9, 17.0), 1.05),
            capsule(px, py, (18.35, 14.05), (18.35, 17.0), 1.1),
            capsule(px, py, (20.85, 14.75), (20.85, 17.0), 1.05),
            rbox(px, py, 12.0, 15.6, 21.9, 20.2, 0.9),
            rbox(px, py, 13.3, 18.0, 20.9, 23.3, 1.2),
            capsule(px, py, (9.4, 15.4), (13.6, 21.2), 1.1),
        )
        crease = min(
            capsule(px, py, (15.5, 17.2), (15.5, 21.0), 0.0),
            capsule(px, py, (17.5, 17.2), (17.5, 21.0), 0.0),
            capsule(px, py, (19.5, 17.2), (19.5, 21.0), 0.0),
            capsule(px, py, (17.2, 14.2), (17.2, 15.6), 0.0),
            capsule(px, py, (19.7, 14.6), (19.7, 15.6), 0.0),
        )
        return d, crease
    d = min(
        capsule(px, py, (4.5, 2.5), (4.5, 16.5), 0.5),
        capsule(px, py, (1.5, 1.5), (3.5, 1.5), 0.5),
        capsule(px, py, (3.5, 1.5), (4.5, 2.5), 0.5),
        capsule(px, py, (7.5, 1.5), (5.5, 1.5), 0.5),
        capsule(px, py, (5.5, 1.5), (4.5, 2.5), 0.5),
        capsule(px, py, (1.5, 17.5), (3.5, 17.5), 0.5),
        capsule(px, py, (3.5, 17.5), (4.5, 16.5), 0.5),
        capsule(px, py, (7.5, 17.5), (5.5, 17.5), 0.5),
        capsule(px, py, (5.5, 17.5), (4.5, 16.5), 0.5),
    )
    return d, None


def clamp01(v):
    return min(max(v, 0.0), 1.0)


def cursor(art):
    # size, where the image starts in the shape's frame, fill, edge
    w, h, (ox, oy), fill, edge = {
        "hand": (17, 20, (7.0, 7.0), 1.0, 0.0),
        "ibeam": (9, 21, (0.0, 0.0), 0.0, 1.0),
    }[art]
    vals = []
    for j in range(h):
        for i in range(w):
            x, y = i + 0.5 + ox, j + 0.5 + oy
            d, crease = shape(art, x, y)
            c = a = 0.0

            def over(col, cov):
                nonlocal c, a
                c = col * cov + c * (1 - cov)
                a = cov + a * (1 - cov)

            ds, _ = shape(art, x, y - 1.0)
            over(0.0, 0.28 * clamp01(1.0 - (ds - EDGE) / 1.3))
            over(edge, clamp01(0.5 - (d - EDGE)))
            inside = clamp01(0.5 - d)
            over(fill, inside)
            if crease is not None:
                over(edge, clamp01(0.5 - (crease - 0.38)) * inside)
            vals += [round(c * 255), round(a * 255)]
    return w, h, vals


def main():
    print(table("xi_sin_q", "int",
                [round(math.sin(i / 256 * math.pi / 2) * 65536) for i in range(257)], 10))
    print(table("xi_gamma", "unsigned char",
                [round((i / 256) ** 0.82 * 255) for i in range(257)], 16))
    for art, name in (("hand", "hand"), ("ibeam", "text")):
        w, h, vals = cursor(art)
        print("/* %d x %d: grey, alpha */" % (w, h))
        print(table("xi_cur_" + name, "unsigned char", vals, 16))


main()
