# ゲーム用画像の生成プロンプト集(Nano Banana / Gemini用)

生成した画像を**このフォルダ(`pc_game/static/img/`)に下記のファイル名で保存**して、
ブラウザを再読み込みするだけでゲームに反映されます(サーバー再起動は不要)。

全部の画像で統一感が出るよう、各プロンプトの末尾に共通のスタイル指定を入れてあります。
仕上がりが好みでなければ「もっとポップに」「もっとレトロゲーム風に」など追記して調整してください。

---

## ゲームに自動反映される4枚(ファイル名厳守)

### 1. 背景 — `bg.png`(横長 16:9 推奨)

```
A wide dark background for a rhythm game screen. Mostly deep charcoal (#111318) with
subtle glowing neon shapes near the edges: faint equalizer bars, soft bokeh light dots,
and thin light streaks in rainbow colors (red, green, blue, orange, purple, pink).
The center must stay dark and empty so game UI stays readable.
Flat vector illustration style, neon glow, vibrant but not busy, no text, no characters.
```

メモ: 中央が明るいとノーツが見にくくなるので「center must stay dark」が重要。
うるさく感じたら "even more subtle, 80% of the image is plain dark" を足す。

### 2. タイトルロゴ — `logo.png`(横長、文字入り)

```
A video game title logo that says "COLOR MEMORY GAME" in bold rounded arcade lettering.
Each word in a different vivid color (red, cyan, yellow), with a soft neon glow and a
subtle outline. Small colorful music notes and a tiny remote controller icon decorating
the text. Flat vector style on a plain dark charcoal (#111318) background, high contrast,
centered composition.
```

メモ: 文字が崩れたら「spelling exactly "COLOR MEMORY GAME"」と念押しして再生成。

### 3. ゲームオーバー画面 — `gameover.png`(横長)

```
"GAME OVER" in big broken neon sign lettering, red and purple glow, slightly flickering
look, a sad little cartoon remote controller character with X eyes sitting below the text.
Dark charcoal (#111318) plain background, flat vector illustration, neon glow, no other text.
```

### 4. 全ステージクリア画面 — `allclear.png`(横長)

```
"ALL CLEAR!" in huge celebratory rainbow neon lettering with confetti, fireworks and
sparkling stars bursting around it, a happy cartoon remote controller character jumping
with joy below the text. Dark charcoal (#111318) plain background, flat vector
illustration, vivid neon glow, joyful, no other text.
```

---

## おまけ(ゲームには自動反映されないけど、記事やOGP・今後の拡張用)

### 5. マスコットキャラ(リモコンちゃん)

```
A cute mascot character design: a small white infrared remote controller with a smiling
face, stubby arms and legs, holding a glowing music note. 12 colorful round buttons on
its body arranged in a 3x4 grid. Flat vector sticker style, thick outline, dark charcoal
background, front view, full body, no text.
```

### 6. EXCELLENT判定のバースト演出

```
A radial burst effect for a rhythm game "perfect hit": golden yellow starburst with
sparkles and thin speed lines radiating from the center, transparent-looking edges
fading to dark charcoal (#111318) background. Flat vector style, no text.
```

### 7. ステージクリアのスタンプ

```
A round rubber-stamp style badge that says "STAGE CLEAR!" in bold letters, teal and lime
neon colors, small stars around the rim, slightly tilted like a stamped seal. Flat vector
style on plain dark charcoal (#111318) background, no other text.
```

### 8. 記事・OGP用のキービジュアル(横長 1200x630)

```
A hero illustration for a DIY electronics article: a 100-yen shop LED light remote
controller shooting a glowing infrared beam into a small ESP32 board with an OLED screen,
which sends colorful music notes flying toward a laptop showing a rhythm game lane.
Playful flat vector illustration, neon accents on dark charcoal background, no text.
```

### 9. 12色ノーツのアイコンセット(将来ノーツを画像化するなら)

```
A set of 12 glossy round game orbs in a neat 4x3 grid, colors: red, green, royal blue,
orange, lime, violet, yellow, sky blue, magenta, light pink, blue, dark indigo.
Each orb has a soft inner glow and a small highlight, identical size and style.
Flat vector style on plain dark charcoal (#111318) background, no text.
```

### 10. ライフのハートアイコン

```
A cute pixel-art style glowing red heart game icon with a thin neon outline and a small
shine, single object centered on plain dark charcoal (#111318) background, no text.
```

---

## 使い方まとめ

1. Nano Banana(Gemini)にプロンプトを貼って生成
2. 気に入ったら PNG で保存し、このフォルダに指定のファイル名で置く
   (1〜4のファイル名は `bg.png` `logo.png` `gameover.png` `allclear.png` 厳守)
3. ブラウザでゲームページを再読み込み → 反映
