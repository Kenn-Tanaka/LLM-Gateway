# LLM Gateway ＆ MaintenanceTool 実装仕様書（文書版2.1）

- 更新日: 2026-10-13
- 対応: [要件定義書 第2.1版](LLM_Gateway_Requirements_definition_document.md)
- ファイル名の2.0は既存参照維持のため残す。設定schema_versionは「2.0」、EXE表示版は2.0.0のまま。
- 本書は現実装を記述し、未達・改善対象を分離する。設計上望ましい動作を実装済みと表現しない。
- [設定互換性の利用説明](CONFIG_COMPATIBILITY.md) / [作業引き継ぎ書](WORK_HANDOVER.md)

## 1. 構成と実装境界

| 対象 | ファイル群・役割 |
|---|---|
| Gateway起動 | src/gateway/main.cpp、app.cpp。CLI、AVX2、Mutex、設定、ログ、HTTP起動 |
| 設定 | include/gateway/config.hpp、src/gateway/config.cpp。旧形式正規化、検証、入出力 |
| HTTP入口 | http_server.cpp、request_guard.cpp、auth.cpp |
| 外部経路 | model_router.cpp、model_catalog.cpp、external_proxy.cpp |
| 組み込み | embedded_engine.cpp、embedding_engine.cpp、rerank_engine.cpp |
| 共通基盤 | credential_store.cpp、logger.cpp、secret.hpp、shutdown_coordinator.hpp |
| GUI | src/maintenance/main.cpp、main_frame.cpp |
| GUIサービス | config_store.cpp、credential_editor.cpp、process_controller.cpp、health_client.cpp、log_tail_reader.cpp |
| ビルド | CMakeLists.txt、cmake/Dependencies.cmake、StaticRuntime.cmake、VerifyImports.cmake |
| 試験 | tests/unit、tests/integration、tests/security |
| 出力 | build/ReleaseのEXE・ライブラリ、手動更新するbinの両EXE |

Gatewayはcpp-httplibで受信し、libcurl/SChannelで上流へ接続する。組み込み推論はllama、GUIはwxWidgets。gateway_coreは設定・認証だけでなく推論とHTTPも含む。maintenance_coreはgateway_core全体にリンクしており、以前の文書にあった「小規模共通ライブラリだけ共有」は未実装である。

## 2. 起動と設定パス

### 2.1 Gateway

CLI: llm-gateway.exe [--config <path>] [--no-embedded] [--version]

- 不明引数、重複--config、値なしは終了コード2。
- --versionは2.0.0を表示して終了。
- 通常起動はCPUID/XGETBVによるAVX2検査、Local名前空間のLLM_Gateway_SingleInstance_Mutex取得、設定読込の順。
- --configを優先する。未指定時はLOCALAPPDATA/LLM-Gateway/llm-gateway.json。
- Debugの開発fallbackは現在の作業ディレクトリのllm-gateway.json。EXE隣接ではない。Releaseはこのfallbackを使わない。
- 設定読込後にLogger、CredentialStore、Winsock、EmbeddedEngine、HttpServerを構築する。
- 組み込みロードワーカー開始後、同期listenループに入る。モデルロードそのものはワーカー内で行う。
- Client/Admin資格情報の欠落は警告し、起動は継続する。
- 設定不正・CPU不適合・Mutex競合・bind失敗は終了コード1。設定読込失敗等の早期失敗時はstderrに出力するとともにLOCALAPPDATA（またはTEMP）のfallbackログへ対象パス付きで記録する。
- --no-embeddedはファイルを変更せず、エンジン起動を無効化する。
- main.cppでGetCommandLineWとCommandLineToArgvWからUTF-8引数へ変換し、config.cppのpath_from_utf8/path_to_utf8（MultiByteToWideChar/WideCharToMultiByte CP_UTF8）で一貫して処理するため、日本語・空白・絵文字を含むUnicodeパスに対応する。

### 2.2 MaintenanceTool

MaintenanceTool.exe [--config <path>]を受け付ける。wxCmdLineParserのUnicode文字列からfilesystem::pathを生成し、明示したファイルが通常ファイルでなければエラーダイアログを表示する。

未指定時のConfigStore選択順は、存在するLOCALAPPDATA設定→書き込み可能ACLのEXE隣接設定→新規作成先としてLOCALAPPDATA。新規ファイルがないとGatewayConfigの既定値を返す。GUIには設定ファイル選択ダイアログがない。

GUIは隣接llm-gateway.exeをCreateProcessW、CREATE_NO_WINDOW、SW_HIDEで開始する。設定とEXEはwstringでWindowsの引用符規則に従って渡す。作業ディレクトリはGateway EXEの親。Gatewayを直接起動する場合とGUI経由で、相対ログ/モデルパスの基準が違うことに注意する。

## 3. 設定データ

### 3.1 判定と内部モデル

parse_and_validate_configが入力objectにschema_versionがなければ旧形式としてnormalize_legacy_configへ渡す。その後、現行形式のパーサーで検証する。schema_versionが存在する場合は文字列「2.0」のみ受理する。

GatewayConfig.loaded_from_legacyはメモリ内の由来フラグ。JSONへ保存しない。serialize_configは常に現行形式を生成する。未知フィールドは各設定階層で検査する。すべての文字列・ブール・文字列配列・整数フィールドはrequire_string / require_bool / require_string_array / integer_in_rangeで型と範囲を厳密検証し、型不一致・浮動小数・範囲外はすべてGatewayException(400, "bad_request", ...)へ正規化する（H-12解消済み）。

### 3.2 現行形式の主要設定

| 設定 | 既定値 / 検証・動作 |
|---|---|
| listen.host / port | 127.0.0.1のみ / 8765、1024～65535 |
| limits.max_request_bytes | 10485760、範囲1～268435456 bytes |
| timeouts.upstream_connect_ms | 5000、範囲1～3600000ms |
| timeouts.upstream_read_ms | 60000、範囲1～3600000ms。curl総転送期限 |
| timeouts.upstream_write_ms | 60000、整数1～3600000ms。アップロード進捗停止期限 |
| model_cache.enabled / ttl_sec | true / 3600、TTL整数0～31536000秒。0は再利用しない |
| logging.level | INFO。ERROR/WARN/INFO/DEBUG |
| logging.dir | LOCALAPPDATA/LLM-Gateway/logsを環境変数展開 |
| logging.file | 省略時は日別ログ。指定時は空でない固定パスを優先 |
| security.client_auth | true |
| security.credential_name | llm_gateway|default |
| security.admin_credential_name | llm_gateway|admin |
| security.cors.enabled | true。ただしallowed_originsの既定は空 |
| security.cors.allowed_methods | GET、POST、OPTIONS。設定可能値はGET/POST/PUT/PATCH/DELETE/OPTIONS |
| security.cors.allowed_headers | Authorization、Content-Type。HTTP token文字の検査あり |
| security.cors.max_age_sec | 600、整数0～31536000秒 |
| credentials.service_name | CloudLLM |
| credentials.environment_fallback | false |
| embedded_engine.enabled | true。旧形式からの変換時はfalse |
| embedding_model / reranker_model | 任意のモデル設定。path、n_threads、n_gpu_layers |
| n_threads / n_gpu_layers | スレッド既定max(1, hardware_concurrency/2) / GPU層数0のみ |
| models | Alias→provider、model、任意input_prefix |
| providers | ID→base_url、authentication、key_vendor、key_slot、model_discovery、allowlist |

Provider IDは小文字英数字とハイフン・アンダースコア、Aliasは英数字とピリオド・ハイフン・アンダースコアを基本とする。長さ等の正確な正規表現はconfig.cppの検証関数が基準。外部Providerのbase_urlはhttp/httpsでuserinfo・fragmentを禁止し、embeddedには外部URLを使用しない。モデルパスのUNCを拒否する。

現行形式・旧形式の全数値設定（port、limits.max_request_bytes、timeouts.*、model_cache.ttl_sec、security.cors.max_age_sec、embedded_engine.*.n_threads / n_gpu_layers、proxy.*）にinteger_in_rangeを適用し、浮動小数・文字列・bool・範囲外整数をすべて400 bad_requestで拒否する。

### 3.3 旧形式アダプター

| 旧項目 | 内部変換 / 省略時 |
|---|---|
| schema_versionなし | 「2.0」を付与し、loaded_from_legacy=true |
| listen.address | listen.hostへ移動 |
| proxy.max_request_body_mb | 1～256の整数MiB×1048576 / 64 |
| proxy.connect_timeout_sec | 1～3600の整数秒×1000 / 10 |
| proxy.read_timeout_sec | 同上 / 600 |
| proxy.write_timeout_sec | 同上 / 600 |
| model_cache | 現行設定に引き継ぐ。省略値は共通既定を使用 |
| トップレベルcors | security.corsへ移動。cors自体の省略時はenabled=false |
| logging.level | 大文字化 / info |
| logging.file | 固定ファイルとして保持 / llm-gateway.log |
| providers.*.model_discovery | 指定を保持 / true |
| bearerのkey_slot | 指定を保持 / default |
| allowlistのtype・pattern | exactまたはglobのobjectを文字列へ変換 |
| base_url | 末尾スラッシュを除き、/v1で終わらなければ/v1を補完 |
| embedded_engine | enabled=falseを新規設定 |

旧形式のトップレベル受理項目はlisten/security/credentials/proxy/model_cache/cors/providers/loggingのみ。新旧項目の混在は拒否する。exactはワイルドカードを含められない。globも最終的に現行の完全一致・単独*・末尾/*の範囲で検証する。任意glob/regexの互換エンジンは持たない。

例: https://openrouter.ai/api → https://openrouter.ai/api/v1。既に/v1のLM Studioは重複追加しない。この補完は旧形式だけに適用する。現行base_urlは完全なAPIベースを指定する。

読込だけで元ファイルを書き換えない。既存旧実設定でProvider 3件の読込とファイルSHA-256不変を確認済み。個人の実設定をテストfixtureへ埋め込まない。

### 3.4 保存

ConfigStore::save_atomicは保存前にserialize_config(config)をparse_and_validate_config(..., false)で完全再検証し、不正設定の場合は一時ファイルを作らず元ファイルを無変更のまま失敗を返す（H-06解消済み）。検証通過後、親ディレクトリ作成、ランダム接尾辞.tmpをCREATE_NEW、JSON dump(2)をWriteFile、FlushFileBuffers（戻り値検査あり）、MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)の順で保存し、途中失敗時はTempFileGuard（RAII）でハンドル閉鎖と一時ファイル削除を行う。一時ファイルに専用ACLを指定せず、ディレクトリのACLを継承する。

MainFrame::confirm_config_format_changeは旧由来フラグがある場合、Save Config/Save & Restartの前に2.0形式への変換を警告する。Noを既定とする。Save ConfigおよびSave & Restartで保存段階に到達した後は由来フラグ（loaded_from_legacy）を解除する。

旧形式をそのまま再保存する機能はない。ProcessController::restart_pipelineのValidating段階でも保存・停止前にparse_and_validate_configによる事前検証を実行する。

## 4. HTTP・認証・Routing

HttpServerが以下を登録する。

| API | 認証 / 処理 |
|---|---|
| GET /health | トークン不要、status=ok、PID、version=2.0.0、embedded集約状態 |
| GET /v1/models | Client認証設定に従いModelCatalogを呼ぶ |
| POST /v1/chat/completions | Client認証、JSON/model/stream型解析、ルート解決、外部へ/chat/completions |
| POST /v1/embeddings | Client認証、input/encoding_format/dimensions型・範囲解析、Alias prefix、組み込みまたは外部/embeddings |
| POST /v1/rerank | Client認証、query/documents/top_n/return_documents型・範囲検査、組み込みのみ |
| POST /admin/shutdown | Admin認証、空本文、ShutdownCoordinator |
| OPTIONS | RequestGuardのCORSプリフライト処理 |

Hostは127.0.0.1:portとlocalhost:portの完全一致。Originは設定との完全一致。CORS無効時はOrigin付き要求を拒否するが、OriginなしのネイティブClientは拒否しない。許可ヘッダー名の照合は大文字小文字を区別しない。Access-Control-Allow-Credentialsは送らない。CORSにPATCH等を追加してもAPIルートは追加されない。

本文上限はset_payload_max_lengthとRequestGuardで検査し、413はrequest_body_too_largeのJSONへ変換する。HttpServerはset_exception_handlerを登録し、GatewayExceptionはその定義エラーJSONへ、その他の未捕捉例外は秘密を含まない500 internal_error JSONへ変換する。`/v1/embeddings`の`encoding_format`（文字列）・`dimensions`（正の整数）および`/v1/rerank`の`top_n`（1〜documents数の整数）・`return_documents`（bool）も事前検証で400 bad_requestへ統一済み（H-12解消済み）。

要求受付カウンターはpre_routing_handlerでShutdownCoordinator::acquire_guard()をthread_localガードに保持し、応答ヘッダー・本文・SSE chunked content providerの送信がすべて完了した後に呼ばれるsvr_.set_loggerで解放する。あわせてpre_routing_handlerの開始時刻からset_loggerまでの所要時間（ms）を計測し、アクセスログのlatency_msへ記録する（H-05解消済み）。これによりHost/CORS拒否、OPTIONS、413、404、通常応答、SSEのいずれも1要求につき1回だけ加算・減算される。

ModelRouterはAlias、またはprovider/modelを解決し、Allowlistを評価する。組み込み直接指定はembedding_model/reranker_modelまたは設定ファイル名とエンドポイントの組み合わせを照合する。チャットのembedded経路は非対応。

## 5. 外部Provider中継

ExternalProxyはbase_urlとサブパスを連結する。転送するClientヘッダーはContent-Type/Accept/User-Agentを選択し、Provider BearerはCredentialStoreから取得して付与する。通常応答とSSEは共通のUpstreamCurlTransfer（curl_multi + curl_easy）初期化・ヘッダー選別・アップロード進捗監視を使用する。TLS検証有効、redirect無効。

### 5.1 通常応答

トップレベルJSONのstreamがtrueでない要求は通常応答として扱い、curl_multiで完了まで駆動して上流HTTP status、Content-Type、除外対象以外のヘッダーと本文を返す。curl失敗は502、総期限またはupload期限超過は504。上流の401/429等は通常のHTTP応答として中継する。

接続期限はCURLOPT_CONNECTTIMEOUT_MS、read設定はCURLOPT_TIMEOUT_MS。write設定はXFERINFOコールバックでアップロード済みbytesの進捗停止を検査する。旧実装と同一のidle read/write semanticsではない。

### 5.2 SSE（stream: true）

JSONトップレベルのboolフィールド`stream == true`で判定する（入れ子の`metadata.stream`や文字列内の`"stream":true`は無視し、トップレベル`stream`がbool/null以外ならHttpServerが400 bad_requestを返す）。

1要求につきUpstreamCurlTransferを1つだけ生成し、まず上流応答ヘッダー確定（または転送終了）までcurl_multiを駆動する。
- ヘッダー確定前にcurlエラー・タイムアウトが発生した場合は502/504 JSONを返す。
- 上流HTTP statusが400以上（401/429/500等）、またはContent-Typeが`text/event-stream`でない場合は、残りのエラー本文を読み切って上流のstatus・ヘッダー・本文をそのままClientへ返す（200 SSEへ化けない）。
- 上流が2xxかつ`text/event-stream`の場合は、`res.set_chunked_content_provider`を登録し、ヘッダー確定時に受信済みの初期チャンクを即時flushした上で、後続チャンクを到着順に`sink.write`で逐次中継する。上流正常完了時に`sink.done()`を1回呼び、Client切断（`!sink.is_writable()`または`sink.write`失敗）時は上流転送を即座に中断・解放する。

ローカル模擬上流による自動試験（時間差3チャンク逐次到着、上流POST 1回、429透過、切断時cancel、入れ子stream無視）は完了済み。実クラウドProviderへの接続試験は未実施。

## 6. モデル一覧とcache

ModelCatalogはAliasを列挙し、model_discovery=trueの各Providerのbase_url/modelsへ順次GETする。HTTP 2xxかつdata配列を含むJSONを成功とし、idをprovider/idに変換してAllowlistを適用する。

Provider単位のcacheには取得完了時刻を記録する。enabled=falseまたはttl_sec=0なら再利用しない。成功した空一覧もcacheする。探索失敗（資格情報未登録、curl通信失敗、非2xx応答、JSON不正）はProvider IDと失敗理由をLoggerのWARNへ出力し（秘密値は含めない）、空一覧成功と区別する。探索を試みた全Providerが失敗し、Alias・組み込みモデルを含む返却候補が空なら502を返す。それ以外は得られた一覧を返す。

組み込みreadyモデルを加え、ID重複排除・昇順整列する。Alias一覧は実際の到達性確認結果ではない。Provider探索は逐次であり、長いtimeoutの到達不能Providerが一覧取得を遅らせる。失敗Providerを個別に示すGUI上の診断UIは未実装。

### 7. 組み込み推論

EmbeddedEngineはEmbedding/Rerankを独立して保持し、1本のロードワーカーで順次initする。状態はDisabled/Loading/Ready/Error。集約状態はロード中を優先し、対象すべてreadyならready、失敗があればerror、対象がなければdisabled。デストラクタでロードワーカーをjoinする。

各推論に独立mutexを持つ。Phase 3（H-08）により以下の推論信頼性・境界仕様を実装済みである:
- **アーキテクチャ分岐とコンテキスト設定**: `init()`でencoder-decoderモデル（`llama_model_has_encoder && llama_model_has_decoder`）を明示的に拒否して`LoadState::Error`とし、非causal encoderモデルのために`cparams.n_ubatch = cparams.n_batch`を設定する。推論時はencoder-onlyモデルに`llama_encode`、decoder-onlyモデルに`llama_decode`を使い分ける。
- **KVキャッシュ毎回クリア**: `EmbeddingEngine::compute_embeddings`の入力ごと、および`RerankEngine::compute_rerank`の`(query, doc)`組ごとに`llama_kv_cache_clear(ctx_)`を実行し、連続要求（`A -> B -> A`）や文書順序入替で前回状態が一切リークしないことを保証する。
- **Rerank特殊トークン整形**: `RerankEngine::format_rerank_tokens`（および`format_rerank_token_ids`）により、モデル語彙に`EOS`または`SEP`トークンが定義されている場合は`llama.cpp`の`format_rerank`と同じ`[BOS] query [EOS] [SEP] doc [EOS]`形式でトークン列を構築する（未定義時のみ`query + "\n" + doc`へフォールバック）。
- **Rerank厳密弱順序（Strict Weak Ordering）**: `RerankEngine::compare_rerank_items`および`sort_rerank_items`（`std::stable_sort`）により、非NaNをNaNより前、有限スコアは`relevance_score`の厳密降順、同点時は`index`昇順で並べる全順序を実装し、`1e-6f`丸めによる非推移性サイクルを解消した。
- **抽出失敗・非有限値・トークン上限・空文書のエラー分離**: Embedding/Rerank抽出結果が`nullptr`または非有限値（`NaN`/`Inf`）の場合は`score=0`の200 OKにせず、503 `service_unavailable`（`embedded_load_failed`）を返す。トークン数が`max_tokens_`（`min(n_ctx, n_batch)`）を超える場合や`/v1/rerank`に空文字列文書が含まれる場合は400 `bad_request`を返す。

設定読込時はモデルパスの構文（非空・nullバイトなし・非UNC）のみを検査し、既定（`validate_model_files = false`）ではファイル存在検査をスキップしてGateway起動を継続する（H-03解消済み）。ワーカー内の`EmbeddingEngine::init()` / `RerankEngine::init()`で`path_from_utf8`によりファイル存在とGGUFロードを検査し、欠落・破損時はエラーログを出力して該当エンジンを`LoadState::Error`へ遷移させる。これにより組み込みモデルが欠落・破損していても外部Provider（`/v1/chat/completions`、`/v1/models`、外部`/v1/embeddings`）は正常稼働し、組み込みモデル宛の要求のみが503 `service_unavailable`となる。

## 8. 資格情報

CredentialStoreはWinCred汎用資格情報を使用する。Providerターゲットはservice_name/<vendor>|<slot>。Providerのみenvironment_fallbackを許可し、LLM_GATEWAY_KEY_<VENDOR>_<SLOT>を使用する。Client/Adminにfallbackはない。

GUIはBCryptGenRandom由来の32 bytesをBase64URL化したトークンを登録・コピーする。`CredentialEditor::generate_and_store_token`はWinCred保存成功を確認してからトークンを返し（失敗時はzeroizeして空文字を返す）、`copy_to_clipboard_temporary`は`OpenClipboard`/`EmptyClipboard`/`GlobalAlloc`/`SetClipboardData`の成否を`bool`で返し、成功時のみ30秒後の消去スレッドを起動する。MainFrameは保存成否・クリップボードコピー成否・削除成否を個別に判定してダイアログ表示し、30秒自動消去にはMaintenanceToolを開いたままにする必要がある旨を案内する（H-11解消済み）。Secretの所有バッファにzeroizeを用いるが、すべての一時stringやcurl内部コピーの消去を保証するものではない。

ProcessController::stop_gatewayは設定の`security.admin_credential_name`を受け取り（起動時に記憶した値または引数）、非既定のAdmin資格情報名でも正しくBearerトークンを取得して`/admin/shutdown`を呼び出す（H-07解消済み）。

## 9. GUIの実装状態

3タブはProviders & Routing、Credentials、Logs。ヘッダーはStart/Stop/Save Config/Save & Restart/Open Config...とPID/Port/Embedded/Config Path/Config Stateを表示する。

- **GUI初期設定完結とCRUD（H-04解消済み）**:
  - Gateway Settings行でListen Port（1024～65535）、`Require Client Auth`、`Enable CORS`、`Allowed Origins (CSV)`、`Enable Embedded Engine`、`Embedding GGUF` / `Reranker GGUF`パス（`Browse...`ファイル選択ダイアログ付）を編集し、`Apply Settings`（またはSave時自動同期）で反映できる。
  - Configured Providersセクションで`+ OpenRouter Preset`、`+ LM Studio Preset`ワンクリック追加、および任意Provider（ID、Base URL、Auth Mode `bearer`/`none`、Key Vendor/Slot、Model Discovery、Allowlist CSV）の`Add / Update Provider`・`Remove Provider`（Alias参照中は削除拒否）を行える。Bearerプロバイダー追加時は即座にCredentialsタブへ対象キー行が追加される。
  - Configured Model AliasesセクションでAlias Name、Target Provider、Target Model、Input Prefixの`Add / Update Alias`・`Remove Alias`を行える。
  - `Open Config...`ボタンで設定ファイル（既存または新規作成先）を切り替えられる。
- **設定状態の明示と未保存保護（H-04解消済み）**:
  - `current_config_`（編集中）、`saved_config_`（ディスク保存済み）、`applied_config_`（稼働中Gatewayに適用済み）、`config_exists_on_disk_`を保持し、ヘッダーの`Config State`とウィンドウタイトルに`New File (Not Saved)`、`Edited (Unsaved)`、`Saved on Disk (Restart Required)`、`Saved & Applied (Running)`、`Legacy Format (Saves as v2.0)`を区別表示する。
  - `Start`押下時に設定ファイル未作成または未保存変更がある場合は保存確認ダイアログを表示し、保存後に起動する。`Open Config...`切替時およびウィンドウ終了時（`wxEVT_CLOSE_WINDOW`）にも未保存変更の保存・破棄・キャンセル確認を行う。
  - 旧形式（v1.0互換）設定の保存確認（`confirm_config_format_change`）では、上書き保存（`Yes`）・別名保存（`No` = `Save Copy As...`）・キャンセルを選択でき、旧原本を壊さずにv2.0形式コピーへ移行できる。
- healthとログは500msタイマー。health通信やログI/OはUIスレッド内で実行される（H-10残課題）。
- HealthClient::check_healthは`status == "ok"`、`pid > 0`（整数）、`version`（非空文字列）、`embedded`（`disabled`/`loading`/`ready`/`error`のいずれか）を厳密検証し、`{}`等の別プロセス応答を誤認しない。MainFrameは`embedded == "error"`のときバッジを`[ Degraded ]`（Amber）、ステータスを`Healthy (Embedded Error)`として区別表示する（H-03/H-05解消済み）。
- 再起動はMainFrameが所有する`restart_worker_`スレッドで実行し、設定とパスの値snapshotを渡す。操作中は永続フラグ`is_busy_`で全操作コントロールを無効化し、500msの`refresh_status`によるボタン再有効化を抑止する。`~MainFrame()`では`ui_alive_`をfalseにした上で`process_controller_.request_cancel()`と`restart_worker_.join()`を行い、破棄後の`CallAfter`実行とダングリングアクセスを防止する。
- ProcessController::start_gatewayは匿名パイプで子プロセスのstderrを捕捉し、起動直後（100ms）またはhealth待機中にプロセスが異常終了した場合、終了コード・設定パス・stderr出力を含む診断メッセージをGUIへ返す（H-05解消済み）。
- 固定logging.fileの場合、GUIはGateway EXEのディレクトリを基準に同じログを読む。別cwdで直接起動したGatewayへの再アタッチでは基準が一致するとは限らない。

## 10. 保存・起動・停止・再起動

ProcessControllerの再起動段階はValidating→Saving→Stopping→WaitingPortRelease→Starting→WaitingHealth。Validating段階では`parse_and_validate_config`による事前検証を実行し、不正時は既存プロセスを停止せず即中断する。保存後、旧稼働ポート（`prev_port`）と旧Admin資格情報名を使って停止を行い、`WaitingPortRelease`では`prev_port`と新設定ポート（`new_config.listen.port`）の両方の解放を確認してから新ポートで起動・health確認を行う（H-04解消済み）。ポート解放は100ms間隔で約5秒まで、healthは200ms間隔、無効時10秒/有効時60秒まで待つ（プロセスが早期終了した場合はstderrと終了コードを添えて即座に失敗を返す）。すべての待機ループは`request_cancel()`による協調的キャンセルに対応する。

有効時の完了条件は`embedded`が`ready`、`error`、または`disabled`（有効だがモデル未設定の場合）であり、モデル未指定でも60秒タイムアウトしない（H-03解消済み）。

停止（`stop_gateway`）は稼働中設定の`admin_credential_name`でAdmin HTTP `POST /admin/shutdown`を送り、HTTP 2xx応答を確認する。その後ハンドル待機5秒→EXEパス一致時の`TerminateProcess`および終了待機を実行し、ハンドル未保持でHTTP送信のみ成功した場合はポート解放を確認する。停止または強制終了が確認できない場合は`false`とエラー理由を返し、MainFrameのStopボタン操作でも失敗ダイアログを表示する（H-07解消済み）。再アタッチはhealthのPIDとプロセス列挙・EXE一致を使うが、完全な作成時刻照合はない。

ShutdownCoordinatorは所有する`shutdown_thread_`でアクティブ要求のdrainを最大5秒待機した後にshutdown action（既定は50ms猶予の後`ExitProcess(0)`、テスト時はカスタムコールバック注入可）を実行し、デストラクタで安全にキャンセル・joinする。

## 11. ログ

Loggerはmutexで出力を直列化し、1行ごとに`path_from_utf8`でファイルを開いて追記する。時刻・level・client_ip・method・endpoint・status・latency_ms（実測値）・win32_errorと任意messageを出す。

日別モードはllm-gateway-YYYYMMDD.logを使用し、既存候補ファイルを名前降順に並べ7件残す処理を起動/日付切替時に行う。新規当日ファイル作成前に整理するため、常時厳密7件保証ではない。固定fileモードはローテーション・削除をしない。

ログディレクトリ作成失敗やファイルオープン・書込失敗時は`write_error_count()`と`last_write_error()`に記録し、初回失敗時にstderrへ警告を出力する（H-05解消済み）。

GUIは初回256KiBのTailを読み、500msごとに増分を追う。ファイル切替に対応するが、同名ファイルの縮小、UTF-8/行の分割、増分量上限は追加検証が必要。

## 12. ビルドと依存

| 依存 | 固定版 |
|---|---|
| nlohmann/json | 3.11.3 |
| cpp-httplib | 0.18.3 |
| curl | 8.10.1 |
| llama.cpp | b4200 |
| wxWidgets | 3.2.6 |

Dependencies.cmakeでURLとSHA-256を指定する。新規configureは依存取得のネットワークアクセスを伴う。既存build/_depsの第三者ソースはプロジェクトの修正対象にしない。

Release /MT、Debug /MTd。/W4、/permissive-、/EHsc、/utf-8、Release最適化・CFG等を使用する。curlはSChannel、llamaはCPUのみでOpenMPを無効化。VerifyImportsはdumpbinのDEPENDENTS/IMPORTSを使う。

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target verify_imports
~~~

binへのEXE反映は自動ではない。動作中でないことを確認して、検証済みbuild/Releaseの両EXEをコピーする。設定ファイルを上書きしない。実設定・資格情報は配布物へ追加しない。

Git/CI/クリーンVM配布試験は未整備。現在のDLL検査成功をクリーンOS動作保証と同一視しない。

## 13. 試験と既知の未達

既存6件はassertを使用するため、CMakeでReleaseにも/UNDEBUGを付けている。追加legacy_compat_testは例外による常時有効な検査を使用する。

| テスト | 主な範囲 |
|---|---|
| config_test | 設定の基本検証、H-12全数値・文字列・bool・配列の厳密型検証、H-03/H-14モデル存在検査分離と日本語・空白・絵文字パス、H-08 Rerank厳密弱順序（全6順列・同点・NaN）と特殊トークン整形（`format_rerank_token_ids`および実BGE語彙GGUF `format_rerank_tokens`） |
| router_test | Alias/Provider/Allowlist |
| auth_test | Secret、乱数トークン、Authenticator（Client/Admin Bearer検証・異常系） |
| request_guard_test | Host/CORS等 |
| health_test | ローカルhealth、H-01 SSE中継、H-02要求カウンター・graceful shutdown・cancel、Phase 2（H-03欠落GGUF時の外部経路継続と503分離、H-05 health厳密検証・latency計測・Logger失敗検知・ModelCatalog失敗ログ・起動失敗stderr捕捉、H-06事前検証・原子的保存、H-07カスタムAdmin名停止、H-11クリップボード/WinCred結果、H-12 API型・範囲検証、H-14 Unicode CLI引数起動）、Phase 3（H-08 `/v1/embeddings`・`/v1/rerank`のA→B→A一貫性・文書順序不変性・トークン上限400・空文書400・抽出失敗/NaNの503・並列6スレッド要求、H-04初回未作成設定からのプリセット/Provider/Alias CRUD・参照整合性保護・未保存判定・旧形式別名保存による原本保護・稼働ポート追跡） |
| security_test | ローカルHTTP防御（Host/Origin拒否と要求カウンター不変性） |
| legacy_compat_test | 旧形式変換、異常値、現行形式へのroundtrip、無効モデル存在検査、CORS、cache、模擬上流通常POST・入れ子stream無視 |

直近の実行は7/7成功、失敗0（約2.77秒）。両bin EXEのVerifyImports成功、build/ReleaseとのSHA-256一致、bin設定不変を確認済み。実Providerには接続していない。

未達・未検証: 実フルサイズGGUF推論品質評価、実OpenRouter/LM Studio接続試験、クリーンVM、長時間運用・UIタイマー非同期化・ログローテーション詳細（H-09/H-10）。詳しい次作業と受入条件は引き継ぎ書を参照する。

## 14. 改訂履歴

| 版 | 日付 | 内容 |
|---|---|---|
| 2.0 | 従来版 | 実装の目標仕様 |
| 2.1 | 2026-10-13 | 現実装に合わせた再整理。旧形式アダプター、追加設定、保存確認、試験結果、未達・既知問題を明示。同日Phase 1（H-01/H-02/H-13）、Phase 2（H-03/H-05/H-06/H-07/H-11/H-12/H-13/H-14）、Phase 3（H-04/H-08/H-13）の実装内容を反映 |
