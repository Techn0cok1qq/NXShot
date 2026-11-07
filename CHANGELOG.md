# 更新履歴

## 1.0.0
最初の公開版。

- ゲームの開始 / 終了(プレイ時間つき)をDiscordのWebhookへ通知
- SDカードのAlbumに増えた新しいスクリーンショット(jpg)を自動投稿
- Switch単体で動作(PC・常駐アプリ不要)
- 起動時にWi-Fi接続を待ってから通信を開始
- `config/nxdiscord/log.txt` に動作ログを出力(Webhook URLは書き込まない)
- HTTPS通信に必要なsslサービスを先に初期化(起動直後のcurl初期化失敗を修正)
