<img src="NXShot.png" width="200">

# NXDiscord 1.1.2

Nintendo Switch(CFW / Atmosphère)で動く常駐プログラム(sysmodule)です。
**Switch単体で**、ゲームの開始・終了と、撮ったスクリーンショットや動画をDiscordのWebhookへ送ります。PCは要りません。

*English: A tiny Atmosphère sysmodule that posts game start/stop notifications and new screenshots/videos straight to a Discord webhook. No PC needed. See the Japanese text below for install steps; the layout is the same as the SD card tree shown there.*

## できること
- ゲームの開始 / 終了(プレイ時間つき)の通知
- スクリーンショットと動画(mp4)の自動投稿(**起動後に撮ったもの**だけ。起動時にすでにあったものは送りません)
- 大きすぎる動画は、**最初のほうを自動で削って**上限以内に収めて送ります(画質は落ちません。下の「動画について」)
- 起動すると「🟢 NXDiscord 起動」が届きます(Albumを読めたかも書いてあります)

## 必要なもの
- Atmosphère が入ったSwitch
- Discordのサーバーで作ったWebhook URL(サーバー設定 → 連携サービス → ウェブフック)
- Wi-Fi(スクショの保存先はSDカード)

## インストール
1. [Releases](../../releases) から `NXDiscord-1.1.2.zip` をダウンロードして展開します。
2. 中の `atmosphere` と `config` の2つのフォルダを、SDカードの直下にコピーします。
3. SDカードの `config/nxdiscord/config.txt` を開き、1行目の `[ここにWebhook URLを貼る]` を、自分のWebhook URLに書き換えて保存します(カッコも消して、URLだけにします)。2行目の `limit=10` はそのままで大丈夫です(下の「動画について」)。
4. Switchを再起動します。起動してしばらくすると、Discordに「🟢 NXDiscord 起動」が届きます。

SDカードの中身はこうなります。

    atmosphere/contents/4200000000000A7E/exefs.nsp
    atmosphere/contents/4200000000000A7E/flags/boot2.flag   (空ファイル)
    config/nxdiscord/config.txt
        1行目: DiscordのWebhook URL
        2行目以降(省略可):
            limit=10          送信できるファイルの上限(MB)
            trim=1            上限を超える動画の最初を削って送る(0で、削らず「送れなかった」と通知だけ)
            album=sdmc:/...   Albumの場所。通常は書かない

**すでに自分の `config.txt` がある人へ**: 上書きしないでください。`config` フォルダはコピーせず、`atmosphere` だけを入れ替えれば、設定はそのまま使えます。

## 動画について
- Switchで撮った動画(最長30秒)も、スクリーンショットと同じように自動で送ります。
- Discordには、アップロードできるファイルの大きさに**上限**があります(ブーストなしのサーバーでは約10MB。Discord側の都合で変わることがあります)。Switchの動画はこれを超えることがあります。
- 動画が上限(`limit=`)を超えるときは、**最初のほうを削って**上限以内に収めてから送ります。
  - 動画を作り直さず、前の部分を切り捨てるだけなので、画質も音も元のままです。
  - 切れるのはキーフレームの位置(約0.25秒ごと)なので、削る長さは少し大きめになることがあります。
  - 送った動画のメッセージに、「最初の◯秒を削って送りました」と書きます。元の動画は変わりません。
  - 削っても収まらない(残りが2秒未満になる)場合や、削りたくない場合(`trim=0`)は、「送れなかった」とだけ通知します。
- `limit=` 以下でも、Discordのサーバーに断られた場合(http 413など)は、「送信失敗」と通知します。この場合は `limit=` を、実際に送れた大きさまで下げてください。
- ブーストしているサーバーなら、`config.txt` の `limit=` を大きくすると、大きい動画も送れます(例: `limit=50`)。サーバーの上限より大きくすると、送信に失敗します。
- 動画は送るのに時間がかかります。その間は、ゲームの開始・終了の通知が少し遅れることがあります。

## 開発者向け
動画を削る処理は `sysmodule/source/mp4trim.h` に、標準ライブラリだけで書いてあります。PCでも試せます。

    g++ -std=c++17 -O2 -o mp4trim_test tests/mp4trim_test.cpp
    ./mp4trim_test 入力.mp4 出力.mp4 上限バイト

## アンインストール
`atmosphere/contents/4200000000000A7E` フォルダと、`config/nxdiscord` フォルダを削除してください。

## うまく動かないとき
`config/nxdiscord/log.txt` を見てください。起動のたびに作り直されます(各行の先頭は起動からの秒数)。

| ログの内容 | 意味 |
|---|---|
| `main: start v1.1.2` が無い | 常駐が起動していません。`exefs.nsp` と `boot2.flag` の場所を確認し、Switchを完全に再起動 |
| `network: 未接続` のまま | Wi-Fiにつながっていません |
| `curl: global_init=0` 以外 | 通信ライブラリの初期化失敗 |
| `postJson` / `postShot` の `http 200〜204` | 送信成功 |
| `http 401` / `404` | Webhook URLが間違っている、または削除された |
| `http 413` | ファイルが大きすぎる(`limit=` がサーバーの上限より大きい) |
| `media: 上限超え` | `limit=` を超えるため送りませんでした |
| `trim: result=0` | 動画を削って収めました(`-2`: 未対応の形式、`-3`: 削っても収まらない) |

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
