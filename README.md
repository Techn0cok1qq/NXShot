<img src="assets/icon.png" width="200">

# NX PCK Studio

[PCK-studio-Web](https://github.com/Techn0cok1qq/PCK-studio-Web) と同じ画面構成(メニュー / アセット一覧 / プレビュー / アセット詳細)で、
スキンパックの `.pck` を作って次の場所に置く Switch 自作ソフトです。

    sd:/atmosphere/contents/01006BD001E06000/romfs/NX/DLC/<パック名フォルダー>/<ファイル名>.pck

PCKの書き出しは Web PCK Editor / PCK-studio-Web の処理を移植したもので、GPL-3.0 です。

## 使い方
1. `nxpckstudio.nro` を `sd:/switch/` に置いて起動(初回に `sd:/switch/nxpckstudio/` が作られ、サンプル画像が `images/` に入ります)。
2. 自分のスキン(64x64 / 64x32、**128x128 / 256x256 / 512x512 の展開図も可**)のPNGを `sd:/switch/nxpckstudio/skins/` に、マント(64x32)のPNGを `capes/` に入れる。
3. 「スキン追加」→ 画像を選ぶ(**選んでいる間、右に展開図と3Dプレビューが出ます**)→ スキン名を入力。左の一覧にアセット(スキン・マント・localisation.loc)が並びます。
4. 一覧でアセットを選び、右下の「アセット詳細」で名前・テーマ名・スキンID・モデル・マント・プロパティ(キー/値の編集・追加・削除)を変更。
5. 「保存して配置」→ ① パックのフォルダー名 ② PCKファイル名 を入力すると、上の場所に書き出します。
6. ゲームを再起動。

## 大きなスキン (128 / 256 / 512) と展開図・3Dプレビュー (v1.0.3)
- **128x128 / 256x256 / 512x512** のスキン(と、幅が高さの2倍の 128x64 / 256x128 / 512x256)も、`skins/` に入れたり Discord で送ったりして使えます。
  **縮小はせず、画像をそのままスキンに入れます**(使えるかどうかはゲーム側しだい)。画像が 32767 バイトを超えると、一覧が赤くなり、保存時に確認します。
- 「スキン追加」と、詳細の「スキン画像 → 置き換える」の画像選択は、右に **展開図** と **3Dプレビュー** を出します。
  - 展開図: 選んでいる画像と、頭・胴・腕・脚の枠(黄色=1層目、水色=2層目)。`L` か展開図をタッチで枠の ON/OFF。
  - 3Dプレビュー: ドラッグ / 右スティックで回転、`R` かタッチで自動回転、`ZL` / `ZR` で拡大縮小 (大きな画像もそのままの解像度で貼ります)。
- メニューの「取り込み」(ほかの PCK からスキンを取り込む)の一覧も、選んでいるスキンの展開図と3Dプレビューを出します (BOX / OFFSET プロパティやマントも反映)。
- Discord 同期のあとに「スキンを確認」を選ぶと、今回取り込んだスキンを展開図 / 3Dプレビューで見て回れます。

## アニメーションスキン (上級者向け)
ホーム画面の「アニメーションスキンを作成(上級者向け)」は、Animation-Cape-SkinPack-Generator をこのアプリに移植したものです。
**GIF の各コマ**を `TemplateCapes.png` に合成してマントにし、**スキンと1コマずつ**組み合わせたパックを作ります。スキンも**連番のアニメーション**にできます。

1. アニメーションの GIF を `sd:/switch/nxpckstudio/anim/` に入れる(Discord 同期でも入ります)。
2. スキンの PNG(幅=高さ、または幅=高さ×2)を `skins/` に、または**連番スキンを入れた ZIP**(`skin_01.png` … `skin_20.png` など)を `skins/` か `anim/` に入れる。
3. ホーム →「アニメーションスキンを作成(上級者向け)」→ GIF を選ぶ(「GIFなし」ならスキンだけのパック)→ スキンを選ぶ。
   PNG も ZIP も複数選べます。ZIP は中の PNG を取り出し、すべて名前の数字順 (`skin_2` < `skin_10`) に並べます (`__MACOSX` や `._` で始まるファイルは無視)。
4. パック名とスキン名(後ろに `01`, `02`… が付きます)を入力すると、エディターにパックができます。
5. **再生**: プレビューの「再生」ボタン(タッチ)か**左スティックの押し込み**で、スキンとマントが 0.1 秒ごとに順に切り替わります。A / B / もう一度押し込みで停止。
6. 「保存して配置」(−)で書き出す。

- スキンとマントの数が違うときは、少ない方を繰り返して多い方に合わせます (GIFなしならスキンだけ)。
- 各コマは PNG が 35KB 以下になるまで自動で縮小します。マントの画像は 384x192 です。
- GIF は先頭の 300 コマまで使います(メモリー保護)。MP4 は Switch 上では読めないため非対応です (PC で GIF に変換してください)。
- 再生は開いているどのパックでも使えます(スキンが 2 つ以上あるとき)。
- PC での確認用に `tests/test_anim.cpp` (GIF → マント PNG) と `tests/test_zip.cpp` (ZIP 読み込み) があります。

## 操作 (ボタン / タッチどちらも可)
| 場所 | 操作 |
|---|---|
| 一覧 | ↑↓ 選択 / A か → で詳細へ / X スキン追加 |
| 詳細 | ↑↓ 選択 / A 編集 / X キー編集・マント解除 / Y プロパティ削除 / B か ← で一覧へ |
| メニュー | L で出入り / ←→ 選択 / A 実行 |
| 共通 | + 終了(未保存なら確認) / 左スティック押し込み: プレビュー再生 |
| スキン選択 / 取り込み | ↑↓ 選択 / A 決定 / B 戻る / L 展開図の枠 / R 自動回転 / ZL・ZR 拡大縮小 / 右スティック 回転 |

画像は 32767 バイト以下を推奨(超えるとゲームが落ちる恐れがあるため、一覧が赤くなり、保存時に確認します)。
`sd:/switch/nxpckstudio/config.ini` では `endian=little|big` と、アップデートの設定だけ変えられます(出力先は固定)。

## アップデート (アプリ内)
GitHub の「最新リリース」を見つけて、アプリの中だけで更新できます (PC もブラウザーも不要)。

- 起動すると裏で最新版を確認します。新しい版があれば画面下に表示され、メニューの「更新」(ホーム画面は右上のボタン / Y)に赤い印が付きます。
- 「更新」→ 内容を確認 →「更新する」でダウンロード → 「更新して再起動」で、`.nro` を差し替えて自動で起動し直します。
- ダウンロードしたファイルは サイズ / NROヘッダー / SHA-256(GitHub が返すとき)を確かめてから使います。通信は HTTPS で、証明書も検証します。
- 失敗したときは元の `.nro` がそのまま残ります。起動時の自動確認は `config.ini` の `update_check=off` で止められます。
- 更新を探す場所は `config.ini` の `update_repo=<ユーザー名>/<リポジトリ名>` です (既定: `cokacolastic/NX-PCK-Studio`)。

### 新しい版を出す手順
    git tag v1.0.3
    git push origin v1.0.3
GitHub Actions がビルドして Release を作り、`nxpckstudio.nro` を添付します。アプリのバージョンもタグ (`1.0.3`) になります。
普段の push はビルドだけで、Release は作りません。

## アイコン
`icon.jpg` (256x256) が `.nro` に入ります。元の PCK Studio の書類アイコンに NX バッジを足したもので、
`python3 tools/make_icon.py` (Pillow が必要) で作り直せます。透過 PNG は `assets/icon.png`。

## フォント
HTML の `font-family: MCFont → Pixelify Sans → DotGothic16` と同じ順で、1文字ずつ使えるフォントを探して描きます
(どれにも無い文字は本体の共有フォント)。MCFont は `romfs/minecraft.ttf`。Pixelify Sans / DotGothic16 は
GitHub Actions のビルド時に Google Fonts(OFL)から自動で `romfs/` に入ります。手元でビルドする場合は
`romfs/PixelifySans.ttf` と `romfs/DotGothic16.ttf` を自分で置いてください。

## ビルド
GitHub に push すると Actions が `nxpckstudio.nro` を作ります(devkitPro: switch-sdl2 / sdl2_image / sdl2_ttf)。

## Discord 同期
Discord のチャンネルに貼ったスキン / マントの PNG と、アニメーション用の GIF / ZIP を、メニューの「Discord」(ホーム画面は X)で SD に取り込みます。
PC もブラウザーも使わずに、スマホなどから送った画像でスキンが作れます。

### 準備 (最初の1回だけ)
画面の表記は Discord の更新で変わることがあります。
1. [Discord Developer Portal](https://discord.com/developers/applications) で「New Application」を作る。
2. 左の「Bot」で「Reset Token」を押して Token をコピーする (人に見せないこと)。
3. 同じ「Bot」の画面で **Message Content Intent を ON** にする (OFF だと他の人の投稿の添付が空になります)。
4. 「OAuth2 → URL Generator」で `bot` を選び、権限は「View Channels」「Read Message History」。できた URL を開いてサーバーに招待する。
5. Discord の「設定 → 詳細設定 → 開発者モード」を ON にして、画像を貼るチャンネルを右クリック →「チャンネルIDをコピー」。
6. `sd:/switch/nxpckstudio/config.ini` に書く:

       token=<コピーした Token>
       channel=<チャンネルID (数字)>

### 使い方
チャンネルに PNG を貼る → アプリのメニュー「Discord」(ホーム画面は X)。直近 100 件のメッセージから、まだ取っていない PNG を保存します。
同期のあと「スキンを確認」で、取り込んだスキンを展開図 / 3Dプレビューで見られます。
GIF と ZIP (16MB まで) は `sd:/switch/nxpckstudio/anim/` に保存されます(アニメーションスキン用)。Discord の「GIF を送る」ボタンで送った GIF は添付ではなくリンクなので取り込めません。GIF ファイルをアップロードしてください。
- 64x64 → `skins/`
- **128x128 / 256x256 / 512x512 → `skins/`** (展開図。そのまま使います)
- 128x64 / 256x128 / 512x256 → `skins/` (マントとは見なしません)
- 64x32 でファイル名に `cape` / `マント` が入っている → `capes/`、`skin` / `スキン` が入っている → `skins/`
- 64x32 で名前からは分からない → `skins/` と `capes/` の両方 (古い形式のスキンにもマントにもなるため)
- 上以外の大きさ、4MB を超える画像 → 取りません
- 保存した画像の ID は `discord_seen.txt` に控えます。SD から消した画像を、次の同期で取り直すことはありません
  (取り直したいときは `discord_seen.txt` を消す)。
- Token は SD の `config.ini` に平文で入ります。Bot は「読むだけ」の権限にして、SD カードを人に渡すときは Token を作り直してください。

## PC上のテスト (devkitPro 不要)
    cd tests && g++ -std=c++17 -o test_pck test_pck.cpp && ./test_pck skin.png cape.png out.pck
    # verify.js は PCK-studio-Web の index.html から PckParser/LOCParser を抜き出して使います
    sed -n '/class PckParser/,/UI コントローラ/p' /path/to/PCK-studio-Web/index.html > parsers.js
    node verify.js out.pck
    # アップデート確認のロジック (JSON / バージョン比較)
    g++ -std=c++17 -I../source -o test_update test_update.cpp && ./test_update
    # Discord 同期のロジック (添付の取り出し / スキンかマントかの判定 / 64〜512 の大きさ)
    g++ -std=c++17 -I../source -o test_discord test_discord.cpp && ./test_discord

## BGM
内蔵の曲(オリジナル)が流れます。`sd:/switch/nxpckstudio/bgm/` に mp3 / wav / ogg を入れると、その曲が順番に流れます。
メニューの「BGM」でオン/オフ、`config.ini` の `bgm=off` で起動時から切れます。

## 操作の追加
十字キーと左スティックは長押しで連続移動、右スティックは3Dプレビューの回転、`−` は「保存して配置」です。
