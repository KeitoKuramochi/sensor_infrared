# ガチャ機ディスプレイ用画像の生成プロンプト集(Nano Banana / Gemini用)

ガチャ機のESP32(1.9インチ ST7789)に表示する画像です。
色記憶ゲーム側の画像(`pc_game/static/img/PROMPTS.md`)と世界観を揃えてあります。

## 手順

1. 下のプロンプトで3枚生成する
2. このフォルダに `src_locked.png` / `src_unlocked.png` / `src_thanks.png` として保存
   (拡張子は png / jpg / jpeg / webp どれでも可)
3. 変換スクリプトを実行して `images.h` を生成する
   ```
   python3 firmware/gacha_lock_ble/make_images.py
   ```
4. ファームウェアを書き込む
   ```
   arduino-cli compile --upload -p /dev/cu.usbserial-XXXX \
     --fqbn esp32:esp32:esp32:UploadSpeed=115200 firmware/gacha_lock_ble
   ```

## 画像の仕様

- **画面は 320×170 ピクセルの横長**(かなり横に広い、約1.9:1)
- 生成時は大きめの横長(例: 1920×1020 前後)で作れば、スクリプトが自動で
  中央基準にトリミング+縮小する
- 横長なので「**左にキャラ、右に文字**」のような横並びレイアウトが映える
- 背景は端まで塗りつぶす(周囲に余白や白フチを作らない)
- 小さい画面なので、**文字は極端に大きく・太く**。細い線や小さい装飾は潰れて見えない

> ⚠️ 画像生成AIは日本語の文字が崩れることがよくあります。崩れたら、絵だけのバージョンを
> 生成してもらえば、プログラム側から日本語テキストを綺麗に焼き込むこともできます。

### 容量について

1枚あたり約106KB、3枚で約319KB。標準のパーティション設定(アプリ領域1.31MB)で
**画像3枚込みで75%**なので問題なく収まる(画像なしの状態は50%)。
4枚以上に増やしたい場合は、このボードは16MBフラッシュなので、
書き込み時に `--fqbn esp32:esp32:esp32:PartitionScheme=huge_app`(アプリ領域3MB)を
付ければ大幅に余裕ができる。

---

## 共通のスタイル指定(各プロンプトの末尾に入れてあります)

色記憶ゲームと同じ「フラットベクター/ネオン発光/濃いチャコール背景」路線。
マスコットは**リモコン型のキャラクター**(白いリモコンの体に12色のカラーボタン、
にっこりした目と口、手足がある)です。

---

### 1. ロック中 — `src_locked.png`

ゲームで500点取るまで回せない状態。「まだ引けない」ことが一目で分かるように。

```
A wide horizontal banner illustration (aspect ratio about 1.9:1) for a small gacha
machine display. On the left: a cute mascot character shaped like a white TV remote
control with 12 small colorful round buttons on its body (red, green, blue, orange,
lime, purple, yellow, sky blue, pink), simple smiling dot eyes, tiny arms and legs.
The mascot looks a little disappointed and is holding up both hands in an "X" gesture.
On the right side: a big closed golden padlock icon with a soft red glow, and large
bold rounded Japanese text that reads "ロック中" on the top line and smaller text
"500てんとってね" below it.
Background: deep charcoal (#111318) filled edge to edge, with faint red neon glow
around the padlock and a few small gachapon capsule silhouettes in the dark corners.
Flat vector illustration style, thick bold shapes, high contrast, very large readable
text, no white border, no frame.
```

### 2. 解錠中 — `src_unlocked.png`

500点達成して解錠された状態。「今すぐ回せる!」というワクワク感を。

```
A wide horizontal banner illustration (aspect ratio about 1.9:1) for a small gacha
machine display. On the left: a cute mascot character shaped like a white TV remote
control with 12 small colorful round buttons on its body (red, green, blue, orange,
lime, purple, yellow, sky blue, pink), simple smiling dot eyes, tiny arms and legs.
The mascot is cheering with both arms raised and sparkles around it.
On the right side: a big open padlock icon glowing bright green, a colorful gachapon
capsule (half red half clear) popping out with motion lines, and large bold rounded
Japanese text that reads "かいじょ!" on the top line and smaller text "まわしてね"
below it.
Background: deep charcoal (#111318) filled edge to edge, with bright green and cyan
neon glow radiating from the center, confetti and small colorful capsules flying.
Flat vector illustration style, thick bold shapes, high contrast, very large readable
text, energetic and celebratory, no white border, no frame.
```

### 3. ありがとう — `src_thanks.png`

カプセルが出たあと数秒だけ表示され、その後ロック中に戻る。

```
A wide horizontal banner illustration (aspect ratio about 1.9:1) for a small gacha
machine display. On the left: a cute mascot character shaped like a white TV remote
control with 12 small colorful round buttons on its body (red, green, blue, orange,
lime, purple, yellow, sky blue, pink), simple smiling dot eyes, tiny arms and legs.
The mascot is happily waving one hand and winking, hugging a colorful gachapon capsule
with the other arm.
On the right side: large bold rounded Japanese text that reads "ありがとう!" with a few
heart shapes and sparkles around it.
Background: deep charcoal (#111318) filled edge to edge, with warm pink and gold neon
glow, soft confetti falling.
Flat vector illustration style, thick bold shapes, high contrast, very large readable
text, cheerful, no white border, no frame.
```

---

## 参考: マスコットの元画像

`pc_game/static/img/mascot.png` が既存のマスコットです。
生成AIにこの画像を添付して「このキャラクターを使って」と指示すると、
見た目が揃いやすくなります。
