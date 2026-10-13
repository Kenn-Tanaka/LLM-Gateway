# 修正履歴

本ファイルの導入以降の変更を、新しい順に記録します。導入前の作業内容は既存の要件定義書・実装仕様書・作業引き継ぎ書を参照してください。

## 2026-10-13 — GitHub Actions CI の追加

- **変更対象**: 新規 `.github/workflows/ci.yml`、`CHANGELOG.md`。ソース・EXE・テストの変更はありません。
- **変更内容**: `windows-2022` で、ビルドツリーのキャッシュ → CMake構成（VS 2022 x64）→ Releaseビルド → `ctest` → `verify_imports` → EXEを `bin/` へ配置 → `scripts/package.ps1` → ZIPとSHA256SUMSをアーティファクト保存、を実行するワークフローを追加しました。`main` へのpush、PR、手動実行で起動します。`bin/*.exe` はGit管理外（`.gitignore`）になったため、CI内でビルド成果物を配置してから配布スクリプトを実行します。
- **変更理由**: クリーンな環境でのビルド・テスト・配布物作成を自動化し、手元環境への依存を検出するためです。
- **検証結果**: 未実施です。ワークフローはローカルでは実行できず、GitHub上での初回実行結果の確認が必要です。ローカルでは同等のコマンド（ビルド、`ctest` 7/7、`verify_imports`、`package.ps1`）が成功していることを直前の作業で確認済みです。
- **未解決事項**: 初回実行はwxWidgetsとllama.cppのビルドで長時間かかる見込みです。クリーン環境ではWinCredやポートの条件でテストが失敗する可能性があり、失敗時はログから対応が必要です。

## 2026-10-13 — MITライセンス（HAIA Project）での配布対応

- **変更対象**: 新規 `LICENSE`。更新 `NOTICE.md`、`README.md`、`scripts/package.ps1`、`WORK_HANDOVER.md`、`CHANGELOG.md`。ソース・EXE・テストの変更はありません。
- **変更内容**: MIT License（Copyright (c) 2026 HAIA Project）の `LICENSE` を追加。依存ライセンスを再調査し、`CMakeCache.txt` でwxWidgetsが静的ビルドであり内蔵ライブラリのうちlibpng・zlib・PCREのみがリンクされる（expat・libjpeg・libtiff・liblzmaは無効）こと、MSVCランタイムが静的（`/MT`）であることを確認して、`NOTICE.md` を表形式で拡充しました。`scripts/package.ps1` は `LICENSE` を必須同梱とし、wxWidgets内蔵3種のライセンス全文を `THIRD_PARTY_LICENSES/` へ収集します（欠落時は失敗）。`README.md` のライセンス節を更新しました（編集時に文字コードが壊れたため、UTF-8で全文を再作成）。
- **変更理由**: 依存は全てMITまたは同等の寛容ライセンス（wxWidgetsはバイナリ配布を独自条件で許可する例外付き）であり、MIT配布が可能と判断したためです。
- **検証結果**: `package.ps1` でZIPを再生成し、`LICENSE` と全ライセンス全文が含まれることをSHA256SUMSで確認しました。法的助言ではなく上流ライセンス文書に基づく技術的整理です。
- **未解決事項**: 法的適合の最終確認、llama.cpp内部で実際にビルドされる第三者コードの網羅確認、クリーンWindows環境での起動試験、CIは未実施です。

## 2026-10-13 — Phase 5（P2: H-10 配布構成の整備）

- **変更対象**: 新規 `README.md`、`NOTICE.md`、`.gitignore`、`config/llm-gateway.external-only.example.json`、`scripts/package.ps1`。更新 `WORK_HANDOVER.md`、`CHANGELOG.md`。ソース・EXE・テストの変更はありません。
- **変更内容**: GUI中心の初回起動手順・診断・資格情報の移行条件・組み込みモデルの扱いを記したREADME、第三者ソフトウェア表示（nlohmann/json、cpp-httplib、libcurl、llama.cpp、wxWidgets）、実設定・ログ・モデル・生成物の除外、GGUF不要で秘密を含まない外部専用設定例、許可リスト方式の配布スクリプト（Dropbox等のロック回避のためTEMPで組み立て、`dist/` にZIPとSHA256SUMSを出力。`.log`・`.gguf`・`llm-gateway.json` の混入を拒否し、依存ソースからライセンス全文を収集）を追加しました。
- **変更理由**: フォルダ丸ごと配布による実設定・個人情報の混入を防ぎ、第三者が初回起動できる入口を用意するためです。
- **検証結果**: `scripts/package.ps1` でZIPを生成し、内容が許可リストと一致することをSHA256SUMSで確認（EXEは `bin` と同一ハッシュ）。外部専用設定例（ポート・ログ先のみ一時的に変更した複製）で `llm-gateway.exe` を起動し `/health` が `status: ok, embedded: disabled` を返すことを確認しました。ビルド・CTestはコード未変更のため再実行していません。
- **未解決事項**: 本プロジェクト自体の `LICENSE`（所有者の決定が必要）、依存ライセンスの法的適合・再配布条件の確認、クリーンなWindows 10/11での起動試験、CI、プロジェクト直下に残る試験由来の `llm-gateway-20261013.log`（配布対象外・`.gitignore` 済み、未削除）は一般公開前の課題です。要件定義書・実装仕様書は今回更新していません。

## 2026-10-13 — Phase 4（P2: H-09 ログTail・表示の堅牢化、SEC-L01 定数時間比較の境界修正、H-13 回帰テスト追加）の実装と検証

- **変更対象**:
  - ソース・ヘッダー: `include/gateway/secret.hpp`、`include/maintenance/log_tail_reader.hpp`、`src/maintenance/log_tail_reader.cpp`、`include/maintenance/main_frame.hpp`、`src/maintenance/main_frame.cpp`
  - テスト: `tests/unit/auth_test.cpp`、`tests/integration/health_test.cpp`
  - 実行ファイル: `bin/llm-gateway.exe`、`bin/MaintenanceTool.exe`
  - ドキュメント: `WORK_HANDOVER.md`、`CHANGELOG.md`
- **変更内容**:
  1. **SEC-L01**: `constant_time_equals` の長さ差を `unsigned char` へ縮めず `size_t` で保持し、長さ差が256の倍数かつ追加部分がNULの入力でも不一致になるようにしました。
  2. **H-09**: `LogTailReader` を、ファイル縮小・置換の検知（offsetを0へ戻す）、未完行の次回持越し、改行（0x0A）でのみ分割することによるUTF-8多バイト文字の分断防止、初回tailで途中から読む場合の先頭部分行の破棄、1回の読込量を1MiBに制限、1行64KiB超の未完行の強制出力に改修しました。`MainFrame` は `append_log_lines` で追記をまとめ、表示を最大5000行に制限しました（初回表示にも適用）。
  3. **H-13**: `auth_test` に256/512バイト境界テスト、`health_test` に `test_phase4_h09_log_tail_reader`（未完行・日本語の分割追記・縮小・6万行の大量追記・途中開始tail）を追加しました。
- **変更理由**: 長時間運用でのUI負荷増大・ログ欠落・文字化けの防止、および比較関数の理論上の境界不備の解消のためです。
- **検証結果**: Releaseビルド成功、CTest 7/7成功（2.73秒）、`verify_imports` 成功、`bin` と `build/Release` の両EXEのSHA-256一致（`llm-gateway.exe`: `354CD711ABF3D05D5ABCAFE7A03FDF8C1591A4CBC2E08D6C00792E6156B54353`、`MaintenanceTool.exe`: `3166070C8EC76753C90DF4581F594C4AACBD75EB1B179A2578B92A82EA671575`）、`bin/llm-gateway.json` は不変（`67D3474D…1DEFE3`）。未実施: GUIでの実操作・長時間メモリ計測、固定ログの容量管理、モデル探索の並列化（測定前のため未着手）。
- **未解決事項 / 互換性への影響**: ログ形式・設定の互換性への影響はありません。GUIログ表示は最大5000行（それ以前は破棄、ファイルには残ります）。Phase 5（H-10 配布構成）以降は `WORK_HANDOVER.md` に保持しています。

## 2026-10-13 — Phase 3（P1: H-04 GUI初回設定完結・状態管理、H-08 組み込み推論比較・KV初期化・特殊トークン・境界値検査、H-13 回帰テスト追加）の実装と検証

- **変更対象**:
  - ソース・ヘッダー: `src/gateway/config.cpp`、`include/gateway/embedding_engine.hpp`、`src/gateway/embedding_engine.cpp`、`include/gateway/rerank_engine.hpp`、`src/gateway/rerank_engine.cpp`、`src/gateway/http_server.cpp`、`include/maintenance/config_store.hpp`、`src/maintenance/config_store.cpp`、`include/maintenance/process_controller.hpp`、`src/maintenance/process_controller.cpp`、`include/maintenance/main_frame.hpp`、`src/maintenance/main_frame.cpp`
  - テスト: `tests/unit/config_test.cpp`、`tests/integration/health_test.cpp`
  - 実行ファイル: `bin/llm-gateway.exe`、`bin/MaintenanceTool.exe`
  - ドキュメント: `LLM_Gateway_Requirements_definition_document.md`、`LLM_GATEWAY_MAINTENANCE_TOOL_IMPLEMENTATION_SPEC_2.0.md`、`WORK_HANDOVER.md`、`CHANGELOG.md`
- **変更内容**:
  1. **H-04（GUIでの初回設定完結と編集・保存・稼働適用状態の分離管理）**:
     - `include/maintenance/config_store.hpp` / `src/maintenance/config_store.cpp` に設定ファイル実在確認 `exists_on_disk()`、シリアライズ正規化比較 `configs_equal()`、事前検証 `validate_config()`、純粋な設定編集ヘルパー（`upsert_provider()`、`remove_provider()`［Model Aliasからの参照保護付き］、`upsert_model_alias()`、`remove_model_alias()`）、OpenRouter / LM Studio（Tailscale）プリセット生成（`make_openrouter_preset()`、`make_lm_studio_preset()`）、およびCSVトークン分割・結合（`split_csv_tokens()`、`join_csv_tokens()`）を追加しました。
     - `src/gateway/config.cpp` の `serialize_config` で `ModelFileConfig::n_threads <= 0`（GUIから既定構築された場合）を `default_threads = max(1, hardware_concurrency / 2)` に正規化して出力するように修正しました。
     - `include/maintenance/process_controller.hpp` / `src/maintenance/process_controller.cpp` に `set_active_port()` と `active_port()` を追加し、`restart_pipeline` の `WaitingPortRelease` 段階で旧稼働ポート（`prev_port`）と新設定ポート（`new_config.listen.port`）の両方の解放を確認するように改善しました。
     - `include/maintenance/main_frame.hpp` / `src/maintenance/main_frame.cpp` に以下のGUI編集・状態管理機能を実装しました:
       - 設定ファイル選択（`Open Config...`）および状態バナー（`New (Not Yet Saved)` / `Saved & Applied` / `Unsaved Changes` / `Saved (Restart Required)` / `Legacy v1.0 Loaded`）とウィンドウタイトルへの `[Unsaved]`・`[Restart Required]` 表示。
       - Gateway & Security 設定編集（Listen Port、Client Auth Required、CORS Enable、Allowed Origins CSV、Save Config）。
       - Embedded Engine 設定編集（Enableチェックボックス、Embedding / Reranker GGUF ファイルパス入力・`Browse...`・`Clear`）。
       - External Providers CRUD（一覧選択でフォーム自動反映、Add/Update、Remove、`Preset: OpenRouter`、`Preset: LM Studio`、選択中ProviderのAPIキーを1クリックでWinCredに登録する `Set Selected Provider Key...`）。
       - Model Aliases CRUD（Alias名・Target Provider・Target ModelのAdd/Update・Remove）。
       - 旧形式（v1.0 `gateway.json`）読込後の保存・再起動時に「v2.0形式で上書き」「原本を保持して別名保存（`Save Copy As...`）」「キャンセル」を選択できる3択ダイアログ。
       - 未保存変更がある状態での `Start`、`Open Config...`、ウィンドウ終了（`EVT_CLOSE`）時の確認ダイアログ。
  2. **H-08（組み込み推論のスコア全順序比較・KVキャッシュ初期化・特殊トークン構築・境界値検査）**:
     - `include/gateway/rerank_engine.hpp` / `src/gateway/rerank_engine.cpp` に厳密弱順序（strict weak ordering）を満たす `RerankEngine::compare_rerank_items` と `sort_rerank_items` を実装し、推移律を破っていたイプシロン比較（`|a - b| <= 1e-6` で `index` 順）を廃止して「有限値優先 → `score` 降順 → `index` 昇順」に統一しました。
     - `EmbeddingEngine`（`include/gateway/embedding_engine.hpp` / `src/gateway/embedding_engine.cpp`）と `RerankEngine` の両方で、コンテキスト初期化時に `cparams.n_ubatch = cparams.n_batch` を設定し、各入力の評価前に `llama_kv_cache_clear(ctx_)` を呼び出して先行リクエストのKV状態を完全に消去するようにしました。また `llama_model_has_encoder(model_)` に応じて `llama_encode`（BERT/BGE等のEncoderモデル）と `llama_decode`（Decoderモデル）を自動選択するようにしました。
     - `RerankEngine::format_rerank_tokens` / `format_rerank_token_ids` を実装し、固定の `query + "\n" + doc` 連結ではなく、モデルの語彙（`llama_token_bos` / `llama_token_sep` / `llama_token_eos`）を検査して `[BOS] query [SEP] doc [SEP]`（またはEOS区切り・改行フォールバック）のトークン列を構築するようにしました。
     - トークン数が `max_tokens`（`n_ctx` / `n_batch`）を超える場合や空文書（`""`）の場合は `GatewayException(400, "bad_request", ...)` を返し、`llama_get_embeddings_seq` / `llama_get_embeddings` が `nullptr` または `NaN`/`Inf` を返した場合はスコア `0.0` で継続せず `GatewayException(503, "service_unavailable", ...)` を送出するように修正しました（`src/gateway/http_server.cpp` の `/v1/rerank` でも空文書検査を追加）。
     - 実GGUFファイルを持たない環境でもHTTPエンドポイント経由で連続推論・境界値・並行性を決定論的に検証できるよう、両エンジンにテスト専用の `enable_test_mode()` を追加しました。
  3. **H-13（Phase 3 回帰テスト追加・`bin/` 同期・ドキュメント更新）**:
     - `tests/unit/config_test.cpp` に `test_h08_rerank_sorting_and_token_formatting()` を追加し、旧比較器の反例 `(0,0), (0.75e-6,1), (1.5e-6,2)` の全6順列での推移律・順序不変性、同点時の `index` 昇順、非有限値の末尾配置、`format_rerank_token_ids` のSEP/EOS/改行分岐、および `build/_deps/llama-src/models/ggml-vocab-bert-bge.gguf` を `vocab_only=true` でロードした実トークナイズ検証（`[CLS]=101`、`[SEP]=102`、長文上限超過の400拒否）を実装しました。
     - `tests/integration/health_test.cpp` に `test_phase3_h04_h08()` を追加し、HTTP `/v1/embeddings`・`/v1/rerank` の `A -> B -> A` 連続推論一致性、文書配列の逆順・シャッフル時のトップ結果・スコア不変性、トークン上限超過と空文書の `400 bad_request`、ベクトル抽出失敗（null/NaN注入）時の `503 service_unavailable`、6スレッド×4回の並行リクエスト安全性、ならびに `ConfigStore` による未作成ファイルからの初期設定構築・プリセット追加・Provider/Alias CRUD・参照中Provider削除防止・旧形式設定からの別名保存（原本保全）・`ProcessController` の稼働ポート追跡を検証しました。
     - 検証済みの `build/Release/llm-gateway.exe` と `build/Release/MaintenanceTool.exe` を `bin/` にコピーし、要件定義書・実装仕様書・作業引き継ぎ書の実装状態を更新しました。
- **変更理由**: P1（Phase 3）課題であるGUIのみでの初回設定完結と未保存・保存済み・稼働適用済み状態の明確な分離（H-04）、および組み込み推論エンジンのスコア比較推移律・KVキャッシュ分離・特殊トークン構築・境界値/異常系処理の保証（H-08）を実装し、自動回帰テスト（H-13）で固定するためです。
- **検証結果**:
  - Releaseビルド: `gateway_core`、`maintenance_core`、`llm-gateway`、`MaintenanceTool`、全7テストターゲット、`verify_imports` がエラーなしでビルド成功しました。
  - CTest: `ctest --test-dir build -C Release --output-on-failure -V` を実行し、全7テスト（`config_test`、`router_test`、`auth_test`、`request_guard_test`、`health_test`、`security_test`、`legacy_compat_test`）が **7/7成功（失敗0、2.77秒）** しました。
  - DLL検査・バイナリ整合性: `verify_imports` が両EXEで成功し、`build/Release` と `bin` の両EXEのSHA-256一致（`llm-gateway.exe`: `1CF09AFADA54F4458FD8F289726C756E6868494D6695F15DA449B44302C3072E`、`MaintenanceTool.exe`: `9986C4F9ECDB001E29F4E827F9A49C086A286EE4E61F8062831B0D67A23C2363`）、および `bin/llm-gateway.json` の不変（`67D3474D30133898638498091333D37FB52D22AEE25C15343A9998841D1DEFE3`）を確認しました。
  - 未実施事項と理由: 実OpenRouter / Tailscale（LM Studio）への通信試験、重み付きフルサイズGGUFモデル（`bge-m3` / `bge-reranker-v2-m3`）による推論品質ベンチマーク、GUIの対話的実画面操作、実運用 `gateway.json` や実環境 `CloudLLM/*` 資格情報を用いた変更試験は、引き継ぎ書の安全制約およびテスト環境の構成（語彙専用GGUFと決定論的テストモードを使用）に従い実施していません。
- **未解決事項 / 互換性への影響**:
  - 既存のv2.0設定および旧形式設定の読込互換性は維持されています。GUIで旧形式設定を開いて保存する際は、原本を上書きするか別名で保存するかを選択できるようになりました。また `/v1/rerank` に空文字列の文書が含まれる場合やトークン上限を超える場合は `400 bad_request`、推論エンジンのベクトル抽出失敗時は `503 service_unavailable` が返されます。
  - Phase 4以降の課題（H-09 ログ容量・Tail分割管理、SEC-L01 定数時間比較のサイズ差保持、H-10 配布構成整備、ARCH-L01 共通部分離）は次フェーズ以降の対象として `WORK_HANDOVER.md` に保持しています。

## 2026-10-13 — Phase 2（P1: H-03・H-05・H-06・H-07・H-11・H-12・H-14 の修正および H-13 回帰テスト追加）の実装と検証

- **変更対象**:
  - ソース・ヘッダー: `include/gateway/config.hpp`、`src/gateway/config.cpp`、`src/gateway/main.cpp`、`src/gateway/app.cpp`、`include/gateway/logger.hpp`、`src/gateway/logger.cpp`、`src/gateway/http_server.cpp`、`src/gateway/model_catalog.cpp`、`src/gateway/embedding_engine.cpp`、`src/gateway/rerank_engine.cpp`、`include/maintenance/health_client.hpp`、`src/maintenance/health_client.cpp`、`src/maintenance/config_store.cpp`、`include/maintenance/process_controller.hpp`、`src/maintenance/process_controller.cpp`、`include/maintenance/credential_editor.hpp`、`src/maintenance/credential_editor.cpp`、`src/maintenance/main_frame.cpp`
  - テスト: `tests/unit/config_test.cpp`、`tests/integration/health_test.cpp`、`tests/integration/legacy_compat_test.cpp`
  - 実行ファイル: `bin/llm-gateway.exe`、`bin/MaintenanceTool.exe`
  - ドキュメント: `LLM_Gateway_Requirements_definition_document.md`、`LLM_GATEWAY_MAINTENANCE_TOOL_IMPLEMENTATION_SPEC_2.0.md`、`WORK_HANDOVER.md`、`CHANGELOG.md`
- **変更内容**:
  1. **H-03（組み込みファイル欠落時の障害分離・クラウド継続）**:
     - `include/gateway/config.hpp` / `src/gateway/config.cpp` の `parse_and_validate_config` と `load_config_from_file` の引数 `validate_model_files` の既定値を `false` に変更し、`src/gateway/app.cpp` からの起動時読込ではモデルパスの構文（非空・nullバイトなし・非UNC）のみを検査してファイル存在確認をスキップするように修正しました。
     - `src/gateway/embedding_engine.cpp` と `src/gateway/rerank_engine.cpp` の `init()` で `path_from_utf8` による通常ファイル存在検査とGGUFロードを実行し、欠落・破損時はエラーログを出力して `LoadState::Error` へ遷移させることで、組み込みモデル欠落時でも外部Provider経路（`/v1/chat/completions`、`/v1/models`、外部 `/v1/embeddings`）が正常稼働し、組み込みモデル宛のみ `503 service_unavailable` を返すようにしました。
     - `src/maintenance/process_controller.cpp` の `restart_pipeline`（`WaitingHealth`）で、`new_config.embedded_engine.enabled == true` かつモデル未設定時の `health_opt->embedded == "disabled"` を完了条件に追加し、60秒タイムアウトを防止しました。
     - `src/maintenance/main_frame.cpp` の `refresh_status()` で `health->embedded == "error"` の場合にバッジを `[ Degraded ]`（Amber）、ステータスを `Status: Healthy (Embedded Error)` として表示するようにしました。
  2. **H-05（起動失敗・Healthスキーマ・Latency・Logger・ModelCatalog診断）**:
     - `include/maintenance/health_client.hpp` / `src/maintenance/health_client.cpp` に `HealthClient::parse_health_payload` を追加し、`status == "ok"`、整数 `pid > 0`、非空文字列 `version`、および `embedded`（`disabled` / `loading` / `ready` / `error`）を厳密検証して `{}` 等の別プロセス応答を拒否するようにしました。
     - `src/gateway/http_server.cpp` で `pre_routing_handler` から `svr_.set_logger` までの所要時間を `std::chrono::steady_clock` で計測し、アクセスログの `entry.latency_ms` に実測値を記録するようにしました。
     - `include/gateway/logger.hpp` / `src/gateway/logger.cpp` にログディレクトリ作成失敗やファイルオープン・書込失敗を追跡する `write_error_count()` / `last_write_error()` と初回失敗時の `std::cerr` 警告出力を追加しました。
     - `src/gateway/model_catalog.cpp` でProviderごとのモデル一覧取得失敗時（Bearer資格情報未登録、curl通信エラー、非2xx HTTPステータス、JSONパース・形式エラー）に秘密情報を含まないWARNログを出力するようにしました。
     - `src/gateway/app.cpp` の設定読込失敗時に対象ファイルパス付きのエラーメッセージをstderrおよび `%LOCALAPPDATA%\LLM-Gateway\logs`（またはTEMP）のfallbackログへ記録するようにし、`include/maintenance/process_controller.hpp` / `src/maintenance/process_controller.cpp` の `start_gateway` で匿名パイプにより子プロセスのstderrと終了コードを捕捉して起動直後（100ms）およびヘルス待機中の異常終了原因をGUIへ返すようにしました。
  3. **H-06（保存前設定検証と原子的保存のエラー確認）**:
     - `src/maintenance/config_store.cpp` の `save_atomic` で一時ファイルを作成する前に `parse_and_validate_config(serialize_config(config), false)` を実行し、不正設定時は元ファイルを無変更のまま失敗を返すようにしました。また、ハンドルと一時ファイルを確実に片付ける `TempFileGuard`（RAII）と `FlushFileBuffers` の戻り値確認を追加しました。
     - `src/maintenance/process_controller.cpp` の `restart_pipeline`（`RestartStage::Validating`）でも保存・既存プロセス停止前に `parse_and_validate_config` による検証を実行するようにしました。
  4. **H-07（停止処理のAdmin資格情報名整合・HTTP status・終了検証）**:
     - `include/maintenance/process_controller.hpp` / `src/maintenance/process_controller.cpp` の `stop_gateway` に `admin_credential_name` 引数と起動時設定（`active_port_` / `active_admin_credential_name_`）の保持を追加し、非既定のAdmin資格情報名でも `/admin/shutdown` を認証できるようにしました。
     - `POST /admin/shutdown` のHTTPステータスコード（`200..299`）を確認し、`TerminateProcess` / `WaitForSingleObject` の成否確認およびハンドル未保持時のポート解放待機を行い、停止失敗時は `false` とエラー理由を返すようにしました。`src/maintenance/main_frame.cpp` の `on_stop` で失敗時にエラーダイアログを表示するようにしました。
  5. **H-11（資格情報保存・クリップボード操作結果の厳密通知）**:
     - `include/maintenance/credential_editor.hpp` / `src/maintenance/credential_editor.cpp` の `generate_and_store_token` で `store_.set_credential` の戻り値を確認し、保存失敗時はトークンをzeroizeして空文字列を返すようにしました。また `copy_to_clipboard_temporary` の戻り値を `bool` に変更し、`OpenClipboard` / `EmptyClipboard` / `GlobalAlloc` / `GlobalLock` / `SetClipboardData` がすべて成功した場合のみ `true` を返して30秒消去スレッドを起動するようにしました。
     - `src/maintenance/main_frame.cpp` の `on_gen_client_token`、`on_gen_admin_token`、`on_delete_secret` で保存失敗・クリップボードコピー失敗・削除失敗を個別に判定して通知し、30秒自動消去には `MaintenanceTool` を開いたままにする必要がある旨を明記しました。
  6. **H-12（設定・HTTP APIの厳密型・範囲検証統一）**:
     - `src/gateway/config.cpp` に `require_string` / `require_bool` / `require_string_array` を追加し、現行・旧形式の全数値設定（`listen.port`、`limits.max_request_bytes`、`timeouts.*`、`model_cache.ttl_sec`、`security.cors.max_age_sec`、`embedded_engine.*.n_threads`、`n_gpu_layers`、`proxy.*`）に `integer_in_range` を適用して、浮動小数（例: `port: 18765.5`）の暗黙切り詰めや型不一致による `nlohmann::json::type_error` をすべて `GatewayException(400, "bad_request", ...)` に統一しました。
     - `src/gateway/http_server.cpp` の `/v1/embeddings`（`encoding_format` が文字列であること、`dimensions` が正の整数であること）および `/v1/rerank`（`top_n` が `1..documents.size()` の整数であること、`return_documents` が bool であること）の事前検証を追加し、不正型・範囲外で空の500ではなく400 JSONを返すようにしました。
  7. **H-14（Unicode/日本語/空白/絵文字パスの統一対応）**:
     - `include/gateway/config.hpp` / `src/gateway/config.cpp` に `MultiByteToWideChar(CP_UTF8)` / `WideCharToMultiByte(CP_UTF8)` を用いた `gateway::path_from_utf8` と `gateway::path_to_utf8` を実装し、`config.cpp`、`logger.cpp`、`embedding_engine.cpp`、`rerank_engine.cpp`、`config_store.cpp`、`process_controller.cpp`、`main_frame.cpp`、`legacy_compat_test.cpp` のパス変換を統一しました。
     - `src/gateway/main.cpp` で `GetCommandLineW()` と `CommandLineToArgvW()` からワイド文字列引数を取得してUTF-8へ変換するようにし、ANSIコードページ（`CP_ACP`）外の絵文字（`🧪`）や日本語・空白を含む設定パスでも起動・読込できるようにしました。
  8. **H-13（Phase 2 回帰テスト追加・`bin/` 同期・ドキュメント更新）**:
     - `tests/unit/config_test.cpp` に `test_h12_strict_types_and_ranges` と `test_h03_h14_unicode_paths_and_missing_models` を追加し、`tests/integration/health_test.cpp` に `test_phase2_h03_h05_h06_h07_h11_h12_h14`（欠落GGUF設定での外部チャット中継成功と組み込み503分離、Healthスキーマ検証、latency記録、Logger失敗検知、保存前検証と原本保全、カスタムAdmin名での停止、クリップボードコピー結果、API型検証、`壊れた設定 🧪.json` によるサブプロセス起動失敗stderr捕捉と正常Unicodeパス起動）を追加しました。
     - 検証済みの `build/Release/llm-gateway.exe` と `build/Release/MaintenanceTool.exe` を `bin/` にコピーし、要件定義書・実装仕様書・作業引き継ぎ書の実装状態を更新しました。
- **変更理由**: P1（Phase 2）課題である組み込みモデル障害時の外部経路継続（H-03）、起動・通信・ログ・Healthの診断性向上（H-05）、保存前検証と原子的保存の安全性強化（H-06）、停止時のAdmin資格情報名整合と終了確認（H-07）、資格情報とクリップボード操作結果の正確な通知（H-11）、設定・APIの厳密な型・範囲検証統一（H-12）、日本語・空白・絵文字を含むUnicodeパスの一貫処理（H-14）を実装し、自動回帰テスト（H-13）で保証するためです。
- **検証結果**:
  - Releaseビルド: `gateway_core`、`maintenance_core`、`llm-gateway`、`MaintenanceTool`、全7テストターゲット、`verify_imports` がエラー・警告なしでビルド成功しました。
  - CTest: `ctest --test-dir build -C Release --output-on-failure` を実行し、全7テスト（`config_test`、`router_test`、`auth_test`、`request_guard_test`、`health_test`、`security_test`、`legacy_compat_test`）が **7/7成功（失敗0、2.09秒）** しました。
  - DLL検査・バイナリ整合性: `verify_imports` が成功し、`build/Release` と `bin` の両EXEのSHA-256一致、`bin/llm-gateway.exe --version`（`2.0.0`）の動作、および `bin/llm-gateway.json` の不変を確認しました。
  - 未実施事項と理由: 実OpenRouter / Tailscale（LM Studio）への通信試験、実GGUFモデルの推論試験、GUIの対話的操作確認、実運用 `gateway.json` や実環境 `CloudLLM/*` 資格情報を用いた変更試験は、引き継ぎ書の安全制約およびPhase 2の作業範囲に従い実施していません。
- **未解決事項 / 互換性への影響**:
  - 既存のv2.0設定および旧形式設定の正常値の読込互換性は維持されています。従来暗黙に切り詰められていた浮動小数設定値（例: `port: 8765.5`）や不正型のAPIパラメータ（例: `encoding_format: 123`）は `400 bad_request` JSONで明示的に拒否されます。また、設定読込時のモデルファイル実在検査は既定でランタイム初期化時（`EmbeddedEngine`）に移行し、ファイル存在まで事前検証したい場合は `parse_and_validate_config(..., true)` を指定する仕様となりました。
  - Phase 3以降の課題（H-04 初回設定GUI完結、H-08 組み込み推論スコア比較・境界検証、H-09 ログ容量・Tail分割管理、H-10 配布構成整備、SEC-L01、ARCH-L01）は次フェーズ以降の対象として `WORK_HANDOVER.md` に保持しています。

## 2026-10-13 — Phase 1（P0: H-01 SSE中継修正、H-02 非同期寿命・要求カウンター修正、H-13 回帰テスト追加）の実装と検証

- **変更対象**:
  - ソース・ヘッダー: `include/gateway/shutdown_coordinator.hpp`、`include/gateway/http_server.hpp`、`src/gateway/http_server.cpp`、`src/gateway/external_proxy.cpp`、`include/maintenance/process_controller.hpp`、`src/maintenance/process_controller.cpp`、`include/maintenance/main_frame.hpp`、`src/maintenance/main_frame.cpp`
  - テスト: `tests/unit/auth_test.cpp`、`tests/integration/health_test.cpp`、`tests/security/security_test.cpp`、`tests/integration/legacy_compat_test.cpp`
  - ビルド設定・実行ファイル: `build/` 配下の自動生成CMake/MSBuildパス参照（338ファイル、第三者ソース `build/_deps/*-src` を除く）、`bin/llm-gateway.exe`、`bin/MaintenanceTool.exe`
  - ドキュメント: `LLM_Gateway_Requirements_definition_document.md`、`LLM_GATEWAY_MAINTENANCE_TOOL_IMPLEMENTATION_SPEC_2.0.md`、`WORK_HANDOVER.md`、`CHANGELOG.md`
- **変更内容**:
  1. **H-01（SSEストリーミング中継と上流プロキシ設定統合）**:
     - `src/gateway/external_proxy.cpp` に `is_top_level_stream_request()` を実装し、部分文字列検索ではなくJSONトップレベルのboolフィールド `"stream": true` のみでSSE要求を判定するように修正しました（入れ子の `metadata.stream` やメッセージ本文内の文字列は無視）。また `src/gateway/http_server.cpp` の `POST /v1/chat/completions` でトップレベル `stream` が bool または null 以外の場合は上流接続前に `400 bad_request` を返す検証を追加しました。
     - 通常応答とSSE応答のlibcurl初期化・ヘッダー選別・アップロード進捗監視を `UpstreamCurlTransfer`（`curl_multi` + `curl_easy`）に統合し、1要求につき上流POSTを1回のみ実行する構造に改めました。
     - `stream: true` 時はまず上流レスポンスヘッダー完了まで `curl_multi` を駆動し、通信失敗時は `502`/`504` JSON、上流HTTPステータスが `400` 以上（`429` 等）または `Content-Type` が `text/event-stream` 以外の場合は残りの本文を読み切って上流のステータス・ヘッダー・エラー本文をそのままクライアントへ返すように修正しました。
     - 上流が `2xx` かつ `text/event-stream` の場合のみ `res.set_chunked_content_provider` を登録し、初期受信済みチャンクの即時flush、到着順の逐次 `sink.write`、正常完了時の `sink.done()`、クライアント切断時（`!sink.is_writable()` または `sink.write` 失敗）の即時中断・解放を実装しました。
  2. **H-02（HTTP要求カウンター・ShutdownCoordinator・GUI再起動ワーカー寿命）**:
     - `include/gateway/shutdown_coordinator.hpp` にムーブ可能なRAIIガード `ShutdownCoordinator::RequestGuard`、`acquire_guard()`、`active_requests()`、`wait_for_drain()`、負値アンダークランプ、デストラクタで安全にキャンセル・`join` される所有 `shutdown_thread_`、およびインプロセステスト用の `set_shutdown_action()` を追加しました。
     - `src/gateway/http_server.cpp` で `pre_routing_handler` 内の早期 `leave_request()` と `post_routing_handler` を廃止し、`pre_routing_handler` で取得した `thread_local ShutdownCoordinator::RequestGuard` を `svr_.set_logger`（HTTPレスポンスおよびSSE chunked content providerの送信完了後に呼ばれるフック）で解放する方式に統一しました。併せて `svr_.set_exception_handler` と `wait_until_ready()` を追加しました。
     - `ProcessController`（`include/maintenance/process_controller.hpp` / `.cpp`）に `mutex_` と協調的キャンセルAPI（`request_cancel()` / `reset_cancel()` / `is_cancel_requested()`）を追加し、停止待機・ポート解放待機・ヘルスチェック待機ループでキャンセル要求とプロセスの早期終了を即座に検出するようにしました。
     - `MainFrame`（`include/maintenance/main_frame.hpp` / `.cpp`）で再起動ワーカーの `.detach()` を廃止して所有メンバ `restart_worker_` に変更し、永続フラグ `is_busy_`（500msタイマー `refresh_status()` によるボタン再有効化や組み込みチェックボックス変更を防止）、設定とパスの値スナップショット、`ui_alive_` による破棄後 `CallAfter` 抑止、`~MainFrame()` での `request_cancel()` + `join_restart_worker()`、および `Save & Restart` で保存段階に到達した際の `loaded_from_legacy = false` 更新を実装しました。
  3. **H-13（Phase 1 回帰テスト追加）**:
     - `tests/unit/auth_test.cpp` に合成サービス名（`LLMGatewayPhase1AuthTest_Synthetic`）とRAII削除ガードを用いた `Authenticator` 単体テスト（`client_auth=false` バイパス、未登録資格情報401、ヘッダー欠落・重複・スキーム不正・末尾空白拒否、Client/Admin Bearerトークン照合）を追加しました。
     - `tests/integration/health_test.cpp`、`tests/security/security_test.cpp`、`tests/integration/legacy_compat_test.cpp` に、H-01（時間差3チャンクSSEの逐次到着、上流POST 1回実行、上流429のステータス・`Retry-After`・JSON本文透過、クライアント途中切断時の早期キャンセル、入れ子 `stream:true` 無視と非bool `stream` の400拒否）および H-02（Host/Origin拒否・OPTIONS・413・404・400・SSE全経路での `active_requests() == 0` 不変性、SSE実行中のgraceful shutdown待機と新規要求503拒否、`ProcessController::restart_pipeline` の協調的キャンセル）の自動回帰テストを追加しました。
  4. **ビルド環境調整・実行ファイル反映・ドキュメント同期**:
     - 作業フォルダのユーザーパス変更（`C:/Users/kenta/...` → `C:/Users/User/...`）に伴い、`build/` 内の生成済みCMake/MSBuildファイル338件のパス参照を更新しました。
     - 検証済みの `build/Release/llm-gateway.exe` と `build/Release/MaintenanceTool.exe` を `bin/` にコピーし、要件定義書・実装仕様書・作業引き継ぎ書の実装状態を更新しました。
- **変更理由**: P0課題であるSSE中継の上流重複POST・エラーステータス喪失・未完了ハング（H-01）と、HTTP要求カウンターの二重減算・SSE送信中の早期解放・GUI再起動ワーカーの破棄後アクセス競合（H-02）を解消し、自動回帰テスト（H-13）で再発を防止するためです。
- **検証結果**:
  - Releaseビルド: `llm-gateway`、`MaintenanceTool`、全7テストターゲット、`verify_imports` がすべてエラーなしでビルド成功しました。
  - CTest: `ctest --test-dir build -C Release --output-on-failure -V` を実行し、全7テスト（`config_test`、`router_test`、`auth_test`、`request_guard_test`、`health_test`、`security_test`、`legacy_compat_test`）が **7/7成功（失敗0、約4.05秒）** しました。
  - DLL検査・バイナリ整合性: `bin/llm-gateway.exe` と `bin/MaintenanceTool.exe` に対する `cmake/VerifyImports.cmake` がともに成功し、`build/Release` と `bin` の両EXEのSHA-256一致、および `bin/llm-gateway.json` の前後SHA-256一致（不変）を確認しました。
  - 未実施事項と理由: 実OpenRouter / Tailscale（LM Studio）への通信試験、実GGUFモデルの推論試験、GUIの対話的操作確認、実運用 `gateway.json` や実環境 `CloudLLM/*` 資格情報を用いた変更試験は、引き継ぎ書の安全制約およびPhase 1の作業範囲に従い実施していません。
- **未解決事項 / 互換性への影響**:
  - 既存のv2.0設定および旧形式設定の読込互換性は維持されています。トップレベル `stream` に非bool値（文字列 `"true"` 等）を渡す不正リクエストは `400 bad_request` として明示的に拒否されます。
  - P1以降の課題（H-03 組み込みモデル欠落時のクラウド継続、H-04 初回設定GUI、H-05 診断改善、H-06 保存検証、H-07 停止結果・Admin名整合、H-08 組み込み推論比較・境界、H-09 ログ容量管理、H-10 配布整備、H-11 資格情報操作結果通知、H-12 数値/型検証統一、H-13 残課題分の回帰テスト、H-14 Unicode設定パス、SEC-L01、ARCH-L01）は次フェーズ以降の対象として `WORK_HANDOVER.md` に保持しています。

## 2026-10-13 — プロジェクト全体の再レビューを引き継ぎ書へ反映

- **変更対象**: `WORK_HANDOVER.md`、`CHANGELOG.md`。
- **変更内容**: 調査範囲、今回の検証結果、実装状況、良好な点、追加・統合・削除候補、未確認事項を追記しました。H-01～H-10を再評価し、資格情報操作の誤成功通知（H-11）、型検証（H-12）、テスト範囲（H-13）、Unicode設定パス（H-14）を追加しました。High 5件・Medium 9件と軽微な候補2件に整理し、根拠の行番号・確信度・受入条件・依存関係・作業順を更新しました。
- **変更理由**: 次のエージェントが、推定と実再現を区別し、現在の実装に対応した優先順位で作業を引き継げるようにするためです。
- **検証結果**: 既存CTestは7成功・0失敗・スキップ0（0.52秒）、両EXEのVerifyImportsは成功しました。合成設定とローカル模擬Providerで、SSEの上流POST反復・429の200化・入れ子stream誤判定、不正encoding_formatによる空500、モデル欠落時の全体停止、小数portの切り詰め、絵文字入り設定パスの読込失敗を再現しました。引き継ぎ書の参照先・行番号範囲・14課題ID・重要度件数・Markdownリンクを確認しました。
- **保全と制約**: ソース・ヘッダー・テスト・ビルド設定・設定例・bin等58ファイルの前後SHA-256は一致しました。今回は再ビルド、GUI実操作、実Provider接続、実GGUF推論、私用設定・資格情報の変更は行っていません。
- **生成物**: CTestの`build/Testing/Temporary`記録とTEMP内の合成設定・ログ・結果JSON・文書編集用テキスト。所在は引き継ぎ書に記載しました。配布対象ではありません。
- **未解決事項 / 互換性への影響**: 不具合の修正は未実施です。コード・設定・EXEの挙動は変更していません。

## 2026-10-13 — 修正履歴の更新ルールを導入

- **変更対象**: `AGENTS.md`、`CHANGELOG.md`（ともに新規作成）。
- **変更内容**: ファイルの修正・追加・削除を行うたびに、作業単位で修正履歴を更新する指示と記録先を追加しました。
- **変更理由**: 別のコーディングエージェントへ引き継いだ後も、変更内容・理由・検証結果を追跡できるようにするためです。
- **検証結果**: 両ファイルの記載内容と履歴の参照先を確認しました。文書のみの変更のため、ビルド・動作テストは実施していません。
- **互換性への影響**: アプリケーションのコード・設定・EXEに変更はありません。
