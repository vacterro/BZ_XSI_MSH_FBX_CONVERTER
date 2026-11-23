# bzconv — Battlezone XSI/MSH コンバーター

Battlezone 2 の XSI / DOCB MSH モデルを扱う Windows C++20 コンバーターです。
GLB / glTF / OBJ はネイティブ書き出し、FBX はインストール済み Blender
4.x/5.x と同梱ブリッジを使用します。

ポータブル版は `release/README.txt`、CLI のヘルプは `bzconv --help`。

現在のバージョン: **v0.0.1**

検証ステータスは明確に区別されます: `FILE_WRITTEN`、`STRUCTURALLY_VALID`、
`GEOMETRY_VERIFIED`、`FULL_SCENE_VERIFIED`、`PARTIAL_LOSS`。
