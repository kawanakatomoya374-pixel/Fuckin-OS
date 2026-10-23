# QMP GUI click findings

Secure Boot + UEFI + SMP8のQEMU画面をクリック前後で比較した。両画像とも1024x768で、デスクトップアイコンとタスクバーが同一状態のままだった。NetSurfアイコンは画面上部の2列目、概ねx=136, y=53付近にある。今回の相対移動(205,345)は中央付近からNetSurfアイコンへ到達せず、ドック判定領域をクリックしていない。serialにもNetSurf/Browser起動ログは出なかった。

次の試験ではPS/2相対入力の開始位置を考慮し、NetSurfアイコン中心へ到達する移動量を使う。クリック後は前後screendumpを取得し、ウィンドウ表示差分とserialのbrowser lifecycleログを同時に判定する。
