import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

def create_galaxy_nebula(W, H, is_plus=False):
    """Generates a deep, rich cosmic galaxy background."""
    y, x = np.ogrid[:H, :W]
    cx, cy = W / 2.0, H / 2.0
    nx = (x - cx) / (W / 2.0)
    ny = (y - cy) / (H / 2.0)
    r = np.sqrt(nx * nx + ny * ny)
    theta = np.arctan2(ny, nx)

    # Base space palette
    if not is_plus:
        # Deep space navy / indigo
        bg_r = 7.0 + 15.0 * (1.0 - np.clip(r, 0, 1.2))
        bg_g = 10.0 + 18.0 * (1.0 - np.clip(r, 0, 1.2))
        bg_b = 26.0 + 44.0 * (1.0 - np.clip(r, 0, 1.2))
    else:
        # Deep cosmic obsidian purple
        bg_r = 16.0 + 26.0 * (1.0 - np.clip(r, 0, 1.2))
        bg_g = 6.0 + 12.0 * (1.0 - np.clip(r, 0, 1.2))
        bg_b = 30.0 + 55.0 * (1.0 - np.clip(r, 0, 1.2))

    # Swirling Spiral Arms (Logarithmic spiral)
    log_r = np.log(np.clip(r, 0.04, 2.0))
    spiral_angle = theta - 3.4 * log_r
    arm1 = np.clip(np.cos(spiral_angle), 0, 1) ** 3.5
    arm2 = np.clip(np.cos(spiral_angle + math.pi), 0, 1) ** 3.5
    arms = (arm1 + arm2) * np.exp(-1.4 * r)

    if not is_plus:
        # Celestial Cyan, Royal Blue, & Lavender dust
        bg_r += arms * 70.0 + np.exp(-2.5 * r) * 75.0
        bg_g += arms * 105.0 + np.exp(-2.5 * r) * 70.0
        bg_b += arms * 200.0 + np.exp(-2.0 * r) * 125.0
    else:
        # Neon Magenta, Cosmic Violet, & Cyan dust
        bg_r += arms * 145.0 + np.exp(-2.5 * r) * 110.0
        bg_g += arms * 45.0 + np.exp(-2.5 * r) * 50.0
        bg_b += arms * 210.0 + np.exp(-2.0 * r) * 140.0

    # Core warm stellar glow
    core = np.exp(-3.8 * (r ** 2))
    bg_r += core * 125.0
    bg_g += core * 140.0
    bg_b += core * 165.0

    img_arr = np.dstack([
        np.clip(bg_r, 0, 255).astype(np.uint8),
        np.clip(bg_g, 0, 255).astype(np.uint8),
        np.clip(bg_b, 0, 255).astype(np.uint8),
        np.full((H, W), 255, dtype=np.uint8)
    ])
    nebula = Image.fromarray(img_arr, "RGBA")

    # Twinkling star field
    np.random.seed(101 if not is_plus else 202)
    draw = ImageDraw.Draw(nebula)
    num_stars = 260
    for _ in range(num_stars):
        sx = np.random.uniform(0, W)
        sy = np.random.uniform(0, H)
        srad = np.random.uniform(0.7, 2.4)
        bright = np.random.uniform(0.35, 1.0)
        col = (
            int(220 * bright + 35),
            int(235 * bright + 20),
            255,
            int(255 * bright)
        )
        draw.ellipse([sx - srad, sy - srad, sx + srad, sy + srad], fill=col)

    # Cross flare stars
    for _ in range(16):
        fx = np.random.uniform(W * 0.05, W * 0.95)
        fy = np.random.uniform(H * 0.05, H * 0.95)
        if math.hypot(fx - cx, fy - cy) < W * 0.22:
            continue
        flen = np.random.uniform(8.0, 18.0)
        draw.line([fx - flen, fy, fx + flen, fy], fill=(255, 255, 255, 170), width=1)
        draw.line([fx, fy - flen, fx, fy + flen], fill=(255, 255, 255, 170), width=1)
        draw.ellipse([fx - 2, fy - 2, fx + 2, fy + 2], fill=(255, 255, 255, 240))

    return nebula


def draw_crystal_starbits(img, cx, cy, is_plus=False):
    """Draws multi-colored floating crystal starbits with 3D faceted shading."""
    W, H = img.size
    draw = ImageDraw.Draw(img)

    starbits = [
        {"x": cx - 360, "y": cy - 230, "r": 25, "col": (255, 60, 95)},    # Ruby red
        {"x": cx + 340, "y": cy - 250, "r": 28, "col": (0, 225, 255)},    # Sapphire cyan
        {"x": cx - 330, "y": cy + 300, "r": 26, "col": (85, 245, 130)},   # Emerald green
        {"x": cx + 340, "y": cy + 280, "r": 29, "col": (255, 215, 20)},   # Citrine gold
        {"x": cx + 220, "y": cy - 400, "r": 19, "col": (255, 130, 230)},  # Rose pink
        {"x": cx - 210, "y": cy + 410, "r": 21, "col": (150, 110, 255)},  # Amethyst purple
    ]

    for sb in starbits:
        bx, by, br, bcol = sb["x"], sb["y"], sb["r"], sb["col"]
        # Ambient glow
        glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        g_draw = ImageDraw.Draw(glow)
        g_draw.ellipse([bx - br * 2.5, by - br * 2.5, bx + br * 2.5, by + br * 2.5], fill=(*bcol, 135))
        glow = glow.filter(ImageFilter.GaussianBlur(radius=14))
        img.alpha_composite(glow)

        # 6-sided faceted crystal polygon
        pts = []
        for i in range(6):
            a = i * (math.pi / 3.0) + 0.2
            pts.append((bx + br * math.cos(a), by + br * math.sin(a)))
        
        # Facets
        for i in range(6):
            p1 = pts[i]
            p2 = pts[(i + 1) % 6]
            light_mult = 0.75 + 0.4 * math.cos(i * (math.pi / 3.0) - 0.8)
            fcol = tuple(min(255, int(c * light_mult)) for c in bcol)
            draw.polygon([(bx, by), p1, p2], fill=(*fcol, 255))
        
        draw.polygon(pts, outline=(255, 255, 255, 210), width=2)
        # Specular glint
        draw.ellipse([bx - br * 0.35, by - br * 0.35, bx + br * 0.1, by + br * 0.1], fill=(255, 255, 255, 240))


def draw_smooth_ring_arc(img, cx, cy, rx, ry, tilt_deg, thick, fill_color, spec_color=None, angle_range=(0, 360)):
    """Draws a silky-smooth anti-aliased polygon arc."""
    W, H = img.size
    temp = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(temp)

    start_deg, end_deg = angle_range
    pts_outer = []
    pts_inner = []
    tilt_rad = math.radians(tilt_deg)
    cos_t = math.cos(tilt_rad)
    sin_t = math.sin(tilt_rad)

    r_out_x = rx + thick * 0.5
    r_out_y = ry + thick * 0.5
    r_in_x = rx - thick * 0.5
    r_in_y = ry - thick * 0.5

    steps = int(abs(end_deg - start_deg) * 2)
    for i in range(steps + 1):
        deg = start_deg + (end_deg - start_deg) * i / steps
        rad = math.radians(deg)
        # Outer
        ox = r_out_x * math.cos(rad)
        oy = r_out_y * math.sin(rad)
        gx_o = cx + ox * cos_t - oy * sin_t
        gy_o = cy + ox * sin_t + oy * cos_t
        pts_outer.append((gx_o, gy_o))

        # Inner
        ix = r_in_x * math.cos(rad)
        iy = r_in_y * math.sin(rad)
        gx_i = cx + ix * cos_t - iy * sin_t
        gy_i = cy + ix * sin_t + iy * cos_t
        pts_inner.append((gx_i, gy_i))

    poly_pts = pts_outer + pts_inner[::-1]
    if len(poly_pts) >= 4:
        # Soft blur glow layer underneath
        glow_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        g_draw = ImageDraw.Draw(glow_layer)
        g_draw.polygon(poly_pts, fill=(fill_color[0], fill_color[1], fill_color[2], 140))
        glow_layer = glow_layer.filter(ImageFilter.GaussianBlur(radius=10))
        img.alpha_composite(glow_layer)

        # Main solid ring
        draw.polygon(poly_pts, fill=fill_color)

    # Specular highlight on top curve
    if spec_color and len(pts_outer) > 1:
        draw.line(pts_outer, fill=spec_color, width=max(2, int(thick * 0.22)))

    img.alpha_composite(temp)


def render_mario_galaxy_star_3d(img, cx, cy, R_out=270, is_plus=False):
    """
    Renders the authentic 3D faceted Super Mario Galaxy Power Star
    with true 3D normal vector shading across all 10 facets,
    smooth bevels, specular reflections, and the signature Mario star eyes.
    """
    W, H = img.size
    R_in = R_out * 0.49
    Z_center = 75.0  # Height of central ridge in 3D

    # 1. Outer radiant golden/cyan aura
    aura = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    a_draw = ImageDraw.Draw(aura)
    pts_aura = []
    for i in range(10):
        ang = -math.pi / 2.0 + i * (math.pi / 5.0)
        r = (R_out + 45) if (i % 2 == 0) else (R_in + 35)
        pts_aura.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    aura_col = (255, 215, 60, 160) if not is_plus else (0, 230, 255, 175)
    a_draw.polygon(pts_aura, fill=aura_col)
    aura = aura.filter(ImageFilter.GaussianBlur(radius=32))
    img.alpha_composite(aura)

    # 2. Build 3D Points for the 10 outer vertices
    # Tips T0..T4 and Valleys V0..V4
    outer_pts_2d = []
    outer_pts_3d = []
    for i in range(10):
        ang = -math.pi / 2.0 + i * (math.pi / 5.0)
        r = R_out if (i % 2 == 0) else R_in
        px = cx + r * math.cos(ang)
        py = cy + r * math.sin(ang)
        outer_pts_2d.append((px, py))
        outer_pts_3d.append((px, py, 0.0))  # Base plane Z=0

    center_3d = (cx, cy, Z_center)

    # Dark amber/ruby border outline for crisp definition
    star_base = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sb_draw = ImageDraw.Draw(star_base)
    pts_border = []
    for i in range(10):
        ang = -math.pi / 2.0 + i * (math.pi / 5.0)
        r = (R_out + 6) if (i % 2 == 0) else (R_in + 4)
        pts_border.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    border_col = (185, 80, 0, 255) if not is_plus else (120, 15, 110, 255)
    sb_draw.polygon(pts_border, fill=border_col)

    # 3. Render 10 Facets with 3D Normal Lighting
    # Light direction: from top-left, slightly toward viewer: L = (-0.5, -0.65, 0.58)
    lx, ly, lz = -0.48, -0.62, 0.62
    l_len = math.sqrt(lx*lx + ly*ly + lz*lz)
    lx, ly, lz = lx / l_len, ly / l_len, lz / l_len

    # Viewer V = (0, 0, 1)
    vx, vy, vz = 0.0, 0.0, 1.0
    hx, hy, hz = lx + vx, ly + vy, lz + vz
    h_len = math.sqrt(hx*hx + hy*hy + hz*hz)
    hx, hy, hz = hx / h_len, hy / h_len, hz / h_len

    for i in range(10):
        p1 = outer_pts_3d[i]
        p2 = outer_pts_3d[(i + 1) % 10]
        # Vectors from center
        v1 = (p1[0] - center_3d[0], p1[1] - center_3d[1], p1[2] - center_3d[2])
        v2 = (p2[0] - center_3d[0], p2[1] - center_3d[1], p2[2] - center_3d[2])
        # Cross product v1 x v2
        nx = v1[1] * v2[2] - v1[2] * v2[1]
        ny = v1[2] * v2[0] - v1[0] * v2[2]
        nz = v1[0] * v2[1] - v1[1] * v2[0]
        # Ensure normal points toward viewer (positive Z)
        if nz < 0:
            nx, ny, nz = -nx, -ny, -nz
        n_len = math.hypot(nx, ny, nz)
        if n_len > 0:
            nx, ny, nz = nx / n_len, ny / n_len, nz / n_len
        else:
            nx, ny, nz = 0, 0, 1

        # Diffuse dot
        dot_l = max(0.0, nx * lx + ny * ly + nz * lz)
        # Specular dot
        dot_h = max(0.0, nx * hx + ny * hy + nz * hz)
        spec = (dot_h ** 14) * 0.9

        # Color evaluation
        if not is_plus:
            # Gold palette:
            # Shadow: (230, 130, 0) -> Lit: (255, 235, 75)
            r = int(min(255, 220 + 35 * dot_l + spec * 255))
            g = int(min(255, 130 + 105 * dot_l + spec * 255))
            b = int(min(255, 5 + 70 * dot_l + spec * 255))
        else:
            # Plus palette: Supercharged Prism Gold with iridescent highlights
            r = int(min(255, 225 + 30 * dot_l + spec * 255))
            g = int(min(255, 145 + 95 * dot_l + spec * 255))
            b = int(min(255, 35 + 130 * dot_l + spec * 255))

        tri_2d = [(center_3d[0], center_3d[1]), (p1[0], p1[1]), (p2[0], p2[1])]
        sb_draw.polygon(tri_2d, fill=(r, g, b, 255))

    img.alpha_composite(star_base)

    # 4. Soft golden ambient central glow over face to round off facet edges
    soft = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    s_draw = ImageDraw.Draw(soft)
    center_glow = (255, 240, 110, 140) if not is_plus else (255, 245, 160, 150)
    s_draw.ellipse([cx - R_in * 0.8, cy - R_in * 0.7, cx + R_in * 0.8, cy + R_in * 0.8], fill=center_glow)
    soft = soft.filter(ImageFilter.GaussianBlur(radius=24))
    img.alpha_composite(soft)

    # 5. Top Point Specular Glint
    spec_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sp_draw = ImageDraw.Draw(spec_layer)
    top_p = outer_pts_2d[0]
    sp_draw.ellipse([top_p[0] - 25, top_p[1] + 15, top_p[0] + 25, top_p[1] + 65], fill=(255, 255, 255, 190))
    spec_layer = spec_layer.filter(ImageFilter.GaussianBlur(radius=10))
    img.alpha_composite(spec_layer)

    # 6. Signature Nintendo Super Mario Galaxy Eyes
    eye_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    eye_draw = ImageDraw.Draw(eye_layer)
    eye_w = R_out * 0.125
    eye_h = R_out * 0.34
    eye_spacing = R_out * 0.22
    eye_cy = cy + R_out * 0.05

    left_eye_cx = cx - eye_spacing * 0.5
    right_eye_cx = cx + eye_spacing * 0.5

    for ecx in [left_eye_cx, right_eye_cx]:
        # Eye drop shadow
        eye_draw.ellipse(
            [ecx - eye_w * 0.5, eye_cy - eye_h * 0.5 + 3, ecx + eye_w * 0.5, eye_cy + eye_h * 0.5 + 3],
            fill=(140, 70, 0, 170)
        )
        # Eye main black/indigo glossy body
        eye_draw.ellipse(
            [ecx - eye_w * 0.5, eye_cy - eye_h * 0.5, ecx + eye_w * 0.5, eye_cy + eye_h * 0.5],
            fill=(16, 18, 30, 255)
        )
        # Specular glint (oval near the top-right of each eye)
        glint_w = eye_w * 0.46
        glint_h = eye_h * 0.36
        glint_cx = ecx + eye_w * 0.10
        glint_cy = eye_cy - eye_h * 0.24
        eye_draw.ellipse(
            [glint_cx - glint_w * 0.5, glint_cy - glint_h * 0.5, glint_cx + glint_w * 0.5, glint_cy + glint_h * 0.5],
            fill=(255, 255, 255, 255)
        )
        # Tiny secondary glint at bottom-left
        sec_w = eye_w * 0.22
        sec_h = eye_h * 0.18
        sec_cx = ecx - eye_w * 0.12
        sec_cy = eye_cy + eye_h * 0.26
        eye_draw.ellipse(
            [sec_cx - sec_w * 0.5, sec_cy - sec_h * 0.5, sec_cx + sec_w * 0.5, sec_cy + sec_h * 0.5],
            fill=(255, 255, 255, 200)
        )

    # Warm rosy cheeks
    blush = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bl_draw = ImageDraw.Draw(blush)
    blush_w = R_out * 0.18
    blush_h = R_out * 0.10
    blush_y = eye_cy + eye_h * 0.46
    for ecx in [left_eye_cx - eye_w * 0.9, right_eye_cx + eye_w * 0.9]:
        bl_draw.ellipse(
            [ecx - blush_w * 0.5, blush_y - blush_h * 0.5, ecx + blush_w * 0.5, blush_y + blush_h * 0.5],
            fill=(255, 125, 55, 130)
        )
    blush = blush.filter(ImageFilter.GaussianBlur(radius=8))
    img.alpha_composite(blush)
    img.alpha_composite(eye_layer)


def draw_plus_crest(img, W, H):
    """Draws a bold, high-tech, glowing 3D Plus badge in the lower-right."""
    badge_cx = W * 0.77
    badge_cy = H * 0.76
    badge_r = W * 0.175

    draw = ImageDraw.Draw(img)

    # 1. Vibrant cyan/magenta aura glow behind badge
    aura = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    a_draw = ImageDraw.Draw(aura)
    a_draw.ellipse(
        [badge_cx - badge_r * 1.5, badge_cy - badge_r * 1.5, badge_cx + badge_r * 1.5, badge_cy + badge_r * 1.5],
        fill=(0, 235, 255, 180)
    )
    aura = aura.filter(ImageFilter.GaussianBlur(radius=28))
    img.alpha_composite(aura)

    # 2. Outer chamfered metallic bezel
    draw.ellipse([badge_cx - badge_r - 8, badge_cy - badge_r - 8, badge_cx + badge_r + 8, badge_cy + badge_r + 8], fill=(0, 229, 255, 255))
    draw.ellipse([badge_cx - badge_r - 3, badge_cy - badge_r - 3, badge_cx + badge_r + 3, badge_cy + badge_r + 3], fill=(255, 215, 0, 255))

    # 3. Inner badge plate (Deep midnight purple)
    draw.ellipse([badge_cx - badge_r, badge_cy - badge_r, badge_cx + badge_r, badge_cy + badge_r], fill=(18, 8, 42, 255))
    draw.ellipse([badge_cx - badge_r * 0.88, badge_cy - badge_r * 0.88, badge_cx + badge_r * 0.88, badge_cy + badge_r * 0.88], fill=(32, 14, 68, 255))

    # 4. Bold Glowing Plus Symbol '+'
    plus_len = badge_r * 0.62
    plus_w = badge_r * 0.27
    rad = plus_w * 0.45

    # Plus drop shadow
    draw.rounded_rectangle(
        [badge_cx - plus_len, badge_cy - plus_w * 0.5 + 3, badge_cx + plus_len, badge_cy + plus_w * 0.5 + 3],
        radius=rad, fill=(5, 2, 15, 200)
    )
    draw.rounded_rectangle(
        [badge_cx - plus_w * 0.5, badge_cy - plus_len + 3, badge_cx + plus_w * 0.5, badge_cy + plus_len + 3],
        radius=rad, fill=(5, 2, 15, 200)
    )

    # Outer white core
    draw.rounded_rectangle(
        [badge_cx - plus_len, badge_cy - plus_w * 0.5, badge_cx + plus_len, badge_cy + plus_w * 0.5],
        radius=rad, fill=(255, 255, 255, 255)
    )
    draw.rounded_rectangle(
        [badge_cx - plus_w * 0.5, badge_cy - plus_len, badge_cx + plus_w * 0.5, badge_cy + plus_len],
        radius=rad, fill=(255, 255, 255, 255)
    )

    # Inlay: horizontal cyan, vertical gold
    draw.rounded_rectangle(
        [badge_cx - (plus_len - 3), badge_cy - (plus_w * 0.5 - 3), badge_cx + (plus_len - 3), badge_cy + (plus_w * 0.5 - 3)],
        radius=rad * 0.8, fill=(0, 240, 255, 255)
    )
    draw.rounded_rectangle(
        [badge_cx - (plus_w * 0.5 - 3), badge_cy - (plus_len - 3), badge_cx + (plus_w * 0.5 - 3), badge_cy + (plus_len - 3)],
        radius=rad * 0.8, fill=(255, 220, 60, 255)
    )

    # Top gloss gleam on badge
    draw.arc(
        [badge_cx - badge_r * 0.86, badge_cy - badge_r * 0.86, badge_cx + badge_r * 0.86, badge_cy + badge_r * 0.86],
        start=190, end=350, fill=(255, 255, 255, 220), width=4
    )


def generate_icon(output_webp, is_plus=False):
    W, H = 1024, 1024
    cx, cy = 512, 512

    # Layer 1: Cosmic Nebula Background (full-bleed for adaptive launcher compatibility)
    canvas = create_galaxy_nebula(W, H, is_plus=is_plus)

    # Layer 2: Floating Starbits
    draw_crystal_starbits(canvas, cx, cy, is_plus=is_plus)

    # Layer 3: Back half of Launch Star Ring (180° to 360° passes behind star)
    ring_col = (250, 190, 25, 255) if not is_plus else (175, 225, 255, 255)
    spec_col = (255, 255, 235, 255) if not is_plus else (255, 255, 255, 255)

    draw_smooth_ring_arc(canvas, cx, cy, rx=350, ry=160, tilt_deg=-22, thick=26, fill_color=ring_col, spec_color=spec_col, angle_range=(180, 360))

    if is_plus:
        # Extra hyper-warp secondary ring back half
        ring2_col = (255, 120, 220, 255)
        draw_smooth_ring_arc(canvas, cx, cy, rx=410, ry=185, tilt_deg=26, thick=20, fill_color=ring2_col, spec_color=(255, 255, 255, 255), angle_range=(180, 360))

    # Layer 4: The 3D Super Mario Galaxy Power Star
    render_mario_galaxy_star_3d(canvas, cx, cy, R_out=265, is_plus=is_plus)

    # Layer 5: Front half of Launch Star Ring (0° to 180° passes IN FRONT of lower star points!)
    draw_smooth_ring_arc(canvas, cx, cy, rx=350, ry=160, tilt_deg=-22, thick=26, fill_color=ring_col, spec_color=spec_col, angle_range=(0, 180))

    if is_plus:
        # Extra hyper-warp secondary ring front half
        draw_smooth_ring_arc(canvas, cx, cy, rx=410, ry=185, tilt_deg=26, thick=20, fill_color=ring2_col, spec_color=(255, 255, 255, 255), angle_range=(0, 180))

    # Layer 6: Plus Badge (for GalaxyMobile+)
    if is_plus:
        draw_plus_crest(canvas, W, H)

    # Downscale with Lanczos for 512x512 master app icon
    out_512 = canvas.resize((512, 512), Image.Resampling.LANCZOS)
    
    # Save WEBP (lossless / high quality 96) and PNG
    out_512.save(output_webp, "WEBP", quality=96)
    png_path = output_webp.replace(".webp", ".png")
    out_512.save(png_path, "PNG")
    print(f"Generated {output_webp} and {png_path} successfully!")


if __name__ == "__main__":
    os.makedirs("out-icons", exist_ok=True)
    generate_icon("out-icons/ic_launcher.webp", is_plus=False)
    generate_icon("out-icons/ic_launcher_plus.webp", is_plus=True)
