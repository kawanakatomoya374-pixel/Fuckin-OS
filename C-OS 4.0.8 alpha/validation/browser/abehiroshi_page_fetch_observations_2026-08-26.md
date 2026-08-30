# 阿部寛ホームページ取得観察

- 対象URL: `https://abehiroshi.la.coocan.jp/`
- ブラウザ側タイトル: `阿部寛のホームページ`
- テキスト抽出は文字化けを含み、画像URLを返さなかった。
- ページの生HTMLは `/home/ubuntu/upload/abehiroshi.la.coocan.jp__1787752378429.html` に保存された。
- ブラウザのスクリーンショットアップロードは失敗したため、配色の基準はHTMLのasset参照とホスト取得した原画像で確認する必要がある。

## 画像assetと目視基準

`top.htm` は背景に `image/abehiroshi.jpg`、人物表示に `abe-top-20190328-2.jpg` を使用する。人物assetは350×414の3成分baseline JPEGであり、白背景、自然な暖色の肌、黒に近い髪とジャケット、淡い灰青色のシャツが基準となる。背景assetは400×100の3成分baseline JPEGであり、白地に淡いミントグリーンの `ABE Hiroshi` ロゴを表示する。

この二つは透明度を持たない通常JPEGであるため、色異常はalpha合成ではなく、JPEG decoderが渡すRGBA/XBGR byte orderとNetSurf plotterの32bpp変換の不一致を最優先で検査する。
