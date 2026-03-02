#!/usr/bin/env python3
"""
doom_preview.py — SE/30 1-bit dither preview tool

Usage:  python3 doom_preview.py [image.png]
Requires: pip install pillow numpy
"""

import sys
import tkinter as tk
from tkinter import filedialog
from PIL import Image, ImageTk
import numpy as np

BAYER = np.array([
    [  0, 136,  34, 170],
    [204,  68, 238, 102],
    [ 51, 187,  17, 153],
    [255, 119, 221,  85],
], dtype=np.float32)


def apply_se30_dither(img, gamma, sbar_black, sbar_white, sbar_height,
                      r_weight=77, g_weight=150, b_weight=29,
                      game_black=0, game_white=255):
    img = img.convert('RGB')
    arr = np.asarray(img, dtype=np.float32)
    H, W = arr.shape[:2]
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]

    total = float(r_weight + g_weight + b_weight) or 1.0
    gray = (r * r_weight + g * g_weight + b * b_weight) / total
    gray = np.clip(gray, 0, 255)
    gray_g = np.power(gray / 255.0, max(gamma, 0.01)) * 255.0
    gray_g = np.clip(gray_g, 0, 255)

    # Contrast stretch: remap [game_black..game_white] → [0..255]
    denom = float(max(game_white - game_black, 1))
    gray_g = np.clip((gray_g - game_black) * 255.0 / denom, 0, 255)

    reps_y, reps_x = H // 4 + 1, W // 4 + 1
    bayer_tile = np.tile(BAYER, (reps_y, reps_x))[:H, :W]
    out = np.where(gray_g < bayer_tile, 0.0, 255.0)

    if 0 < sbar_height <= H:
        y0 = H - sbar_height
        sr = r[y0:, :]
        denom = float(max(sbar_white - sbar_black, 1))
        sb = np.clip((sr - sbar_black) * 255.0 / denom, 0, 255)
        out[y0:, :] = np.where(sb < 128.0, 0.0, 255.0)

    return Image.fromarray(out.astype(np.uint8), mode='L')


def make_test_image(w=320, h=200):
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            if y < 40:
                v = int(60 + y * 2)
                px[x, y] = (v // 3, v // 3, v)
            elif y < 110:
                dist = abs(x - w // 2)
                light = max(40, 180 - int(dist * 0.9) - int((y - 40) * 0.3))
                px[x, y] = (int(light*0.65), int(light*0.55), int(light*0.45))
            elif y < h - 32:
                light = int(50 + (y - 110) * 0.6)
                px[x, y] = (light//2, int(light*0.55), light//3)
            else:
                sb_x, sb_y = x % 56, y - (h - 32)
                if 6 < sb_x < 46 and 4 < sb_y < 28:
                    px[x, y] = (220, 175, 35)
                else:
                    px[x, y] = (75, 55, 35)
    return img


class App:
    def __init__(self, root, image_path=None):
        self.root = root
        self.root.title('SE/30 Dither Preview')
        self.root.resizable(True, True)
        self.source = None
        self._tk_img = None
        self._after_id = None
        self._show_orig = False

        self._build_ui()

        if image_path:
            self._load_path(image_path)
        else:
            self.source = make_test_image()
            self._schedule_update()

    def _build_ui(self):
        root = self.root
        FONT = ('Helvetica', 10)
        FONT_B = ('Helvetica', 10, 'bold')

        top = tk.Frame(root, padx=8, pady=6)
        top.pack(fill=tk.X)

        btn_frame = tk.Frame(top)
        btn_frame.pack(fill=tk.X, pady=(0, 6))
        tk.Button(btn_frame, text='Load image…', command=self._pick_file,
                  font=FONT).pack(side=tk.LEFT, padx=(0, 8))
        self._toggle_btn = tk.Button(btn_frame, text='Show: Dithered',
                                     command=self._toggle_view, font=FONT)
        self._toggle_btn.pack(side=tk.LEFT, padx=(0, 8))
        self._copy_btn = tk.Button(btn_frame, text='Copy values',
                                   command=self._copy_values, font=FONT)
        self._copy_btn.pack(side=tk.LEFT)

        specs = [
            ('Gamma',        'gamma',       0.10, 3.0, 0.01, 0.50),
            ('R weight',     'r_weight',    0,    255, 1,    77),
            ('G weight',     'g_weight',    0,    255, 1,    150),
            ('B weight',     'b_weight',    0,    255, 1,    29),
            ('Game black',   'game_black',  0,    255, 1,    0),
            ('Game white',   'game_white',  0,    255, 1,    255),
            ('SBAR_BLACK',   'sbar_black',  0,    255, 1,    100),
            ('SBAR_WHITE',   'sbar_white',  0,    255, 1,    170),
            ('Sbar height',  'sbar_height', 0,    640, 1,    32),
        ]
        self._vars = {}
        self._val_labels = {}

        grid = tk.Frame(top)
        grid.pack(fill=tk.X)

        for row, (label, key, lo, hi, res, default) in enumerate(specs):
            tk.Label(grid, text=label, font=FONT, anchor='w', width=12
                     ).grid(row=row, column=0, sticky='w')
            val_lbl = tk.Label(grid, text='', font=FONT_B, width=6, anchor='e')
            val_lbl.grid(row=row, column=1, sticky='e')
            self._val_labels[key] = val_lbl

            var = tk.DoubleVar(value=default) if isinstance(default, float) else tk.IntVar(value=default)
            self._vars[key] = var

            scale = tk.Scale(grid, variable=var, from_=lo, to=hi,
                             resolution=res, orient=tk.HORIZONTAL,
                             showvalue=False, length=180,
                             command=lambda _v, k=key: self._on_change(k))
            scale.grid(row=row, column=2, sticky='ew', padx=(4, 0))
            self._on_change(key)

        grid.columnconfigure(2, weight=1)

        self.canvas_lbl = tk.Label(root, bg='#111', cursor='crosshair')
        self.canvas_lbl.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

    def _toggle_view(self):
        self._show_orig = not self._show_orig
        self._toggle_btn.config(text='Show: Original' if self._show_orig else 'Show: Dithered')
        self._schedule_update()

    def _copy_values(self):
        v = {k: var.get() for k, var in self._vars.items()}
        text = (
            f"dither_gamma={v['gamma']:.2f} "
            f"r_weight={int(v['r_weight'])} g_weight={int(v['g_weight'])} b_weight={int(v['b_weight'])} "
            f"game_black={int(v['game_black'])} game_white={int(v['game_white'])} "
            f"sbar_black={int(v['sbar_black'])} sbar_white={int(v['sbar_white'])} sbar_height={int(v['sbar_height'])}"
        )
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self._copy_btn.config(text='Copied!')
        self.root.after(1500, lambda: self._copy_btn.config(text='Copy values'))

    def _on_change(self, key):
        v = self._vars[key].get()
        self._val_labels[key].config(text=f'{v:.2f}' if isinstance(v, float) else str(v))
        self._schedule_update()

    def _schedule_update(self):
        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
        self._after_id = self.root.after(30, self._update)

    def _pick_file(self):
        path = filedialog.askopenfilename(
            title='Open image',
            filetypes=[('Images', '*.png *.jpg *.jpeg *.bmp *.tiff *.gif'),
                       ('All files', '*')])
        if path:
            self._load_path(path)

    def _load_path(self, path):
        try:
            self.source = Image.open(path).convert('RGB')
            self._schedule_update()
        except Exception as e:
            print(f'Could not load {path}: {e}')

    def _update(self):
        self._after_id = None
        if self.source is None:
            return

        gamma       = float(self._vars['gamma'].get())
        r_weight    = int(self._vars['r_weight'].get())
        g_weight    = int(self._vars['g_weight'].get())
        b_weight    = int(self._vars['b_weight'].get())
        game_black  = int(self._vars['game_black'].get())
        game_white  = int(self._vars['game_white'].get())
        sbar_black  = int(self._vars['sbar_black'].get())
        sbar_white  = int(self._vars['sbar_white'].get())
        sbar_height = int(self._vars['sbar_height'].get())

        if self._show_orig:
            display = self.source.resize((640, 480), Image.NEAREST)
        else:
            result = apply_se30_dither(self.source, gamma, sbar_black, sbar_white, sbar_height,
                                       r_weight, g_weight, b_weight, game_black, game_white)
            display = result.resize((640, 480), Image.NEAREST).convert('RGB')

        self._tk_img = ImageTk.PhotoImage(display)
        self.canvas_lbl.config(image=self._tk_img, width=640, height=480)


def main():
    try:
        import numpy, PIL  # noqa
    except ImportError as e:
        print(f'Missing dependency: {e}\nRun: pip install pillow numpy')
        sys.exit(1)

    root = tk.Tk()
    App(root, sys.argv[1] if len(sys.argv) > 1 else None)
    root.mainloop()


if __name__ == '__main__':
    main()
