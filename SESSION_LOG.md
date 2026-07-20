# Session Log

このファイルは、どのセッション/どのマシンからでも「今何をしているか」を共有するための進捗ログです。
新しいセッションを始めるときはまずこのファイルを読んで直近の状況を確認し、作業を終える前に新しいエントリを一番上に追記してください(過去のエントリは消さない)。

フォーマット:

```
## YYYY-MM-DD HH:MM - 短いタイトル
- やったこと
- 現在の状態
- 次にやること
```

## Entries

## 2026-07-20 17:40 - プロジェクト初期設定
- このフォルダ (/Users/kuramochikeito/Desktop/hobby/sensor) 以外への Edit/Write を禁止するフックを .claude/settings.json に追加
- セッションごとに本ログの更新を促す/強制する SessionStart・Stop フックを追加
- GitHub リポジトリ https://github.com/KeitoKuramochi/sensor_infrared.git にプッシュする運用ルールを CLAUDE.md に記載
- 次にやること: git init して初回コミット・プッシュ
