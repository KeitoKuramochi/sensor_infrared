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

## 2026-07-20 17:55 - 機密情報保護ルールの追加 + 初回プッシュ完了
- git init → リモート https://github.com/KeitoKuramochi/sensor_infrared.git に初回プッシュ完了(main ブランチ)
- 追加指示を受けて、.env / credentials / secrets / 鍵ファイルへの Read/Edit/Write を禁止する PreToolUse フックを追加(Claude 自身も参照不可に)
- .gitignore に機密ファイルパターンを追加、CLAUDE.md に「GitHubへ個人情報・機密情報を絶対に上げない」ルールを明記
- 現在の状態: フォルダ外操作禁止フック・.env参照禁止フック・セッションログ強制フックがすべて .claude/settings.json に設定済みで、リポジトリにプッシュ済み
- 次にやること: `.claude/` が今セッション開始時になかったため、フックを有効化するには `/hooks` を一度開くか Claude Code を再起動する必要あり(ユーザーに案内済み)。実際にセンサー関連のコード作業を始める。

## 2026-07-20 17:40 - プロジェクト初期設定
- このフォルダ (/Users/kuramochikeito/Desktop/hobby/sensor) 以外への Edit/Write を禁止するフックを .claude/settings.json に追加
- セッションごとに本ログの更新を促す/強制する SessionStart・Stop フックを追加
- GitHub リポジトリ https://github.com/KeitoKuramochi/sensor_infrared.git にプッシュする運用ルールを CLAUDE.md に記載
- 次にやること: git init して初回コミット・プッシュ
