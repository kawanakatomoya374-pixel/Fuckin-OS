"""class定義を関数本体に書いた場合の診断テスト（未対応）。

Cには関数の入れ子定義が無く、生成コードが不正なCになるため、
黙って壊れたCを出さずにtranspilerが診断する。
"""


def build():
    class Local:
        value = 1

    return Local.value


print(build())
