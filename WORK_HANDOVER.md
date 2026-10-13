# LLM Gateway 作業引き継ぎ書

- 作成日: 2026-10-13
- 最終更新: 2026-10-13（Phase 3: P1課題 H-04・H-08 の修正および H-13 回帰テスト実装・bin更新）
- 対象: 別のコーディングエージェントによる、この作業フォルダでの継続開発
- 状態: 旧設定の互換読込・**Phase 1（P0: H-01・H-02・H-13）**・**Phase 2（P1: H-03・H-05・H-06・H-07・H-11・H-12・H-14・H-13）**に加え、**Phase 3（P1: H-04 GUI初期設定完結・Provider/Alias CRUD・設定状態管理・旧形式別名保存・ポート跨ぎ再起動、H-08 組み込み推論Rerank厳密弱順序・KVキャッシュ毎回クリア・特殊トークン整形・encoder/decoder分岐・抽出失敗/NaNの503化・トークン長/空文書400化、H-13 Phase 3回帰テスト）を実装・検証し、`bin/` の両EXEを更新済み**。Phase 4以降の課題（H-09、H-10、SEC-L01、ARCH-L01）が次段階の対象。
- 本書は開発用・個人環境向け。一般配布物に含めない。
- 本書は次作業の提案であり、実サービス接続や残課題すべての修正を自動的に許可するものではない。引き継ぎ先では依頼者の作業指示を優先する。

## 0A. Phase 1（P0: H-01 / H-02 / H-13）実施結果（2026-10-13）

| 項目 | 実施内容・検証結果 |
|---|---|
| H-01（SSE中継・上流1回実行・エラーステータス保持） | `src/gateway/external_proxy.cpp` と `src/gateway/http_server.cpp` を修正。トップレベルJSON bool `"stream": true` のみでSSE判定（非bool/非nullは400 `bad_request`、入れ子`metadata.stream`は無視）。通常応答とSSEで共通の `UpstreamCurlTransfer`（`curl_multi` + `curl_easy`）を使用し、上流POSTは1要求につき1回のみ実行。ヘッダー完了まで先に駆動し、上流status `>= 400`（429等）や非SSE `Content-Type` はエラー本文を読み切ってstatus・ヘッダー・本文をそのまま返却。`2xx` `text/event-stream` は `set_chunked_content_provider` で逐次中継し、正常完了時 `sink.done()`、Client切断時（`!sink.is_writable()`）は即座に上流転送を中断 |
| H-02（HTTP要求カウンター・ShutdownCoordinator・GUI再起動ワーカー寿命） | `include/gateway/shutdown_coordinator.hpp` にムーブ可能なRAII `RequestGuard`、負値クランプ、所有 `shutdown_thread_`（デストラクタでcancel/join）、テスト用 `ShutdownAction` を追加。`src/gateway/http_server.cpp` で `pre_routing_handler` 内の手動 `leave_request()` と `post_routing_handler` を廃止し、`thread_local RequestGuard` を `svr_.set_logger`（応答・SSE送信完了後）で解放する構成に統一。`ProcessController` に `mutex_` と `request_cancel()` / `reset_cancel()` を追加。`MainFrame` に永続 `is_busy_`、`ui_alive_`、所有 `restart_worker_`、設定・パスの値snapshot、`~MainFrame()` でのcancel+join、Save & Restart保存成功時の `loaded_from_legacy = false` 更新を実装 |
| H-13（Phase 1 回帰テスト追加） | `tests/unit/auth_test.cpp` に合成資格情報サービス名を用いた `Authenticator`（Client/Admin Bearer検証・重複ヘッダー・スキーム不正・末尾空白・未登録401）単体テストを追加。`tests/integration/health_test.cpp`、`tests/security/security_test.cpp`、`tests/integration/legacy_compat_test.cpp` に H-01（時間差3チャンク逐次到着、上流POST 1回、429ヘッダー/本文透過、Client切断時早期cancel、入れ子`stream:true`無視・非bool 400）と H-02（全拒否/OPTIONS/413/404/400/SSE経路での `active_requests() == 0`、SSE実行中のgraceful shutdown待機と新規要求503、`ProcessController::restart_pipeline` の協調的cancel）を追加 |
| ビルド・CTest・DLL検査・`bin/` 同期 | Releaseビルド成功、`ctest --test-dir build -C Release --output-on-failure` **7/7成功（約4.05秒）**、`verify_imports` 両EXE成功。`build/Release` の `llm-gateway.exe` と `MaintenanceTool.exe` を `bin/` へ反映し、SHA-256一致および `bin/llm-gateway.json` 不変を確認 |

## 0B. Phase 2（P1: H-03 / H-05 / H-06 / H-07 / H-11 / H-12 / H-14 / H-13）実施結果（2026-10-13）

| 項目 | 実施内容・検証結果 |
|---|---|
| H-03（組み込みファイル欠落時の障害分離・クラウド継続） | `config.cpp` / `app.cpp` で既定の設定読込時はモデルパスの構文（非空・nullバイトなし・非UNC）のみを検査し（`validate_model_files = false`）、ファイル存在とGGUFロード検査を `EmbeddingEngine::init()` / `RerankEngine::init()` のランタイム初期化へ分離。欠落時はエラーログを出力して `LoadState::Error` へ遷移し、外部Provider経路（`/v1/chat/completions`、`/v1/models`、外部`/v1/embeddings`）は正常稼働、組み込み宛のみ503を返す。`ProcessController::restart_pipeline` で `embedded_engine.enabled == true` かつモデル未設定時の `embedded == "disabled"` を完了条件に追加。`MainFrame::refresh_status` で `embedded == "error"` を `[ Degraded ]`（Amber）/ `Status: Healthy (Embedded Error)` として区別表示 |
| H-05（起動失敗・Healthスキーマ・Latency・Logger・ModelCatalog診断） | `HealthClient::parse_health_payload` を新設し、`status == "ok"`・整数 `pid > 0`・非空 `version`・有効な `embedded` 状態のみを受理（`{}` 等を拒否）。`HttpServer` で `pre_routing_handler`〜`set_logger` 間の実測 `latency_ms` を記録。`Logger` に書込失敗カウンター `write_error_count()`・`last_write_error()` と初回stderr警告を追加。`ModelCatalog` でProvider探索失敗時に秘密を含まないWARNログを出力。`app.cpp` の設定読込失敗時にfallbackログとパス付きエラーを出力し、`ProcessController::start_gateway` で匿名パイプにより子プロセスstderrと終了コードを捕捉して起動直後・health待機中の異常終了原因をGUIへ返却 |
| H-06（保存前設定検証・原子的保存のエラー確認） | `ConfigStore::save_atomic` で一時ファイル作成前に `parse_and_validate_config(serialize_config(config), false)` を実行し、不正設定時は元ファイルを無変更のまま失敗を返却。`TempFileGuard`（RAII）によるハンドル閉鎖・一時ファイル削除と `FlushFileBuffers` の戻り値確認を追加。`ProcessController::restart_pipeline` の `Validating` 段階でも事前検証を実行 |
| H-07（停止処理のAdmin名整合・HTTP status・終了検証） | `ProcessController::stop_gateway` に `admin_credential_name` 引数と起動時設定の保持を追加し、非既定Admin資格情報名に対応。`POST /admin/shutdown` のHTTP status `200..299` を確認し、`TerminateProcess` / `WaitForSingleObject` の結果とポート解放を検証して停止失敗時は `false` と理由を返却。`MainFrame::on_stop` で失敗時にエラーダイアログを表示 |
| H-11（資格情報保存・クリップボード結果の厳密通知） | `CredentialEditor::generate_and_store_token` で `store_.set_credential` 失敗時はトークンをzeroizeして空文字を返却。`copy_to_clipboard_temporary` の戻り値を `bool` に変更し、Win32クリップボードAPIの全段階成功時のみ `true` を返して30秒消去スレッドを起動。`MainFrame` のトークン生成・削除ハンドラで保存失敗・コピー失敗・削除失敗を個別通知し、30秒自動消去の前提（GUIを開いたまま維持）を明示 |
| H-12（設定・HTTP APIの厳密型・範囲検証統一） | `config.cpp` に `require_string` / `require_bool` / `require_string_array` を追加し、現行・旧形式の全数値項目（`port`、`max_request_bytes`、`timeouts.*`、`n_threads`、`n_gpu_layers` 等）に `integer_in_range` を適用して小数切り詰めや `nlohmann::json::type_error` を400 `bad_request` に統一。`http_server.cpp` の `/v1/embeddings`（`encoding_format` 文字列・`dimensions` 正整数）と `/v1/rerank`（`top_n` 1〜文書数の整数・`return_documents` bool）も事前検証で400 JSONへ統一 |
| H-14（Unicode/日本語/空白/絵文字パス対応） | `config.hpp` / `config.cpp` に `MultiByteToWideChar(CP_UTF8)` / `WideCharToMultiByte(CP_UTF8)` ベースの `path_from_utf8` / `path_to_utf8` を実装し、設定・ログ・組み込みモデル・GUIプロセス起動のパス変換を統一。`src/gateway/main.cpp` で `GetCommandLineW()` + `CommandLineToArgvW()` からUTF-8へ変換し、`CP_ACP` 外の絵文字（`🧪`）や日本語・空白を含む `--config` パスでの起動・保存・再読込に対応 |
| H-13（Phase 2 回帰テスト・ビルド・`bin/` 同期） | `tests/unit/config_test.cpp` と `tests/integration/health_test.cpp` に H-03/H-05/H-06/H-07/H-11/H-12/H-14 の単体・統合テスト（`壊れた設定 🧪.json` を用いたサブプロセス起動失敗stderr捕捉試験を含む）を追加。Releaseビルド成功、`ctest --test-dir build -C Release --output-on-failure` **7/7成功（2.09秒）**、`verify_imports` 成功、`bin/` 両EXE更新・SHA-256一致・`bin/llm-gateway.json` 不変を確認 |

## 0C. Phase 3（P1: H-04 / H-08 / H-13）実施結果（2026-10-13）

| 項目 | 実施内容・検証結果 |
|---|---|
| H-04（GUI初期設定完結・CRUD・設定状態管理・未保存保護・別名保存・ポート跨ぎ再起動） | `ConfigStore` に `exists_on_disk`、`configs_equal`、`validate_config`、`upsert_provider`、`remove_provider`（Alias参照中は削除拒否）、`upsert_model_alias`、`remove_model_alias`、`make_openrouter_preset`、`make_lm_studio_preset`、`split_csv_tokens` / `join_csv_tokens` を追加し、`config.cpp` の `serialize_config` で `n_threads <= 0`（GUI新規作成時の既定値）を `default_threads` に正規化。`MainFrame` に `Open Config...`、Gateway設定（Listen Port / Client Auth / CORS / Embedded GGUFパス `Browse...` / `Apply Settings`）、Provider CRUDとプリセット（`+ OpenRouter Preset` / `+ LM Studio Preset`）、Alias CRUDを追加し、Bearerプロバイダー追加時にCredentialsタブへ即座にキー行を反映。`saved_config_` / `applied_config_` / `config_exists_on_disk_` で `New File (Not Saved)`・`Edited (Unsaved)`・`Saved on Disk (Restart Required)`・`Saved & Applied (Running)`・`Legacy Format` をヘッダーとタイトルバーで明示し、`Start`・`Open Config...`・ウィンドウ終了時の未保存保護、および旧形式保存時の `Save Copy As...`（原本保持）を実装。`ProcessController` に `active_port` 追跡と `WaitingPortRelease` での新旧ポート解放待機を追加 |
| H-08（組み込み推論Rerank厳密弱順序・KVキャッシュ毎回クリア・特殊トークン整形・境界検証） | `RerankEngine` に厳密弱順序（Strict Weak Ordering）の `compare_rerank_items` / `sort_rerank_items`（非NaN優先・`relevance_score` 厳密降順・同点時 `index` 昇順）を実装し、`1e-6f` 丸めによる非推移性サイクルを解消。`RerankEngine::format_rerank_tokens` / `format_rerank_token_ids` でモデル特殊トークンを用いた `[BOS] query [EOS] [SEP] doc [EOS]` 整形を実装。`EmbeddingEngine` と `RerankEngine` の両方で入力/文書組ごとの `llama_kv_cache_clear(ctx_)`、encoder-only（`llama_encode`）/ decoder-only（`llama_decode`）分岐、encoder-decoderモデルの `init()` 拒否、`cparams.n_ubatch = cparams.n_batch`、抽出失敗（`nullptr`）や非有限値（`NaN`/`Inf`）時の503 `embedded_load_failed` 返却、トークン上限超過（`n_tokens > max_tokens_`）および空文書文字列の400 `bad_request` 拒否を実装 |
| H-13（Phase 3 回帰テスト・ビルド・`bin/` 同期） | `tests/unit/config_test.cpp` に `RerankEngine` 厳密弱順序（反例 `(0,0),(0.75e-6,1),(1.5e-6,2)` 全6順列・同点・NaN）と `format_rerank_token_ids` / 実BGE語彙GGUF `format_rerank_tokens` 単体テストを追加。`tests/integration/health_test.cpp` に `/v1/embeddings`・`/v1/rerank` の `A -> B -> A` 連続一貫性、文書順序入替不変性、トークン上限超過400、空文書400、抽出失敗/NaN時503、6スレッド並列推論、および `ConfigStore` 初回未作成状態からのプリセット/CRUD・参照整合性保護・旧形式別名保存（原本不変）統合テストを追加。Releaseビルド成功、`ctest --test-dir build -C Release --output-on-failure` **7/7成功（2.77秒）**、`verify_imports` 成功、`bin/` 両EXE更新・SHA-256一致・`bin/llm-gateway.json` 不変を確認 |

## 0. 再レビュー結果の要約（2026-10-13・Phase 1着手時点の記録）

### 0.1 調査範囲・前提

プロジェクト全体の構成、既存の要件/実装/互換文書、CMake、設定例、GatewayとGUIの主要フロー、永続化・認証・通信・ログ、全7テストを再確認した。src/include/tests/cmake/configは計54ファイル。主要cppは処理を追跡し、ヘッダーは型・所有関係・公開APIを中心に確認した。全行の形式的検証ではない。

追跡したフローは、設定読込→GUI表示→保存→起動/再起動、資格情報登録→Bearer取得、API入力→Routing/Allowlist→上流通信→応答、モデル探索→cache、モデルロード→推論、HTTP拒否→要求カウンター→shutdown、ログ出力→GUI Tail。

第三者依存・build成果物は自作ソースとしてレビューしていない。httplibはGatewayが利用するpre/post-routingの呼出し順のみ確認した。resourcesは空。Gitがなく、履歴/blame・追跡対象の確認はできない。build/_depsのライセンス全文・依存CVEの網羅的監査は未実施。

UIは**静的確認**。今回GUIの実操作・画面レイアウト・キーボード操作は評価していない。HTTP経路は既存binと合成設定・ループバック模擬Providerで**動的確認**した。実OpenRouter、Tailscale、GGUF、私用設定への追加アクセス、資格情報書込は行っていない。

脅威モデルはローカル単独利用、127.0.0.1待受、上流だけLAN/インターネット接続。開発中・個人Portableを前提に、一般公開前の整備と今すぐの主要機能不良を区別する。

### 0.2 今回の実行結果

| 実行・確認 | 結果 |
|---|---|
| rgによる構成/危険操作/実装箇所検索、ファイル閲覧 | 完了。既存テストに資格情報の書込・削除がないことを確認して実行 |
| Test-Path .git | False |
| ctest --test-dir build -C Release --output-on-failure | **7成功、0失敗、スキップ0、0.52秒**。固定試験port 18765/18766が未使用であることを確認 |
| cmake -DTARGET_FILE=<binの各EXE絶対パス> -P cmake/VerifyImports.cmake | 両EXE成功 |
| binとbuild/Releaseの両EXEのSHA-256比較 | 両方一致 |
| 調査前後のファイル保全 | src/include/tests/cmake/config/binおよびCMakeLists.txtの計58ファイルでSHA-256一致。追加・削除なし |
| ローカルHTTP追加試験 | SSE反復、429→200、入れ子stream誤判定、型不正500、モデル欠落終了を再現 |
| パス・設定追加試験 | 小数portを切り詰めて起動すること、絵文字を含む実在設定パスで起動失敗することを再現 |
| ビルド/Lint/静的解析 | 今回は未実行。自作コード・依存設定を変更せず、既存Releaseを使用。新規クリーンビルド保証は追加していない |

既存テストと追加HTTP試験は、許可されたループバック通信可能な実行環境で実施した。追加試験はsynthetic credential service名、認証なし模擬Provider、短いtimeoutのみを使用した。試験で起動したGatewayをfinallyで終了し、既存Gateway・実サービスは操作していない。

生成/更新物はbuild/Testing/TemporaryのCTest記録、および以下の一時フォルダ内の合成JSON・ログ・results.json。EXEや実設定を再生成・コピーしていない。.gitignoreはなくGitもないため、これらがGit管理対象かどうかは判定不能。配布対象にはしない。

~~~text
C:/Users/kenta/AppData/Local/Temp/gateway-rereview-o2u2ys8k
C:/Users/kenta/AppData/Local/Temp/gateway-path-review-epk6vfin
~~~

文書編集用にTEMP内のgateway-review-section8.txt、gateway-review-section0.txtも作成した。恒久テストとしては登録していない。再現手順と期待値は第8章に記載した。

### 0.3 全体評価

旧設定の互換読込、通常の外部中継、Host/CORS、cache、静的DLL検査には実装と試験の裏付けがある。一方、完成したシステムとしてはSSE・処理寿命・モデル障害分離・初期設定GUIが未完成であり、一般公開の受入完了とは判断できない。

重大度は**High 5件、Medium 9件、Low 1件、Suggestion 1件**。Criticalと断定する証拠は得ていない。最優先は実通信の重複を起こすH-01と、競合・停止管理のH-02。7テスト成功でもこれらを検出していない。

| High指摘 | 優先度 | 主な影響 |
|---|---|---|
| H-01 SSEの反復とstatus喪失 | P0 | 重複送信、処理未完了、課金・負荷増加の可能性 |
| H-02 非同期寿命・要求カウンター | P0 | GUI破棄後アクセス、同時操作、処理中の早期終了 |
| H-03 組み込み欠落で全体停止 | P1 | クラウドを含め起動不能 |
| H-04 GUI初期設定と状態管理 | P1 | 初回設定の行き止まり、適用状態の誤認 |
| H-07 停止結果・Admin設定不整合 | P1 | 強制終了への不要な移行、停止失敗の誤通知 |

### 0.4 機能・観点別評価

| 領域 | 状態・判断 | 関連 |
|---|---|---|
| 設定互換・通常Routing | 実装済み。旧形式読込のためのAdapterは合理的。保存は2.0変換であり旧形式再保存ではない | H-06、H-12 |
| チャット通常応答 | 模擬中継は既存テスト成功。実Providerは未確認 | H-13 |
| SSE | 部分実装・不具合を再現 | H-01 |
| 組み込みEmbedding/Rerank | 実装あり、実GGUF品質は判断不能。ソート比較等に具体的不整合あり | H-03、H-08 |
| GUI設定CRUD・初回導線 | 未実装/部分実装。必須操作として不足 | H-04 |
| 認証・秘密 | WinCred分離、Host/Allowlist/TLSは妥当。操作結果と例外の扱いが不足 | H-11、H-12、SEC-L01 |
| 保存・復旧 | 原子的置換あり。全失敗の検出・検証・復旧導線は不足 | H-06 |
| 状態・非同期 | GUIと実行中設定の区別、busy、終了寿命が不整合 | H-02、H-07 |
| 診断・ログ | Tailあり。原因分類、起動前エラー、容量・表示上限が不足 | H-05、H-09 |
| 性能 | cacheは有効。逐次探索とUI同期I/O、無制限ログ表示が実用上の懸念 | H-05、H-09 |
| テスト | 基本ケースに意味はあるが、主要異常系の範囲不足 | H-13 |
| ビルド・運用 | 依存固定・DLL検査は良好。正式配布、README、ライセンス集約、クリーンOS試験は不足 | H-10 |
| ドキュメント整合 | 2.1文書は未達を区別している。今回新規再現は本書を最新根拠とし、修正時に仕様書も同期する | H-12、H-14 |

### 0.5 追加・修正・統合・削除・現状維持

- **追加**: 最小の初回設定GUI、設定/資格情報の失敗診断、保存失敗試験、SSE/寿命/Unicodeの回帰試験（H-04～H-06、H-11、H-13、H-14）。
- **修正**: H-01/H-02/H-03/H-07/H-08/H-12の実装不整合。固定ログ/Tail運用はH-09。
- **統合**: SSE/通常中継のcurl設定、共通数値検証、GUIのbusy/稼働状態の決定箇所。重複するエラー処理方式を増やさない。
- **削除候補**: 独立した不要ユーザー機能は確認できない。修正時に重複設定コードと実検証のない進捗だけの経路を置換し、配布物からbuild/ログ/私用文書を除外する。互換Adapterを「旧実装だから」という理由で削除しない。
- **現状維持**: GatewayとGUIの別プロセス、CPU専用/静的リンク、WinCred、loopback限定、Alias後Allowlist、依存SHA固定、旧形式のメモリ内変換、Release assert有効化。
- Undo/Redo、汎用Plugin、クラウド認証アカウント管理、大規模DIは現在の用途に必須とは判断しない。過剰設計を解消するための全面書換えも不要。

### 0.6 未確認事項・依頼者への確認事項

実クラウド/Tailscaleの疎通とモデルID、GGUF実推論、GUI視認性/キーボード、保存中断・権限・容量不足、shutdown中の実負荷、長時間メモリ、Windows 10/11クリーンVM、AVX2非対応PC、長いパス、依存の既知脆弱性・ライセンス適合は未確認。レビューのために実キー・共有先・モデルを変更/取得していない。

今後確認するとよい意図は、GUIの表示言語、Portable時のLOCALAPPDATA優先を維持するか、旧設定を今後も旧Gatewayと共有し続けるか、最初に保証するGGUFモデルの範囲。これらは今回レビューを完了するための承認待ちではない。

## 1. 最初に読むもの

1. [要件定義書 第2.1版](LLM_Gateway_Requirements_definition_document.md): 依頼者方針、到達目標、実装状態。
2. [実装仕様書 文書版2.1](LLM_GATEWAY_MAINTENANCE_TOOL_IMPLEMENTATION_SPEC_2.0.md): 現在のコードの挙動、設定、制約。
3. [設定互換性](CONFIG_COMPATIBILITY.md): 旧設定の使い方と非互換点。
4. 本書の試験記録・残課題を読んで、対象コードを確認する。

文書版2.1と設定schema_version「2.0」を混同しない。EXEはまだ2.0.0を表示する。実装仕様書のファイル名は既存参照維持のため変更していない。

## 2. 作業環境と依頼者の判断

作業フォルダ:

~~~text
C:/Users/kenta/Dropbox/2026年/ソフトウェア開発/2026-10-13_LLM-Gateway
~~~

Windows、PowerShell、MSVC/Visual Studio 2022、CMake。Gitリポジトリはない。Git履歴や差分を前提にしない。Git初期化、commit、pushを勝手に行わない。

依頼者の確定事項:

- 対象はWindows 10/11、ローカルLLMとクラウドLLMを使い分ける個人ユーザー。
- 初回設定をGUIのみで完結させることが望ましい。
- 組み込みモデルが利用不能でもクラウド経路を継続する。
- 当面は個人用Portable。安定後に一般公開を検討する。
- UIの一貫性・理解しやすさと、エラー原因究明を重視する。
- LM StudioはTailscale経由のLAN内PC。既存設定の既定モデル運用を尊重する。
- 実OpenRouter試験を行うならGemma4-31Bを推奨。正確なモデルIDは推測しない。

旧設定の読込互換対応は実装指示を受けて実施済み。今回の文書更新ではソース、設定、EXEに変更を加えていない。

## 3. 私用設定と操作制約

依頼者指定のテスト設定:

~~~text
C:/Users/kenta/Documents/Antigravity_Projects/VTA_v2/tools/llm-gateway/config/gateway.json
~~~

- 読み取り専用の比較・読込試験に使用した。SHA-256の前後一致を確認済み。
- このファイルをプロジェクト、fixture、bin、公開資料へコピーしない。
- APIキーや資格情報値を表示・記録しない。WinCredを試験の都合で書き換えない。
- 元のVTA側プロジェクトを修正しない。
- GUIでこの設定を開いても、Save Config / Save & Restartを実行すると形式変更の確認後に原本が2.0形式へ変換される。読込試験に保存操作を含めない。
- 保存試験は秘密・実接続先を含まない合成設定で行う。必要なら依頼者の許可範囲内で作業用コピーを使用する。
- 既存Gatewayが動いている場合、ポートや名前付きMutexの競合を避ける。別用途のGatewayを勝手に停止しない。
- 従来レビューで禁止された外部サービスへの書き込み、本番・共有資源への接続は包括的には許可されていない。実Provider接続の具体的な範囲を確認する。課金し得る推論試験は特に区別する。
- ビルド・テストの内容を読んでから実行する。グローバルへの恒久インストールは行わない。

## 4. 完了した互換対応

| 変更領域 | ファイル / 内容 |
|---|---|
| 設定モデル・変換 | include/gateway/config.hpp、src/gateway/config.cpp。旧形式判定、正規化、値検証、loaded_from_legacy |
| timeout | upstream_write_msを追加。旧秒単位からmsへ変換。ExternalProxyでupload進捗停止を監視 |
| URL | 旧APIルートへ/v1を補完。既存/v1は重複させない |
| Allowlist | 旧object形式を現行文字列へ変換。未対応ルールは拒否 |
| 組み込み無効 | 無効時のモデルファイル存在検査をスキップ |
| HTTP容量 | src/gateway/http_server.cpp。受信上限と413 JSON |
| CORS | request_guard.hpp/cpp。enabled、methods、headers、max-ageを反映 |
| 一覧探索 | model_catalog.hpp/cpp。cache有効/TTL、成功空一覧のcache、HTTP/JSON失敗の区別、全失敗時502 |
| ログ | logger.hpp/cpp。logging.fileによる固定ファイル追記 |
| GUIログ | log_tail_reader.hpp/cpp、main_frame.cpp。固定ログの参照 |
| GUI設定選択引数 | main.cpp、config_store.hpp/cpp、main_frame.hpp/cpp。--config対応 |
| Windows起動引数 | process_controller.cpp。wstringでパスと引用符を処理 |
| 保存確認 | MainFrame::confirm_config_format_change。旧設定から2.0への変換警告、No既定 |
| 試験 | tests/integration/legacy_compat_test.cpp、CMakeLists.txt。互換試験追加、既存6テストのRelease assert有効化 |
| 説明 | CONFIG_COMPATIBILITY.md |
| EXE | build/ReleaseとbinのGateway/GUIを更新・ハッシュ一致確認済み |

bin/llm-gateway.jsonは変更していない。旧実設定も変更・配布同梱していない。

## 5. 互換性で維持すべき仕様

- schema_versionがない場合だけ旧変換を適用する。現行形式へ旧項目を混ぜて受理しない。
- 旧listen.address、proxy、トップレベルcors、logging.file、小文字log level、Allowlist objectを扱う。
- 旧設定はembedded無効で開始する。
- モデル探索の無効化、TTL=0、成功した空一覧を区別する。
- 旧OpenRouterのAPIルートとLM Studioの/v1の両方を扱う。
- 読込でファイルを改変しない。保存は確認後の現行形式。loaded_from_legacyは保存しない。
- logging.fileはGateway cwd基準。GUI起動時はEXEディレクトリ。
- read timeoutは現在curl総期限であり、旧実装のidle timeoutと完全同一ではない。
- exact、単独*、末尾/*以外のglob/regexは未対応。
- CORSの許可メソッドを増やしても、そのHTTP APIが実装されるわけではない。
- 互換読込成功と、実クラウド通信・SSE・旧API全体の完全互換を混同しない。

## 6. 検証記録

以下は互換実装作業時の過去記録。今回の再レビューで再実行した結果・追加試験は第0章を参照する。今回も再ビルドと実Provider試験は行っていない。

| 確認 | 結果 |
|---|---|
| Releaseビルド | Gateway、MaintenanceTool、7テストが成功 |
| CTest | 7/7成功、失敗0、0.52秒 |
| 旧実設定の読込 | legacy_compat_testへ引数で指定し成功。Provider 3件 |
| 旧実設定の保全 | 試験前後SHA-256一致 |
| 合成設定によるHTTP | 1MiB超過POSTが413 JSON、PATCHプリフライト204、max-age=321、固定ログ作成 |
| ローカル模擬上流 | /api/v1/models、cache挙動、非streamの/chat/completions中継成功 |
| DLL検査 | binの両EXEにVerifyImports成功。許可Windows DLLのみ |
| bin更新 | build/Releaseの両EXEとハッシュ一致。bin設定不変 |
| 実OpenRouter / Tailscale | 未実施 |
| 実GGUF推論 / GUI全操作 / クリーンVM | 未確認 |

サンドボックス内ではhealth_test、security_test、legacy_compat_testがループバック通信に失敗した。許可された制限外実行では全7件成功した。制限内の失敗を無条件に製品バグと断定せず、逆にコード変更で試験を無効化して解消しない。

## 7. 再現コマンド

作業ディレクトリをプロジェクトルートにする。

~~~powershell
# 既存buildを使用
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target verify_imports

# 旧実設定の読込だけを確認。実Providerへは接続しない
$privateConfig = 'C:/Users/kenta/Documents/Antigravity_Projects/VTA_v2/tools/llm-gateway/config/gateway.json'
$beforeHash = (Get-FileHash -LiteralPath $privateConfig -Algorithm SHA256).Hash
& ./build/Release/legacy_compat_test.exe $privateConfig
$testExit = $LASTEXITCODE
$unchanged = $beforeHash -eq (Get-FileHash -LiteralPath $privateConfig -Algorithm SHA256).Hash
"TestExit=$testExit ConfigUnchanged=$unchanged"
~~~

初回configureは実装仕様書を参照。FetchContentで外部取得が発生する。通常は既存build/_depsを利用する。

以前MSBuildのtlogにロック/マップ済みファイルエラーが発生した。原因は確定していない。重複ビルドを止めた後、次のオプションで完了した。

~~~powershell
cmake --build build --config Release --target llm-gateway MaintenanceTool config_test router_test auth_test request_guard_test health_test security_test legacy_compat_test --parallel 2 -- /nr:false /p:TrackFileAccess=false
~~~

このオプションでは依存ソースの再コンパイルが増える場合がある。通常ビルドを先に試し、恒久的な環境設定や第三者コードの修正にしない。/DNDEBUGより/UNDEBUGが優先されるD9025警告は既存assertテストをReleaseで有効にする意図による。既存の未使用変数警告も残る。

追加HTTP手動試験はPython 3.10.6で実施した。通常のpython shimには既定バージョンがなく、明示パスを使用した。

~~~text
C:/Users/kenta/.pyenv/pyenv-win/versions/3.10.6/python.exe
~~~

その手動試験の一時出力は次のディレクトリ。環境依存の参考資料であり、再現可能な正式fixtureではない。

~~~text
C:/Users/kenta/AppData/Local/Temp/gateway-compat-runtime-60v58jy_
~~~

build/compatibility-build-final.logは最終ビルド記録。build、binの成果物および作業ログが残る。Gitがないため管理対象/非対象をGitで判定できない。公開用packageには含めない。

## 8. 残課題と次の受入条件（2026-10-13 再レビュー）

H-01～H-10を維持し、H-11～H-14を追加した。以下が現時点の評価であり、前回の推定を今回確認できた範囲で更新した。重要度と着手順は別の軸。High 5件、Medium 9件、Criticalの断定0件。P0は安定利用前の必須対応である。

### H-01 / P0（Phase 1で修正・回帰試験済み）: SSEが上流POSTを再実行し、エラーステータスを失う

- **状態**: **修正済み（2026-10-13 Phase 1）**。`UpstreamCurlTransfer`（`curl_multi`）による単一転送・ヘッダー先読み確定・`set_chunked_content_provider` + `sink.done()`・切断時cancel・トップレベルbool `stream` 判定を実装し、`health_test` / `legacy_compat_test` で回帰試験済み（実Provider接続のみ未実施）。以下は修正前の調査記録。
- **重要度 / 分類**: High / 機能・通信信頼性。
- **対象**: src/gateway/external_proxy.cpp:168、:175、:235、:244。
- **現状・根拠**: streamをJSON構造ではなく文字列で判定し、content provider呼出しごとにcurlを実行。正常終了のsink.done()と上流status確定処理がない。
- **動的確認**: 合成上流で1要求に8回のPOST、上流429で7回のPOSTとClient側200を観測。350msの観測窓内に応答は終了しなかった。トップレベルstream=falseでもmetadata.stream=trueでSSE分岐へ入り、7回送信された。回数は固定仕様ではなく今回の観測値。
- **問題・影響**: 応答重複、処理が終わらない、API課金・負荷の増加、エラー原因の誤認。
- **改善案**: JSONのトップレベルboolで判定し、上流転送を1回だけ所有する。ヘッダー確定前後の失敗と送信完了・切断を分ける。通常/SSEの共通curl設定は統合する。
- **受入**: 時間差3チャンクの逐次到着、上流1回、正常完了、429伝達、Client切断時cancel、入れ子streamの無視。
- **確信度**: High（静的・動的）。**コスト / 変更リスク / 依存**: 中～大 / 高 / H-02と同時設計。

### H-02 / P0（Phase 1で修正・回帰試験済み）: GUI再起動の競合とHTTP要求寿命の管理不整合

- **状態**: **修正済み（2026-10-13 Phase 1）**。`ShutdownCoordinator::RequestGuard` と `svr_.set_logger` による要求カウンター管理、所有 `shutdown_thread_`、`ProcessController` の `mutex_` + `request_cancel()`、`MainFrame` の `is_busy_`・`ui_alive_`・所有 `restart_worker_`・設定snapshotを実装し、`health_test` / `security_test` で回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: High / 設計・信頼性。
- **対象**: src/maintenance/main_frame.cpp:51、:213、:295、:339、:364、include/gateway/shutdown_coordinator.hpp:24、:32、:46、src/gateway/http_server.cpp:61、:92。
- **現状・根拠**: 再起動ワーカーがthisとcurrent_config_を参照したままdetachし、破棄時はタイマー停止だけ。500msのrefresh_statusがbusy状態と無関係にStart/Stopを再有効化する。組み込みチェックボックスもワーカー中に変更できる。
- **追加確認**: pre-routingでleaveした拒否/OPTIONSにもpost-routingのleaveが走る。利用中httplibの呼出し順を確認し、二重減算となる構造を確認した。post-routingは応答本文送信前なので、SSEの実転送も終了扱いになる。第三者コード自体への品質指摘ではなく、GatewayのAPI利用の誤り。
- **問題・影響**: 破棄後アクセス、設定・プロセスハンドルの同時操作、負の要求カウンター、処理中の早期終了。
- **改善案**: 所有するワーカーのcancel/joinとUI破棄後イベント抑止、設定snapshot、永続busy状態を導入。受付から送信終了までを一度だけ解放する要求ガードへ置換する。
- **受入**: 再起動中のGUI終了・タイマー・連打で競合しない。通常/拒否/OPTIONS/例外/SSEすべてで要求数が正しく戻る。停止期限を守る。
- **確信度**: High（コード上の寿命・状態更新と呼出し順）。GUIクラッシュとshutdownの実再現は未実施。
- **コスト / 変更リスク / 依存**: 中～大 / 高 / H-01。WinCredや実Gatewayを使う破壊的終了試験は行わない。

#### H-03 / P1（Phase 2で修正・回帰試験済み）: 組み込みファイル欠落がクラウド経路まで停止させる

- **状態**: **修正済み（2026-10-13 Phase 2）**。`parse_and_validate_config` / `load_config_from_file` の既定を `validate_model_files = false` とし、設定読込時はパス構文のみ検証。ファイル存在とGGUFロードは `EmbeddingEngine::init()` / `RerankEngine::init()` のランタイム初期化で検査し、欠落時は `LoadState::Error` へ遷移しつつ外部Provider経路（チャット・一覧・外部Embedding）を継続稼働させる構成とした。`ProcessController::restart_pipeline` の完了条件に `embedded == "disabled"` を追加し、`MainFrame::refresh_status` で `embedded == "error"` を `[ Degraded ]`（Amber）表示に分離。`config_test` / `health_test` で回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: High / 機能・障害分離。
- **対象**: src/gateway/config.cpp:410、src/gateway/app.cpp:112、src/maintenance/process_controller.cpp:268、src/maintenance/main_frame.cpp:213。
- **現状・根拠**: 無効時は存在検査をスキップするが、有効時の欠落ファイルは設定読込で例外。GUIはembedded=errorでもHealthy、enabled=trueかつdisabledでは再起動完了条件を満たさない。
- **動的確認**: 欠落GGUFを指定した合成設定で終了コード1、existing regular fileエラー。外部Providerを設定していてもHTTP開始前に終了した。
- **問題・影響**: 「組み込みが使えなくてもクラウド継続」という依頼者の要件に反する。
- **改善案**: 設定構文とモデル利用可能性を分離し、モデル固有障害を個別stateと診断へ移す。Gateway稼働状態とembedded状態を分ける。
- **受入**: 欠落/ロード失敗/モデル未指定で外部模擬POSTが成功し、該当embeddedだけ503。GUIで継続利用可否が分かる。
- **確信度**: High（欠落起動失敗は動的、GUI状態は静的）。
- **コスト / 変更リスク / 依存**: 中 / 中 / H-05。P0の通信・寿命問題を先行するためP1。

### H-04 / P1（Phase 3で修正・回帰試験済み）: GUIだけで初回設定が完結せず、編集状態と稼働状態が混在

- **状態**: **修正済み（2026-10-13 Phase 3）**。`ConfigStore` に初回起動用の `exists_on_disk`、等値比較 `configs_equal`、純粋設定編集ヘルパー（`upsert_provider` / `remove_provider` / `upsert_model_alias` / `remove_model_alias`）、OpenRouter / LM Studio（Tailscale）プリセット生成、CSV変換、事前検証 `validate_config` を追加。`MainFrame` に Listen Port / Client Auth / CORS・Embedded GGUF（Browse・Clear）・Provider CRUD・Model Alias CRUD・`Open Config...`・`Save Copy As...`（旧形式v1.0読込時に原本を保持して別名保存を選択可能）・未保存/未適用状態バナーと確認ダイアログ（`Start` / `Open Config...` / ウィンドウ終了時）を実装し、`ProcessController` の稼働中ポート追跡（`set_active_port` / `active_port`）とあわせて `health_test` で回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: High / UI・機能。
- **対象**: src/maintenance/main_frame.cpp:116、:123、:301、src/maintenance/config_store.cpp:95、:117、src/maintenance/main.cpp:10。
- **現状・根拠**: Provider/Aliasは一覧のみ。接続先、モデルパス、port/CORS、ファイル選択の編集UIがない。組み込みチェックだけメモリを書き換え、Startは既存ファイルから起動する。未保存表示・終了確認もない。
- **問題・影響**: 初心者はJSON/CLIが必要で、画面で変えた状態が起動に反映されないと誤認する。新規設定の空Provider状態から目的の利用へ進めない。
- **改善案**: 最小のProvider/接続先/キー登録→検証→保存→起動フローを先に作る。編集・保存・適用済みを区別する。旧形式は保存時にコピー選択できる導線を検討する。
- **受入**: JSON編集なしで合成Providerの初回設定を完了し、未保存状態と旧形式変換が理解できる。
- **確信度**: High（操作の有無と値の流れ）。レイアウト・視認性・キーボード操作感は静的確認のみ。
- **コスト / 変更リスク / 依存**: 大 / 中 / H-02、H-03、H-06、H-07、H-11。

### H-05 / P1（Phase 2で修正・回帰試験済み）: 起動失敗・Provider失敗の原因がGUIから追いにくい

- **状態**: **修正済み（2026-10-13 Phase 2）**。`HealthClient::parse_health_payload` による厳密スキーマ検証（`{}` 等を拒否）、`HttpServer` での実測 `latency_ms` 記録、`Logger` の書込失敗カウンター（`write_error_count()` / `last_write_error()`）と初回stderr警告、`ModelCatalog` のProvider別探索失敗WARNログ、`app.cpp` 設定読込失敗時のfallbackログ記録、`ProcessController::start_gateway` の匿名パイプstderr・終了コード捕捉を実装し、`health_test` で回帰試験済み（UIの500msポーリング非同期化はH-09とあわせて検討対象）。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / 診断・UI。
- **対象**: src/gateway/app.cpp:114、src/gateway/logger.cpp:145、src/gateway/model_catalog.cpp:74、:190、src/maintenance/main_frame.cpp:214、:312、src/maintenance/health_client.cpp:50。
- **現状・根拠**: 設定エラーはLogger初期化前のstderr。非表示起動するGUIは捕捉しない。healthの通信/JSON失敗をnulloptにまとめ、通常HTTPログにlatencyがない。ログ書込失敗も通知しない。healthは{}のような必須項目不足のJSONでもHealthResponseを返す。
- **問題・影響**: 設定、資格情報、ネットワーク、別サービス、モデル問題が同じ停止/初期化表示へ潰れ、対処を選べない。UIスレッドのhealth/ログI/Oや停止待ちで応答も遅れる。
- **改善案**: 秘密を含まない起動結果、終了コード、使用パス、Provider別エラー分類を返す。healthスキーマ/PIDを検証し、latencyを計測。長い処理はH-02の安全な非同期方式へ。
- **受入**: 設定破損・未登録キー・到達不能・401/429・ログ権限不足を区別。{}をHealthyとしない。
- **確信度**: High（静的）。権限・ディスク枯渇の失敗注入は未実施。
- **コスト / 変更リスク / 依存**: 中 / 中 / H-02、H-12。

### H-06 / P1（Phase 2で修正・回帰試験済み）: 保存前検証と永続化失敗の確認が不十分

- **状態**: **修正済み（2026-10-13 Phase 2）**。`ConfigStore::save_atomic` および `ProcessController::restart_pipeline`（`Validating` 段階）で `parse_and_validate_config(serialize_config(config), false)` による事前検証を必須化し、不正時は一時ファイル作成・既存プロセス停止前に中断。`TempFileGuard`（RAII）と `FlushFileBuffers` の戻り値確認を追加し、`health_test` で不正設定保存時の原本保全を回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / データ整合性。
- **対象**: src/maintenance/config_store.cpp:125、:156、:175、src/maintenance/process_controller.cpp:223。
- **現状・根拠**: FlushFileBuffersの戻り値を無視し置換へ進む。Validating段階は進捗表示だけ。serialize/dump例外時のhandle/tmpのRAII保護もない。
- **問題・影響**: 入力不正を保存・再起動前に止められず、Flush失敗を成功と表示する可能性がある。一方、通常のWrite/Flush/Move失敗は処理されており、常に破損するとは言えない。
- **改善案**: 共通設定検証、Win32結果確認、handle/tmpのRAII、原本・復旧手段の保護。旧形式変換確認を維持する。
- **受入**: 不正値・Write/Flush/Move失敗・途中終了を合成ファイルで検証し、成功誤通知と原本破壊がない。
- **確信度**: High（未検査箇所）。電源断・容量不足の再現は未確認。
- **コスト / 変更リスク / 依存**: 中 / 中 / H-12。H-04の前提。

### H-07 / P1（Phase 2で修正・回帰試験済み）: 停止処理がAdmin名と実行結果を正しく扱わない

- **状態**: **修正済み（2026-10-13 Phase 2）**。`ProcessController::stop_gateway` に `admin_credential_name` 引数および起動時設定の保持を追加し、`POST /admin/shutdown` のHTTP 2xx応答確認、`TerminateProcess` / `WaitForSingleObject` 結果確認、ハンドル未保持時のポート解放確認を実装。失敗時は `false` と理由を返し、`MainFrame::on_stop` でエラーダイアログを表示。`health_test` でカスタムAdmin名での停止および誤Admin名での拒否を回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: High / ライフサイクル。
- **対象**: src/maintenance/process_controller.cpp:125、:150、:165、:176、:236、src/maintenance/main_frame.cpp:315。
- **現状・根拠**: Admin名を固定参照し、curl成功だけでHTTP送信成功とする。TerminateProcessの結果を確認せず、停止不能でもtrueを返す経路がある。GUI on_stopはerrorを表示しない。新設定portを停止先にも使う。
- **問題・影響**: 非既定Admin名で正常停止できず強制終了へ進む。停止失敗を成功扱いし、利用者や後続再起動が誤った状態で動く。
- **改善案**: 稼働中設定と次回設定を分離し、設定したAdmin名・HTTP status・実際のプロセス終了を確認する。結果をGUIへ返す。
- **受入**: 非既定Admin名、HTTP拒否、終了失敗、別EXE、port変更を合成環境で確認。無関係なプロセスを終了しない。
- **確信度**: High（静的）。実資格情報の変更・既存プロセスの停止試験は未実施。
- **コスト / 変更リスク / 依存**: 中 / 高 / H-02、H-06。

### H-08 / P1（Phase 3で修正・回帰試験済み）: 組み込み推論のスコア・状態・境界値の保証が不足

- **状態**: **修正済み（2026-10-13 Phase 3）**。`RerankEngine::compare_rerank_items` でイプシロン比較を廃止し厳密弱順序（`score` 降順 → `index` 昇順、非有限値は末尾）へ変更。`EmbeddingEngine` と `RerankEngine` の両方で `cparams.n_ubatch = cparams.n_batch`、毎入力の `llama_kv_cache_clear(ctx_)`、`llama_model_has_encoder` による `llama_encode` / `llama_decode` 自動切替、トークン長上限検査（超過時 `400 bad_request`）、空文書検査（`400 bad_request`）、`llama_get_embeddings*` のnull/非有限値検査（異常時 `503 service_unavailable`）を実装。さらに `format_rerank_tokens` / `format_rerank_token_ids` でモデルの語彙（SEP / EOS）に応じた `[BOS] query [SEP] doc [SEP]` 等のトークン構築を実装し、`config_test`（6順列・`ggml-vocab-bert-bge.gguf` 実トークナイズ）および `health_test`（`A->B->A` 一貫性・文書順不変性・400/503境界・6スレッド並行要求）で回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / 推論品質・コード。
- **対象**: src/gateway/embedding_engine.cpp:96、:119、:129、src/gateway/rerank_engine.cpp:102、:136、:146、:165。
- **現状・根拠**: コンテキストを再利用し毎入力のpositionを0に戻すが、入力単位のKV初期化やtoken長上限の検査がない。Rerankはquery+改行+docを固定使用し、embedding取得失敗でもscore=0で正常結果に進む。
- **確定した追加問題**: Rerankの「差が1e-6以内ならindex順」という比較は推移性を満たさない。例として(score,index)=(0,0),(0.75e-6,1),(1.5e-6,2)ではA<B、B<C、C<Aとなる。
- **問題・影響**: 結果順序や失敗の扱いが不正になり得る。モデル別入力形式・連続推論の実際の影響は実GGUFで未検証であり、すべてのモデルで誤結果と断定しない。
- **改善案**: 全順序になる比較、ベクトル取得失敗の明示、長文制限、対象モデルの入力形式とコンテキスト初期化を実証する。
- **受入**: 順序の反例テスト、A→B→Aの連続結果、文書順序交換、長文/空/並行入力、対応モデルの品質確認。
- **確信度**: High（比較・失敗扱い）、Medium（実モデルへの影響）。
- **コスト / 変更リスク / 依存**: 中～大 / 中 / H-03、H-13。前回P2から、具体的不整合が判明したためP1へ。

### H-09 / P2（Phase 4で修正・回帰試験済み）: ログ表示が無制限増加し、Tailの縮小・分割に弱い

- **状態**: **修正済み（2026-10-13 Phase 4）**。`LogTailReader` が縮小・置換検知（offsetを0へ戻す）、未完行（`pending_`）の次回持越し、`\n` 区切りによるUTF-8分割回避、初回tailの先頭部分行破棄、1回あたり1MiBの増分上限を実装。`MainFrame::append_log_lines` で表示を最大5000行に制限。`health_test` で回帰試験済み。固定ログの容量管理・モデル探索の並列化は測定前のため未着手。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / 性能・運用。
- **対象**: src/maintenance/main_frame.cpp:286、src/maintenance/log_tail_reader.cpp:118、:127、:140、src/gateway/logger.cpp:107、src/gateway/model_catalog.cpp:123。
- **現状・根拠**: AppendTextし続けるがUI行数上限がない。Tailは差分全量を確保し、縮小時にoffsetを戻さず、未完行やUTF-8分割を保持しない。固定ログは無制限追記。モデル探索は逐次。
- **問題・影響**: 長時間利用でメモリ・表示負荷が増え、ログ欠落/文字化けや到達不能Providerによる長い待機が起こり得る。
- **改善案**: 表示リングバッファ、増分量制限、縮小検知、未完bytes/行保持。固定ログの互換性を保った上で容量管理を検討。探索は測定後に並列化や失敗表示を選ぶ。
- **受入**: 大量追記、切り詰め、日本語の途中分割、日付切替で表示・メモリが安定する。
- **確信度**: High（静的）。長時間メモリ計測は未実施。
- **コスト / 変更リスク / 依存**: 中 / 中 / H-05。

### H-10 / P2（Phase 5で修正済み・一般公開前の確認事項あり）: 個人用Portableから一般配布へ進むための構成が未整備

- **状態**: **実装済み（2026-10-13 Phase 5）**。`README.md`（GUI初回手順・診断・資格情報の移行・組み込みモデル）、`NOTICE.md`（第三者表示）、`.gitignore`、GGUF不要・秘密なしの `config/llm-gateway.external-only.example.json`、許可リスト方式の `scripts/package.ps1`（両EXE・README・NOTICE・CONFIG_COMPATIBILITY・設定例・依存ライセンス全文のみをZIP化し、`.log`/`.gguf`/`llm-gateway.json` 混入を拒否、SHA256SUMS出力）を追加。外部専用設定例でのGateway起動と `/health` 200を確認。**追補（2026-10-13）**: 所有者決定によりMIT License（Copyright (c) 2026 HAIA Project）の `LICENSE` を追加し、`NOTICE.md` と配布スクリプトにwxWidgets内蔵のlibpng・zlib・PCRE（実リンクを `CMakeCache.txt` で確認）のライセンス全文を追加。**未実施（一般公開前に必須）**: 法的適合の最終確認、クリーンなWindows 10/11環境での起動試験、CI。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / 配布・ドキュメント。
- **対象**: CMakeLists.txt:197、cmake/Dependencies.cmake、cmake/VerifyImports.cmake、config/llm-gateway.example.json:36、bin。
- **現状・根拠**: 依存版・SHA固定とDLL検査はあるが、README・プロジェクトのLICENSE/NOTICE・package/CI・.gitignoreはない。binは両EXEと設定のみ。設定例は未同梱GGUFをenabled=trueで要求する。buildは今回計測で約1296.1MiB、16439ファイル。
- **問題・影響**: フォルダ全体の配布で不要物・個人情報が混入しやすい。初回起動手順と資格情報の移行条件が分かりにくい。依存ライセンス適合は未確認。
- **改善案**: 配布許可リスト、秘密なし外部専用設定例、操作/診断の入口README、ライセンス収集、ハッシュ、クリーンVM試験を整える。Git導入は別途依頼者判断。
- **受入**: 実設定/ログ/build/本書を含まないpackage、Windows 10/11起動、WinCred再登録説明、第三者表示確認。
- **確信度**: High（ファイル構成とDLL検査）。法的適合・クリーンVMは未確認。
- **コスト / 変更リスク / 依存**: 中 / 低～中 / H-01～H-07の安定化。個人用開発中のためP2、一般公開前は必須。

### H-11 / P1（Phase 2で修正・回帰試験済み）: 資格情報の保存・コピー失敗を成功と通知する

- **状態**: **修正済み（2026-10-13 Phase 2）**。`CredentialEditor::generate_and_store_token` でWinCred保存成否を検査し（失敗時はzeroizeして空文字返却）、`copy_to_clipboard_temporary` の戻り値を `bool` にしてWin32クリップボード操作成否を返却。`MainFrame` で生成・保存・コピー・削除の各結果を個別にダイアログ表示し、30秒自動消去にはGUIを開いたままにする必要がある旨を明記。`health_test` で検証済み。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / UI・認証運用。
- **対象**: src/maintenance/credential_editor.cpp:77、:83、:93、src/maintenance/main_frame.cpp:375、:383、:432。
- **現状・根拠**: generate_and_store_tokenがset_credentialのboolを無視。copy_to_clipboard_temporaryは失敗を呼出元へ返さない。GUIは無条件に「generated and copied」と表示。削除結果も無視する。
- **問題・影響**: 保存されていないトークンを利用して401となり、利用者が原因を追えない。資格情報の操作結果と画面の説明が一致しない。
- **改善案**: 生成・登録・コピーを個別結果として返し、登録失敗ならコピー/成功通知をしない。クリップボード取得/設定/解放の失敗を確認する。自動消去はGUI終了や競合で保証できない点も正確に表示する。
- **受入**: WinCred失敗、クリップボード使用中、削除失敗、GUI終了の模擬試験で誤成功通知がない。実資格情報を試験で上書きしない。
- **確信度**: High（戻り値とUI分岐）。実WinCred失敗注入は未実施。
- **コスト / 変更リスク / 依存**: 小～中 / 中 / H-04、H-05と通知方式を統一。

### H-12 / P1（Phase 2で修正・回帰試験済み）: APIと設定の型検証が不統一

- **状態**: **修正済み（2026-10-13 Phase 2）**。`config.cpp` に `require_string` / `require_bool` / `require_string_array` と全数値項目への `integer_in_range` を適用し、小数portや型不一致をすべて `GatewayException(400, "bad_request", ...)` へ統一。`http_server.cpp` の `/v1/embeddings`（`encoding_format`・`dimensions`）と `/v1/rerank`（`top_n`・`return_documents`）の型・範囲検証も400 JSONへ統一し、`config_test` / `health_test` で回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / 入力検証・診断。
- **対象**: src/gateway/http_server.cpp:253、:301、:398、src/gateway/config.cpp:255、:269、:284。
- **現状・根拠**: encoding_formatを型検査せずget<string>()する。dimensions/top_nはis_number()で小数も通り、不正型は無視される経路がある。既存設定の数値もget<int>()で切り詰める。
- **動的確認**: encoding_format=123のPOSTは400 JSONでなく本文0 bytesの500。listen.port=N+0.5の設定で整数Nの/healthが200になった。
- **問題・影響**: 入力の間違いが内部故障に見え、指定と異なるport・値で実行される。新しい互換アダプターの厳格整数検証と一貫しない。
- **改善案**: 設定/API共通の型・範囲・必須検査を明示し、ユーザー入力由来は400 JSONに変換する。未知の内部例外も秘密なしの500 JSONへ統一。
- **受入**: null、bool、文字列、小数、巨大unsigned、空配列で規定エラーとなり、暗黙切り詰めと空500がない。
- **確信度**: High（静的・動的）。
- **コスト / 変更リスク / 依存**: 中 / 中 / H-06、H-13。旧形式で受理していた正常値は維持する。

### H-13 / P1（Phase 1・Phase 2・Phase 3分を実装済み、以後も各Phaseで継続追加）: テスト成功が主要な異常系・利用フローを保証しない

- **状態**: **Phase 1（H-01/H-02 + Authenticator単体）、Phase 2（H-03/H-05/H-06/H-07/H-11/H-12/H-14）、および Phase 3（H-04/H-08）の回帰試験を実装済み（2026-10-13）**。Phase 4（H-09/SEC-L01）も実装済み。Phase 5以降も各変更と同時に追加する。以下は初期調査記録。
- **重要度 / 分類**: Medium / テスト。
- **対象**: tests/unit/auth_test.cpp:7、tests/integration/health_test.cpp:16、tests/security/security_test.cpp:18、tests/integration/legacy_compat_test.cpp:99、CMakeLists.txt:188。
- **現状・根拠**: 7件は再実行成功。ただしauth_testはAuthenticatorのBearer/401処理を試さず、security_testはHost/Origin拒否だけ。SSE、shutdownカウンター、GUI、WinCred失敗、保存失敗、実推論の試験がない。固定portと200ms sleepを使う試験もある。
- **問題・影響**: 今回再現した主要不具合が全テスト成功と共存する。遅いPCやport競合で不安定になり得る。
- **改善案**: H-01～H-12/H-14ごとに意味のある回帰を追加。port自動割当、ready待ち、終了保証、試験期限を揃える。実Providerキーを必須にしない。
- **受入**: 今回のSSE/500/小数port/Unicodeを修正前に検出し、修正後に成功。成功件数と未検証項目を別記する。
- **確信度**: High（全7テストの内容・実行結果）。
- **コスト / 変更リスク / 依存**: 中 / 低 / 各修正と同時。専用の巨大テスト基盤は不要。

### H-14 / P1（Phase 2で修正・回帰試験済み）: Unicode設定パスが起動側と読込側で一致しない

- **状態**: **修正済み（2026-10-13 Phase 2）**。`src/gateway/main.cpp` で `GetCommandLineW()` + `CommandLineToArgvW()` からUTF-8へ変換し、`config.hpp` / `config.cpp` の `path_from_utf8` / `path_to_utf8`（`CP_UTF8` 明示変換）を全ファイル・ディレクトリ・モデル・ログパス処理に適用。日本語・空白・絵文字（`設定 🧪`）を含むパスでの保存・読込・CLIサブプロセス起動を `config_test` / `health_test` で回帰試験済み。以下は修正前の調査記録。
- **重要度 / 分類**: Medium / Windows互換性・Portable。
- **対象**: src/gateway/main.cpp:3、src/gateway/app.cpp:71、src/gateway/config.cpp:123、src/maintenance/process_controller.cpp:85、src/maintenance/config_store.cpp:134。
- **現状・根拠**: GUIはCreateProcessWとwstringを使う一方、Gatewayはchar* argv→std::string→filesystem::path。ほかにもファイル名のstring変換が残る。
- **動的確認**: 同内容の合成設定をASCIIパスに置くとhealth成功。「設定 🧪」を含むパスに置くと存在するのにconfig missing、終了コード1。この環境での結果であり、日本語全般が失敗するとは言わない。
- **問題・影響**: Portableの配置・設定選択先によって起動不能。GUIからのwide引数化だけでは解決していない。
- **改善案**: Gateway入口からwideパスを維持するかUTF-8へ明示変換し、filesystem、GUI表示、モデルpath、保存一時名まで規則を統一する。
- **受入**: 日本語、空白、絵文字の設定/ログパスで起動・保存・再読込。長いパスはOS設定を明記して別途検証する。
- **確信度**: High（CLI経路は動的）。GUI実操作・全ファイルI/O経路は未確認。
- **コスト / 変更リスク / 依存**: 中 / 中 / H-06。Windows向け主要用途のためP1。

### 軽微な改善候補（重大件数に含めない）

| ID / 重要度 / 優先度 | 対象 | 根拠・改善案 | 確信度 |
|---|---|---|---|
| SEC-L01 / Low / P2（Phase 4で修正済み: 長さ差をsize_tで保持、256/512境界をauth_testで検証） | include/gateway/secret.hpp:68 | 長さ差をunsigned charへ縮めるため、差256 bytesかつ追加部分がNULなら比較がtrueになり得る。通常のBearer文字列ではその入力条件に達しにくく、認証迂回は未確認。サイズ差はsize_t等で保持し境界試験を追加する | High（関数の論理）、悪用可能性は未確認 |
| ARCH-L01 / Suggestion / P3 | CMakeLists.txt:45、:106、src/gateway/external_proxy.cpp:173 | GUIが推論込みgateway_core全体に依存し、SSE/通常中継でcurl設定が重複。設定・資格情報・HTTP転送の共通部分を変更時に小さく分離する。現規模で全面的なPlugin/DI化は不要 | High（構造） |

## 9. 推奨作業順

1. **P0 / Phase 1 — H-01/H-02/H-13（完了・2026-10-13）**: 再現済みSSEと処理寿命の回帰を固定済み。H-13は以後も各変更と同時に進める。
2. **P1 / Phase 2 — H-03/H-05/H-06/H-07/H-11/H-12/H-14（完了・2026-10-13）**: 外部経路の継続、診断、入力/パス、資格情報、保存・再起動の信頼性を整備・回帰試験済み。
3. **P1 / Phase 3 — H-04/H-08（完了・2026-10-13）**: 安全な共通処理を使ってGUI初期設定を完成し、組み込み推論の厳密弱順序比較・KV初期化・特殊トークン構築・境界値検査を実装・回帰試験済み。
4. **P2 / Phase 4 — H-09/SEC-L01（完了・2026-10-13）**: 長時間ログ運用・境界値を検証し、測定に基づき改善する。
5. **P2 / Phase 5 — H-10（実装済み・2026-10-13。LICENSE決定・クリーンVM試験は未実施）**: 個人Portableの配布構成を整え、一般公開前にクリーンVM・ライセンスを確認する。
6. **P3 — ARCH-L01（次の着手対象）**: 関連箇所を変更する際に小さく分離・統合する。単独の全面リファクタリングを優先しない。

UI統一は各段階で行い、内部用語や複雑なモードを増やしすぎない。既存の小規模構成に、将来用途だけのPlugin層やDI基盤を追加しない。

## 10. 引き継ぎ時の完了判定

次のエージェントは作業範囲を確認してから、以下を記録する。

- 修正したファイル、動作差、対応したH-ID。AGENTS.mdに従いCHANGELOG.mdを毎回更新する。
- 既存の7テストと変更に必要な回帰試験の結果。未実行と失敗を隠さない。
- ビルド/試験の生成物と、bin EXEを更新したか。更新時は両EXEと設定保全を確認する。
- 実設定の読込試験をした場合、前後ハッシュ一致。実値をログや文書へ貼らない。
- 実サービスへ接続した場合、接続先・目的・費用影響を秘密なしで記録する。
- 要件・実装仕様・本書の実装状態を更新する。7テスト成功だけで全機能完成と判定しない。

### 次の依頼に添付できる文例

> WORK_HANDOVER.md、要件定義書、実装仕様書を読んでください。まずH-01のSSE中継をローカル模擬Providerで再現し、修正と回帰テストを行ってください。必要なH-02との接点も調査してください。私用gateway.jsonと資格情報は変更・配布せず、実クラウドやTailscale接続は行わないでください。修正範囲・検証結果・残課題を報告し、文書の実装状態を更新してください。

この文例を依頼者が次のエージェントへ渡した場合に、その範囲での作業依頼となる。


