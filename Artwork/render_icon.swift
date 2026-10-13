// Renders ScreamMusic app-icon design variants to PNG.
//   swift render_icon.swift [outdir]
// Design: single eighth note + three radiating strokes (broadcast burst) at its
// top-right, on a lively gradient squircle. 1024 canvas, 824 artwork box, y-up.
import AppKit

let canvas: CGFloat = 1024
let box: CGFloat = 824
let inset: CGFloat = (canvas - box) / 2

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: a)
}

func newCtx(size: CGFloat) -> CGContext {
    let ctx = CGContext(data: nil, width: Int(size), height: Int(size),
                        bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.scaleBy(x: size / canvas, y: size / canvas)
    return ctx
}

func drawBackground(_ ctx: CGContext, colors: [CGColor]) {
    ctx.saveGState()
    let squircle = NSBezierPath(roundedRect: NSRect(x: inset, y: inset, width: box, height: box),
                                xRadius: 186, yRadius: 186)
    ctx.addPath(squircle.cgPath)
    ctx.clip()
    let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: colors as CFArray,
                          locations: [0, 0.55, 1])!
    ctx.drawLinearGradient(grad, start: CGPoint(x: 460, y: 924), end: CGPoint(x: 560, y: 100), options: [])
    ctx.restoreGState()
}

func ellipsePath(cx: CGFloat, cy: CGFloat, rx: CGFloat, ry: CGFloat, deg: CGFloat) -> CGPath {
    var t = CGAffineTransform(translationX: cx, y: cy).rotated(by: deg * .pi / 180)
    return CGPath(ellipseIn: CGRect(x: -rx, y: -ry, width: rx * 2, height: ry * 2), transform: &t)
}

struct Palette {
    let bg: [CGColor]
    let fg: CGColor
}

let purple = Palette(bg: [rgb(198, 104, 250), rgb(156, 64, 238), rgb(122, 44, 214)], fg: rgb(255, 255, 255))
let green  = Palette(bg: [rgb(86, 224, 128), rgb(34, 184, 96), rgb(14, 146, 66)], fg: rgb(255, 255, 255))
let sunny  = Palette(bg: [rgb(255, 224, 92), rgb(250, 196, 40), rgb(238, 158, 18)], fg: rgb(38, 30, 12))

/// Single eighth note: big tilted head, stem, curved flag with rounded tip.
/// head: center of note head. stemTop: y of stem top. Geometry tuned for the 824 box.
func drawSingleNote(_ ctx: CGContext, fg: CGColor, head: CGPoint,
                    attach: CGFloat = 95, stemW: CGFloat = 34, stemTop: CGFloat = 620,
                    flagW: CGFloat = 46) {
    let sx = head.x + attach - stemW
    ctx.setFillColor(fg)
    ctx.setStrokeColor(fg)
    // stem (top ends early; the flag's round cap forms the rounded stem top)
    ctx.fill(CGRect(x: sx, y: head.y, width: stemW, height: stemTop - 20 - head.y))
    // head
    ctx.addPath(ellipsePath(cx: head.x, cy: head.y, rx: 118, ry: 84, deg: 15))
    ctx.fillPath()
    // flag: stroked curve from the stem top, round caps at both ends
    ctx.setLineWidth(flagW)
    ctx.setLineCap(.round)
    ctx.beginPath()
    ctx.move(to: CGPoint(x: sx + stemW / 2, y: stemTop - 24))
    ctx.addCurve(to: CGPoint(x: sx + stemW + 164, y: stemTop - 115),
                 control1: CGPoint(x: sx + stemW + 67, y: stemTop - 22),
                 control2: CGPoint(x: sx + stemW + 154, y: stemTop - 86))
    ctx.strokePath()
}

/// Ripple arcs CONCENTRIC WITH THE FLAG's own curve: center below the stem top, so the
/// arcs bend the same way as the flag (clockwise, diving down at the right end) — the
/// flag radiating bigger copies of itself toward the upper right.
func drawRipples(_ ctx: CGContext, fg: CGColor, center: CGPoint, radii: [CGFloat], width: CGFloat,
                 fromDeg: CGFloat = 80, toDeg: CGFloat = 36) {
    ctx.setStrokeColor(fg)
    ctx.setLineWidth(width)
    ctx.setLineCap(.round)
    for r in radii {
        ctx.beginPath()
        ctx.addArc(center: center, radius: r, startAngle: fromDeg * .pi / 180,
                   endAngle: toDeg * .pi / 180, clockwise: true)
        ctx.strokePath()
    }
}

/// Three radiating strokes fanning up-right from `origin` (the broadcast burst).
func drawRays(_ ctx: CGContext, fg: CGColor, origin: CGPoint,
              angles: [CGFloat] = [20, 52, 84], r0: CGFloat = 58, r1: CGFloat = 138, width: CGFloat = 26) {
    ctx.setStrokeColor(fg)
    ctx.setLineWidth(width)
    ctx.setLineCap(.round)
    for a in angles {
        let r = a * .pi / 180
        ctx.beginPath()
        ctx.move(to: CGPoint(x: origin.x + r0 * cos(r), y: origin.y + r0 * sin(r)))
        ctx.addLine(to: CGPoint(x: origin.x + r1 * cos(r), y: origin.y + r1 * sin(r)))
        ctx.strokePath()
    }
}

enum Detail { case full, regular, medium, tiny }

func drawIcon(_ ctx: CGContext, p: Palette, detail: Detail) {
    let head = CGPoint(x: 360, y: 195)
    // ripple family centered below the stem top, concentric with the flag's curve
    let rippleC = CGPoint(x: 441, y: 455)
    switch detail {
    case .full:
        drawSingleNote(ctx, fg: p.fg, head: head, attach: 100, stemW: 38, stemTop: 620, flagW: 48)
        drawRipples(ctx, fg: p.fg, center: rippleC, radii: [220, 278, 336], width: 21)
    case .regular: // 64px: slightly thicker arcs
        drawSingleNote(ctx, fg: p.fg, head: head, attach: 100, stemW: 42, stemTop: 620, flagW: 54)
        drawRipples(ctx, fg: p.fg, center: rippleC, radii: [220, 278, 336], width: 24)
    case .medium: // 32px: bolder note, two arcs
        drawSingleNote(ctx, fg: p.fg, head: CGPoint(x: 365, y: 190), attach: 104, stemW: 50, stemTop: 625, flagW: 62)
        drawRipples(ctx, fg: p.fg, center: CGPoint(x: 446, y: 460), radii: [216, 330], width: 26)
    case .tiny: // 16px: bold note, single arc
        drawSingleNote(ctx, fg: p.fg, head: CGPoint(x: 355, y: 185), attach: 110, stemW: 62, stemTop: 635, flagW: 78)
        drawRipples(ctx, fg: p.fg, center: CGPoint(x: 451, y: 470), radii: [262], width: 40)
    }
}

// MARK: - Menu bar glyph (template image: black silhouette, system recolors)

/// Note + rays condensed into an 18pt square, y-up.
func drawMenuBarGlyph(_ ctx: CGContext, color: CGColor = rgb(0, 0, 0)) {
    ctx.setFillColor(color)
    ctx.setStrokeColor(color)
    ctx.setLineCap(.round)
    // slightly smaller note to leave room for the ripples
    let head = CGPoint(x: 5.8, y: 4.1)
    let attach: CGFloat = 2.55, stemW: CGFloat = 1.15, stemTop: CGFloat = 12.3
    let sx = head.x + attach - stemW
    // stem (flag's round cap forms the rounded top)
    ctx.fill(CGRect(x: sx, y: head.y, width: stemW, height: stemTop - 0.5 - head.y))
    // head
    ctx.addPath(ellipsePath(cx: head.x, cy: head.y, rx: 3.0, ry: 2.15, deg: 15))
    ctx.fillPath()
    // flag: quarter-arc-like curve, center ≈ (stemTop x, 8.5)
    ctx.setLineWidth(1.4)
    ctx.beginPath()
    ctx.move(to: CGPoint(x: sx + stemW / 2, y: stemTop - 0.6))
    ctx.addCurve(to: CGPoint(x: 11.5, y: 9.3),
                 control1: CGPoint(x: 9.3, y: 12.2),
                 control2: CGPoint(x: 11.2, y: 11.3))
    ctx.strokePath()
    // ripples: thinner arcs concentric with the flag's curve, inner arc clear of the flag
    ctx.setLineWidth(0.8)
    let origin = CGPoint(x: sx + stemW / 2, y: 8.5)
    for r in [5.0, 6.15, 7.3] as [CGFloat] {
        ctx.beginPath()
        ctx.addArc(center: origin, radius: r, startAngle: 80 * .pi / 180,
                   endAngle: 36 * .pi / 180, clockwise: true)
        ctx.strokePath()
    }
}

func renderMenuBar(_ px: CGFloat, suffix: String, outdir: String) {
    let ctx = CGContext(data: nil, width: Int(px), height: Int(px),
                        bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.scaleBy(x: px / 18, y: px / 18)
    drawMenuBarGlyph(ctx)
    let img = ctx.makeImage()!
    let data = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(outdir)/MenuBarIcon\(suffix).png"))
}

// MARK: - Render

func render(_ name: String, p: Palette, size: CGFloat, detail: Detail, outdir: String) {
    let ctx = newCtx(size: size)
    drawBackground(ctx, colors: p.bg)
    ctx.translateBy(x: inset, y: inset)
    drawIcon(ctx, p: p, detail: detail)
    let img = ctx.makeImage()!
    let data = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(outdir)/\(name).png"))
}

let outdir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."

// Final iconset: sunny colorway, size-adaptive detail tiers
let iconset = "\(outdir)/AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: iconset, withIntermediateDirectories: true)
let jobs: [(String, CGFloat, Detail)] = [
    ("icon_16x16", 16, .tiny),
    ("icon_16x16@2x", 32, .medium),
    ("icon_32x32", 32, .medium),
    ("icon_32x32@2x", 64, .regular),
    ("icon_128x128", 128, .full),
    ("icon_128x128@2x", 256, .full),
    ("icon_256x256", 256, .full),
    ("icon_256x256@2x", 512, .full),
    ("icon_512x512", 512, .full),
    ("icon_512x512@2x", 1024, .full),
]
for (name, px, detail) in jobs {
    let ctx = newCtx(size: px)
    drawBackground(ctx, colors: sunny.bg)
    ctx.translateBy(x: inset, y: inset)
    drawIcon(ctx, p: sunny, detail: detail)
    let img = ctx.makeImage()!
    let data = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(iconset)/\(name).png"))
}
// 1024 preview for README / quick look
render("AppIcon_1024", p: sunny, size: 1024, detail: .full, outdir: outdir)
// menu bar template glyph (@1x/@2x/@3x of 18pt)
renderMenuBar(18, suffix: "", outdir: outdir)
renderMenuBar(36, suffix: "@2x", outdir: outdir)
renderMenuBar(54, suffix: "@3x", outdir: outdir)

// Simulated light/dark menu bar preview: left light bg + black glyph, right dark bg + white glyph
do {
    let W = 480, H = 132, scale = 2
    let ctx = CGContext(data: nil, width: W * scale, height: H * scale,
                        bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.scaleBy(x: CGFloat(scale), y: CGFloat(scale))
    ctx.setFillColor(rgb(236, 236, 236)); ctx.fill(CGRect(x: 0, y: 0, width: W / 2, height: H))
    ctx.setFillColor(rgb(40, 40, 40)); ctx.fill(CGRect(x: CGFloat(W / 2), y: 0, width: CGFloat(W / 2), height: CGFloat(H)))
    for (x, color) in [(CGFloat(W / 4), rgb(0, 0, 0)), (CGFloat(W * 3 / 4), rgb(255, 255, 255))] {
        ctx.saveGState()
        ctx.translateBy(x: x - 30, y: CGFloat(H) / 2 - 30)
        ctx.scaleBy(x: 60 / 18, y: 60 / 18)
        drawMenuBarGlyph(ctx, color: color)
        ctx.restoreGState()
    }
    let img = ctx.makeImage()!
    let data = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(outdir)/MenuBarIcon_preview.png"))
}
print("wrote iconset + menu bar glyph to \(outdir)")
