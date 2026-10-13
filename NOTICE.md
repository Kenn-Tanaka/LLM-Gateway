# Third-Party Notices

本ソフトウェア（Copyright (c) 2026 HAIA Project、MIT License。`LICENSE` 参照）は、次のサードパーティソフトウェアを静的にリンクして `llm-gateway.exe` / `MaintenanceTool.exe` に含みます。各ライセンス全文は配布物の `THIRD_PARTY_LICENSES/` に含まれます（`scripts/package.ps1` が取得済みソースから収集）。

| 名称 | 版 | ライセンス | 全文ファイル |
|---|---|---|---|
| nlohmann/json | 3.11.3 | MIT | `nlohmann_json-LICENSE.MIT` |
| cpp-httplib | 0.18.3 | MIT | `httplib-LICENSE` |
| libcurl | 8.10.1 | curl license（MIT/X 系） | `curl-COPYING` |
| llama.cpp（ggml 含む） | b4200 | MIT | `llama-LICENSE` |
| wxWidgets | `cmake/Dependencies.cmake` 参照 | wxWindows Library Licence 3.1（バイナリ配布は独自条件で可） | `wxwidgets-licence.txt` |
| 〃 内蔵 libpng | wxWidgets 同梱 | libpng License | `wxwidgets-libpng-LICENSE` |
| 〃 内蔵 zlib | wxWidgets 同梱 | zlib License | `wxwidgets-zlib-LICENSE` |
| 〃 内蔵 PCRE | wxWidgets 同梱 | BSD（PCRE LICENCE） | `wxwidgets-pcre-LICENCE` |

ビルド構成（`CMakeCache.txt` で確認）: wxWidgets は静的ビルドで、内蔵ライブラリのうち実際にリンクされるのは libpng・zlib・PCRE のみ（expat・libjpeg・libtiff・liblzma は無効）。MSVC ランタイムは静的（`/MT`）です。

> 注: 上表は上流のライセンス文書に基づく技術的な整理であり、法的助言ではありません。wxWidgets のソースや改変版を配布する場合は LGPL 2 相当の条件が適用されます。GGUF モデルは同梱せず、各モデルのライセンスは利用者が確認してください。
