# 素材(元データ置き場)

撮影した写真・動画の**元データ**と、生成AIで作った画像の**元ファイル**を置く場所。
合計 780MB ほどあるので **git には上げない**(このREADMEだけコミットする)。

公開ページに載っている画像は、ここから縮小・トリミングして書き出した加工済みのもの。
加工済みのほうは `site/images/`・`opencampus/images/`・`pc_game/static/img/` にコミット済み。

```
素材/
├── 写真_製作/              製作過程(2026-07-20 〜 07-27 撮影)
├── 写真_オープンキャンパス/   当日のガチャ機(2026-08-02 撮影)
├── 動画/                   デモ動画の4K元データ(3本で750MB)
├── AI生成画像/             Nano Banana などで生成した元画像
├── 印刷物/                 A4ポスター
└── _プロジェクト外/         このプロジェクトと関係ない写真(下記の注意参照)
```

---

## 元のファイル名との対応

`SESSION_LOG.md` には `IMG_6493` のような元の番号で書かれているので、探すときはこの表を使う。

### 写真_製作/

| 今の名前 | 元の名前 | 加工後の掲載先 |
|---|---|---|
| 01_LEDライトの基板.jpeg | IMG_6308.jpeg | `site/images/led-pcb.jpg` |
| 02_ESP32_OLED付き.jpeg | IMG_6309.jpeg | `site/images/esp32-oled.jpg` |
| 03_リモコン_12色ボタン.jpeg | IMG_6310.jpeg | `site/images/remote.jpg` |
| 04_部品ぜんぶ.jpeg | IMG_6311.jpeg | `site/images/parts-all.jpg` |
| 05_100均の他の商品.jpeg | IMG_6312.jpeg | `site/images/daiso-others.jpg` |
| 06_配線その1.jpeg | IMG_6313.jpeg | `site/images/wiring-1.jpg` |
| 07_配線その2.jpeg | IMG_6314.jpeg | `site/images/wiring-2.jpg` |
| 08_はんだ付け後の基板.jpeg | IMG_6315.jpeg | `site/images/led-pcb-soldered.jpg` |
| 09_はんだ付けした線.jpeg | IMG_6494 2.jpeg | `site/images/soldered-wires.jpg` |
| 10_パッケージ_LEDライト330円.jpeg | IMG_6290 2.jpeg | `site/images/package-light.jpg` |
| 11_パッケージ_人感センサー330円.jpeg | IMG_6291 2.jpeg | `site/images/package-sensor.jpg` |
| 12_ジャンパーワイヤー_オスメス.jpeg | 68_ジャンパーワイヤーオスメス_1.jpeg | `site/images/jumper-wires.jpg` |

※ 01〜12 はすべて `opencampus/images/` にも同じ名前で入っている。

### 写真_オープンキャンパス/(2026-08-02 撮影)

| 今の名前 | 元の名前 | 加工後の掲載先 |
|---|---|---|
| 01_ガチャ機ディスプレイ_ロック中.jpeg | IMG_6540.jpeg | `opencampus/images/gacha-display.jpg` |
| 02_ガチャ機のESP32と配線.jpeg | IMG_6541.jpeg | `opencampus/images/gacha-esp32.jpg` |
| 03_排出口の赤外線センサー.jpeg | IMG_6542.jpeg | `opencampus/images/gacha-ir-sensor.jpg` |
| 04_サーボと回転盤の歯.jpeg | IMG_6543.jpeg | `opencampus/images/gacha-servo-lock.jpg` |

⚠️ **01 の背景に来場者が写っている。** 掲載版はトリミングで除去済み。
このフォルダの元データのほうは未処理なので、そのまま公開しないこと。

### 動画/

| 今の名前 | 元の名前 | サイズ | 加工後の掲載先 |
|---|---|---|---|
| 01_デモ_初代_4K.MOV | IMG_6493 2.MOV | 174MB | `site/videos/demo.mp4`(720p 4.1MB) |
| 02_デモ_BLE版_4K.mov | IMG_6501.mov | 262MB | `site/videos/demo-bt.mp4`(縦720x1280 6.5MB) |
| 03_デモ_オープンキャンパス_4K.MOV | IMG_6539.MOV | 317MB | `opencampus/videos/demo.mp4`(720p 約10MB) |

※ 02 は `rotation: -90` のメタデータ付きの**縦動画**。`scale=1280:720` で変換すると潰れる。

### AI生成画像/

- `ゲーム画面/` — `pc_game/static/img/` にある加工済み(背景透過・余白トリミング)の元
- `ガチャ機ディスプレイ/` — ガチャ機の液晶用。`firmware/gacha_lock_ble/src_*.jpg` として使われ、
  `make_images.py` が RGB565 の `images.h` に変換する
- `ライフ_重複.jpg` は `ライフ.jpg` と中身が同じ(生成時のダウンロード重複)。消してよい

生成に使ったプロンプトは `pc_game/static/img/PROMPTS.md` と
`firmware/gacha_lock_ble/PROMPTS.md` に残してある。

### 印刷物/

- `オープンキャンパス_QRポスターA4.pdf` — 解説ページのQRを貼ったA4ポスター。
  作り方は `opencampus/POSTER_PROMPT.md`

### _プロジェクト外/

このプロジェクトと関係のない写真。ルートに紛れ込んでいたので隔離した。

- `Web3AI概論のクリアファイル.jpeg`(元 `IMG_6491 2.jpeg`)
  — **氏名入りのステッカーが写り込んでいる。git には絶対に上げないこと。**
  不要なら消してよい。
