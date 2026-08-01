#!/usr/bin/env python3
"""ガチャ機ディスプレイ(ST7789 320x170)用に、画像を RGB565 の C 配列に変換する。

使い方:
    # このフォルダに src_locked / src_unlocked / src_thanks を置いてから
    python3 firmware/gacha_lock_ble/make_images.py

生成された images.h をファームウェアが自動で取り込む(存在しなければ従来の
図形描画にフォールバックするので、画像がなくてもコンパイルは通る)。

各画像は画面全体(320x170)を埋めるように、アスペクト比を保ったまま
中央基準でトリミング+縮小する。1枚あたり約106KBのフラッシュを消費する。
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("PIL(Pillow)が必要です: pip install pillow")

WIDTH = 320
HEIGHT = 170

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)

# 元画像はこのフォルダ(BLE版)に置くが、生成した images.h は同じ画面を使う
# 別バージョンのスケッチ(WiFi版)にも配る。Arduinoは同じフォルダのファイルしか
# includeできないため、それぞれのスケッチフォルダに置く必要がある。
OUT_PATHS = [
    os.path.join(HERE, "images.h"),
    os.path.join(FIRMWARE_DIR, "gacha_lock_wifi", "images.h"),
    os.path.join(FIRMWARE_DIR, "gacha_lock_serial", "images.h"),
]

# (Cのシンボル名, 元画像のベース名)
IMAGES = [
    ("IMG_LOCKED", "src_locked"),
    ("IMG_UNLOCKED", "src_unlocked"),
    ("IMG_THANKS", "src_thanks"),
]
EXTS = ("png", "jpg", "jpeg", "webp")


def find_source(base):
    for ext in EXTS:
        path = os.path.join(HERE, f"{base}.{ext}")
        if os.path.exists(path):
            return path
    return None


def fit_cover(im):
    """アスペクト比を保ったまま、320x170を覆うように縮小して中央を切り出す。"""
    im = im.convert("RGB")
    src_w, src_h = im.size
    scale = max(WIDTH / src_w, HEIGHT / src_h)
    new_w, new_h = max(WIDTH, round(src_w * scale)), max(HEIGHT, round(src_h * scale))
    im = im.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - WIDTH) // 2
    top = (new_h - HEIGHT) // 2
    return im.crop((left, top, left + WIDTH, top + HEIGHT))


def to_rgb565(im):
    out = []
    for r, g, b in im.getdata():
        out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return out


def emit(f, symbol, values):
    f.write(f"// {symbol}: {WIDTH}x{HEIGHT} RGB565 ({len(values) * 2} bytes)\n")
    f.write(f"const uint16_t {symbol}[{len(values)}] = {{\n")
    for i in range(0, len(values), 12):
        chunk = values[i:i + 12]
        f.write("  " + ",".join(f"0x{v:04X}" for v in chunk) + ",\n")
    f.write("};\n\n")


def main():
    found = []
    for symbol, base in IMAGES:
        path = find_source(base)
        if path is None:
            print(f"[skip] {base}.({'/'.join(EXTS)}) が見つかりません")
            continue
        im = fit_cover(Image.open(path))
        found.append((symbol, base, path, to_rgb565(im)))
        print(f"[ok]   {os.path.basename(path)} -> {symbol} ({WIDTH}x{HEIGHT})")

    if not found:
        sys.exit("変換できる画像がありません。PROMPTS.md を参考に画像を用意してください。")

    for out_path in OUT_PATHS:
        if not os.path.isdir(os.path.dirname(out_path)):
            continue   # そのバージョンのスケッチが無ければ飛ばす
        with open(out_path, "w") as f:
            f.write("// このファイルは make_images.py が自動生成します。手で編集しないこと。\n")
            f.write("// 元画像: " + ", ".join(os.path.basename(p) for _, _, p, _ in found) + "\n\n")
            f.write("#pragma once\n#include <stdint.h>\n\n")
            f.write(f"#define IMG_W {WIDTH}\n#define IMG_H {HEIGHT}\n\n")
            for symbol, _, _, values in found:
                f.write(f"#define HAS_{symbol} 1\n")
            f.write("\n")
            for symbol, _, _, values in found:
                emit(f, symbol, values)
        print(f"[out]  {out_path}")

    total = sum(len(v) * 2 for _, _, _, v in found)
    print(f"\n{len(found)}枚を変換しました (合計 {total / 1024:.0f} KB)")
    print("次にファームウェアを書き込んでください。")


if __name__ == "__main__":
    main()
