<img src="NXShot.png" width="600">


# NXDiscord 1.0.1

Nintendo Switch(CFW / Atmosphère)で動く常駐プログラム(sysmodule)です。
**Switch単体で**、ゲームの開始・終了と、撮ったスクリーンショットをDiscordのWebhookへ送ります。PCは要りません。

*English: A tiny Atmosphère sysmodule that posts game start/stop notifications and new screenshots straight to a Discord webhook. No PC needed. See the Japanese text below for install steps; the layout is the same as the SD card tree shown there.*

## できること
- ゲームの開始 / 終了(プレイ時間つき)の通知
- スクリーンショットの自動投稿(**起動後に撮ったもの**だけ。起動時にすでにあった写真は送りません)
- 起動すると「🟢 NXDiscord 起動」が届きます(Albumを読めたかも書いてあります)

## 必要なもの
- Atmosphère が入ったSwitch
- Discordのサーバーで作ったWebhook URL(サーバー設定 → 連携サービス → ウェブフック)
- Wi-Fi(スクショの保存先はSDカード)

## インストール
1. [Releases](../../releases) から `NXDiscord-1.0.1.zip` をダウンロードして展開します。
2. 中の `atmosphere` と `config` の2つのフォルダを、SDカードの直下にコピーします。
3. SDカードの `config/nxdiscord/config.txt` を開き、`[ここにWebhook URLを貼る]` の部分を、自分のWebhook URLに書き換えて保存します(カッコも消して、URLだけにします)。
4. Switchを再起動します。起動してしばらくすると、Discordに「🟢 NXDiscord 起動」が届きます。

SDカードの中身はこうなります。

    atmosphere/contents/4200000000000A7E/exefs.nsp
    atmosphere/contents/4200000000000A7E/flags/boot2.flag   (空ファイル)
    config/nxdiscord/config.txt
        1行目: DiscordのWebhook URL
        2行目: (省略可)Albumの場所。通常は書かない

**すでに自分の `config.txt` がある人へ**: 上書きしないでください。`config` フォルダはコピーせず、`atmosphere` だけを入れ替えれば、設定はそのまま使えます。

## アンインストール
`atmosphere/contents/4200000000000A7E` フォルダと、`config/nxdiscord` フォルダを削除してください。

## うまく動かないとき
`config/nxdiscord/log.txt` を見てください。起動のたびに作り直されます(各行の先頭は起動からの秒数)。

| ログの内容 | 意味 |
|---|---|
| `main: start v1.0.1` が無い | 常駐が起動していません。`exefs.nsp` と `boot2.flag` の場所を確認し、Switchを完全に再起動 |
| `network: 未接続` のまま | Wi-Fiにつながっていません |
| `curl: global_init=0` 以外 | 通信ライブラリの初期化失敗 |
| `postJson` / `postShot` の `http 200〜204` | 送信成功 |
| `http 401` / `404` | Webhook URLが間違っている、または削除された |
| `http 413` | 画像が大きすぎる(8MB超は送りません) |

Webhook URLは `log.txt` には書き込まれません。

## 注意
- **セキュリティ**: SwitchにCA証明書の束が無いため、サーバー証明書の検証は行いません。通信経路を偽装される環境(信用できない公衆Wi-Fiなど)では使わないでください。
- `config.txt` にはWebhook URLが入ります。URLを知っている人は誰でもそのチャンネルに投稿できます。SDカードを人に渡すときや、スクショ・ログを公開するときは注意してください。
- CFW(カスタムファームウェア)上でオンライン機能を使うと、BANされるリスクがあります。自己責任で使ってください。このソフトはNintendoの公式サービスには接続せず、Discordにだけ通信します。
- 本ソフトはNintendoおよびDiscordとは無関係の非公式ツールです。
- 画像はDiscordへ送られます。写り込んだ個人情報や、送る相手には気をつけてください。

## ビルド
devkitPro(`devkita64`)に libnx と switch-curl が入った環境で次を実行します。

    cd sysmodule
    make

`sysmodule/out/nxdiscord.nsp` ができるので、`exefs.nsp` という名前でSDカードに置きます。
GitHub Actions(`.github/workflows/build.yml`)でもビルドできます。Releaseを公開すると、配布用zipが自動で添付されます。

## ライセンス
MIT License(`LICENSE` を参照)。
