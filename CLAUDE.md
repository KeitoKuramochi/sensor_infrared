# sensor_infrared プロジェクトルール

## 作業範囲の制限
- このプロジェクトフォルダ (`/Users/kuramochikeito/Desktop/hobby/sensor`) 以外のファイルを編集・作成しないこと。
- `.claude/settings.json` の `PreToolUse` フックにより、フォルダ外への Edit/Write はシステムレベルでブロックされる。
- Bash コマンドについては自動ブロックの対象外なので、フォルダ外のファイルを操作するコマンド (rm, mv, cp, リダイレクトなど) は実行しないこと。

## セッションログ (SESSION_LOG.md)
- 各会話セッションで何をしているか/何をしたかを `SESSION_LOG.md` に必ず記録する。これにより、どのセッション・どのマシンから見ても現在の作業状況が分かる。
- セッション開始時: `SESSION_LOG.md` を読み、直近の状況を把握してから作業を始める。
- セッション終了前: `SESSION_LOG.md` の Entries セクションの一番上に新しいエントリを追記する(過去のエントリは削除しない)。
- `.claude/settings.json` の `Stop` フックが、そのセッション内で `SESSION_LOG.md` がまだ更新されていない場合はセッション終了をブロックし、記録を促す。

## GitHub への反映
- リポジトリ: https://github.com/KeitoKuramochi/sensor_infrared.git
- 作業内容は都度 `git add` / `git commit` / `git push` して構わない(ユーザーから包括的に許可済み)。毎回細かくプッシュしてよい。
- push 先は常にこのリポジトリの `main` ブランチとする。
