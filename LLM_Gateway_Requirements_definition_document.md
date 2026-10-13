# LLM Gateway ＆ MaintenanceTool 全体要件定義書（第2.1版）

- 更新日: 2026-10-13
- 対象: 開発中のWindows用LLM Gatewayと管理GUI
- 文書版2.1は要件の更新番号。設定のschema_versionは引き続き「2.0」、実行ファイルの表示バージョンは2.0.0。
- 対応文書: [実装仕様書](LLM_GATEWAY_MAINTENANCE_TOOL_IMPLEMENTATION_SPEC_2.0.md)、[設定互換性](CONFIG_COMPATIBILITY.md)、[作業引き継ぎ書](WORK_HANDOVER.md)

## 1. 目的・利用者・方針

ローカルLLMとクラウドLLMを使い分ける個人ユーザーに、共通のHTTP APIと管理GUIを提供する。ローカルのClientからGatewayへ接続し、設定したProviderへ中継する。組み込みCPU推論はEmbedding/Rerank用とし、チャット生成は外部Providerを使用する。

依頼者が確定した方針は以下のとおり。

1. 初回設定はGUIだけで完結することが望ましい。これを改善目標とし、現状で達成済みとは扱わない。
2. 組み込みモデルが利用不能でもクラウド経路を継続利用できること。
3. 当面は個人用Portable配布とし、安定性を確認してから一般公開を検討する。
4. 既存のバージョン指定なしgateway.jsonを編集・移行せず読み込めること。
5. 実設定・資格情報を配布物へ含めないこと。
6. UIの一貫性・理解しやすさと、エラー原因を調べやすいことを優先する。
7. 現在Gitリポジトリはない。Git導入・commit・公開は今回の文書改訂に含まない。

本書の「要件」は到達目標であり、実装済みの宣言ではない。実装状態と差分は第11章および実装仕様書に明示する。

## 2. 用語と対象環境

| 用語 | 意味 |
|---|---|
| Gateway | HTTPバックエンド。llm-gateway.exe |
| MaintenanceTool | 設定・資格情報・プロセス・ログの管理GUI。MaintenanceTool.exe |
| Client | Gatewayを呼び出すWebアプリまたはネイティブツール |
| Provider | OpenRouter、LM Studio、Ollama、embeddedなどの経路 |
| Alias | modelsに定義する利用者向けモデル名 |
| Allowlist | 解決後のProvider/モデルへのアクセス許可パターン |
| 旧形式 | schema_versionを持たない既存Gatewayの設定 |
| 現行形式 | schema_versionが文字列「2.0」の設定 |

- Windows 10/11、x64。従来の最低OS目標はWindows 10 1809。クリーンVMでの最低OS検証は未完了。
- 現ビルド・起動処理はAVX2を要求する。クラウド専用でも検査を通る必要がある。
- C++20、MSVC、Releaseは静的ランタイム /MT。Windows標準DLL以外への動的依存を避ける。
- Gatewayの待受はIPv4 127.0.0.1のみ。上流ProviderはLAN/Tailscale/インターネット上でもよい。
- LM StudioはTailscale経由のLAN内PCへ接続する。依頼者の既存設定の既定モデル運用を尊重する。
- 実クラウド試験のモデルは依頼者推奨のGemma4-31Bを優先する。正確なOpenRouterモデルIDは実接続時に確認し、固定値を推測しない。

Linux/macOS、Windowsサービス化、GPU推論、異ユーザー間共有、設定ホットリロードは対象外。Portableはインストーラー不要という意味であり、WinCredやLOCALAPPDATAを一切使わない完全自己完結型ではない。

## 3. 脅威モデルと安全性

ローカル単独利用を基本とする。Gateway自体をLANやインターネットへ公開しない。同一Windowsユーザー権限を奪われた環境での秘密保護は保証しない。

| ID | 要件 |
|---|---|
| SEC-01 | Provider APIキー・Client/Adminトークンを設定JSONに保存しない。Windows Credential Managerを使用する |
| SEC-02 | Provider資格情報のみ、明示設定時に環境変数fallbackを許可する。既定は無効 |
| SEC-03 | Hostを127.0.0.1:portまたはlocalhost:portと照合する。Originは許可一覧との完全一致とする |
| SEC-04 | ClientのAuthorization/Cookie/Hostを上流へそのまま転送しない。Provider用Bearerを別に付与する |
| SEC-05 | TLS検証を有効にし、上流redirectを自動追従しない |
| SEC-06 | ログにAPIキー、トークン、Authorization値、要求・応答本文を記録しない |
| SEC-07 | Client認証は現行設定の既定で有効。旧設定のclient_auth=falseは互換性のため尊重する。Admin認証は独立して必要 |
| SEC-08 | 停止操作は対象EXEの一致を確認し、無関係なプロセスを終了しない |

資格情報の標準ターゲット名はCloudLLM/<vendor>|<slot>。Client/Adminはそれぞれllm_gateway|default、llm_gateway|adminを既定とし、設定値を全利用箇所で一貫して使用すること。

## 4. Gateway機能要件

| ID | API / 機能 | 要件 |
|---|---|---|
| GW-01 | GET /health | 認証不要。status、pid、version、embedded状態を返す。秘密・設定全文を返さない |
| GW-02 | GET /v1/models | Alias、探索した外部モデル、準備済み組み込みモデルを統合。探索cache設定を反映する |
| GW-03 | POST /v1/chat/completions | 外部Providerへ中継。通常応答とSSEを扱い、上流status・エラーを正しく伝える |
| GW-04 | POST /v1/embeddings | OpenAI形式の文字列/文字列配列inputを外部または組み込みへ送る |
| GW-05 | POST /v1/rerank | 組み込み専用。query/documents、top_n、return_documentsを扱う |
| GW-06 | POST /admin/shutdown | Admin認証後、新規処理受付を止め、処理終了を最大5秒待つ |
| GW-07 | Routing | Alias解決後にAllowlistを照合。直接指定はprovider/model形式 |
| GW-08 | 制限 | 本文上限をHTTP受信時にも反映し、超過は413。接続・転送・アップロード停止期限を設定可能にする |

/healthとOPTIONSを除く利用APIはclient_auth設定に従う。Allowlistは完全一致、単独「*」、末尾「/*」の前方一致を扱い、任意globや正規表現は対象外とする。大文字・小文字は区別する。

Gatewayが生成するエラーはerror.message/type/codeを持つJSONを基本とする。上流のエラー本文は通常中継ではそのまま返るため、「すべての上流エラーもGateway独自形式に変換する」とは規定しない。SSE送信開始後の失敗はHTTPステータスを変更できないため、切断・ログ・Clientへの通知の扱いを明確にすること。

## 5. 組み込みモデルと障害分離

| ID | 要件 |
|---|---|
| EMB-01 | 設定enabled=false、または--no-embeddedで組み込みを無効にする。旧形式は既定で無効 |
| EMB-02 | 無効時はモデルファイルの存在検査・ロードを要求しない |
| EMB-03 | 有効時も非同期ロードとし、クラウド要求をロード完了待ちにしない |
| EMB-04 | ファイル欠落・ロード失敗などモデル固有の障害でGateway全体を停止せず、外部経路を継続する |
| EMB-05 | disabled/loading/ready/errorを表示する。クラウド稼働状態と組み込み状態を区別する |
| EMB-06 | Embedding/Rerankに独立した排他を持ち、同種の同時推論を直列化する |

構文不正、待受先不正などGateway全体の設定不正は起動エラーとしてよい。EMB-04を理由に設定全体の検証を無効化してはならない。Phase 2（H-03）により、既定の起動時設定読込ではGGUFモデルファイルの存在不在でGateway全体を停止せず、`EmbeddedEngine::start_async_load`で個別モデルを`LoadState::Error`へ遷移させて外部クラウド経路を継続稼働する実装に更新済みである。

組み込み無効時のlisten開始500ms以内は従来からの性能目標であり、基準PC・測定条件を定めた実測は未実施。

## 6. 設定互換性・保存

| ID | 要件 |
|---|---|
| CFG-01 | 旧形式と現行形式を自動判別し、読込だけでは原本を書き換えない |
| CFG-02 | 旧listen.address、proxy、トップレベルcors、object形式Allowlist、logging.fileを変換する |
| CFG-03 | 旧APIルートURLに必要な/v1を補い、既に/v1のURLへ重複追加しない |
| CFG-04 | 既存credentials、Provider ID、認証方式、接続先ホスト・ポートを維持する |
| CFG-05 | 不明な項目・未対応ルールを黙って捨てない。受理可能範囲を明示する |
| CFG-06 | GUI保存は現行2.0形式へ正規化する。旧形式の上書き前は確認し、キャンセル可能にする |
| CFG-07 | 一時ファイル、FlushFileBuffers、MoveFileExWで保存する。各失敗を検出して原本を保護する |

旧形式のままの再保存・旧Gatewayへの逆変換は現在の対応範囲外。旧Gatewayと共有するファイルはコピーを保存対象にする。

本文容量の旧単位はMiB（1024×1024 bytes）、旧timeoutは秒からmsへ変換する。現在のread timeoutはcurlの総転送時間上限であり、旧実装の読み取り無通信時間とは意味が異なる。この差を隠して完全互換とは表現しない。対応表・既定値は実装仕様書および設定互換性文書を参照する。

## 7. MaintenanceTool要件

| ID | 要件 | 現状 |
|---|---|---|
| UI-01 | 初回起動からProvider・Alias・接続先・資格情報をGUIで設定して開始できる | 実装済み（H-04によりListen Port/Client Auth/CORS/GGUFパス編集、Provider/Alias CRUD、OpenRouter/LM Studioプリセット追加、Bearerプロバイダー追加時の資格情報行自動生成を実装・検証済み） |
| UI-02 | 設定ファイルをGUIで選択し、使用パスを確認できる | 実装済み（H-04により`Open Config...`ダイアログ、GGUFモデル`Browse...`ダイアログ、およびUnicode/絵文字対応パス表示を実装済み） |
| UI-03 | Start / Stop / Save Config / Save & Restartを提供する | 実装済み（H-02再起動busy保護、H-03組み込みdisabled/error時の完了・劣化表示、H-04未保存時Start確認・別名保存・ポート変更跨ぎ再起動、H-06保存前検証とFlush検査、H-07設定Admin名利用と停止結果検証を反映済み） |
| UI-04 | 稼働中設定、編集中設定、未保存状態を区別する | 実装済み（H-04により`Edited (Unsaved)`、`Saved on Disk (Restart Required)`、`Saved & Applied (Running)`、`Legacy Format`をヘッダーとタイトルバーで明示し、終了・切替・起動時の未保存保護を実装済み） |
| UI-05 | Providerキーの登録・削除、Client/Adminトークン生成・コピーを提供する | 実装済み（H-11によりWinCred保存成否・クリップボードコピー成否・削除成否を個別に判定・通知） |
| UI-06 | ログを閲覧し、失敗理由と調査先を知ることができる | Tailあり。H-05により起動直後終了時のstderr/終了コード/設定パス捕捉、フォールバックエラーログ、Provider別探索警告ログを実装済み |
| UI-07 | 長い処理でUIを止めず、終了時にワーカーの破棄後アクセスを防ぐ | 再起動ワーカーの所有・cancel/join・破棄後CallAfter抑止・設定snapshot・busy排他を実装済み。UIスレッドのhealth/ログ同期I/Oは残課題 |
| UI-08 | 用語、状態、通知、入力単位を統一し、色だけに依存しない | 一部実装。現在UIは英語中心 |

Phase 3（H-04）により、設定ファイルが存在しない初回起動状態からJSON手編集なしでProviderプリセット・Alias・資格情報・ポート・組み込み設定をGUI完結で構成・保存・起動できるようになった。UI日本語化は未確定であり、今回の日本語文書化と同義ではない。

## 8. ライフサイクル・データ保護

- GatewayとGUIの寿命は独立。GUIを閉じてもGatewayを停止しない。
- セッション内はNamed Mutexで多重起動を防ぐ。別セッションのポート競合はbindエラーで知らせる。
- 保存→停止→ポート解放→起動→health確認を段階表示する。
- 起動直後に終了した場合は終了コード、使用設定パス、調査可能なエラーを表示する。
- 組み込みerror/disabledでも外部経路が利用可能なら、その状態を起動全体の失敗と混同しない。
- 再起動失敗時の復旧・未保存保護・元設定のバックアップは改善対象。自動復旧が実装済みとは扱わない。
- 保存時のFlush失敗、権限不足、容量不足を成功として扱わない。

## 9. ログ・診断

既定はLOCALAPPDATA/LLM-Gateway/logs配下の日別ログ。logging.fileがある場合は固定ファイルへの追記とし、相対パスはGatewayの作業ディレクトリ基準。GUIから起動するとGateway EXEのディレクトリになる。固定ファイルモードに自動ローテーションはない。

必要な診断情報は時刻、段階、Provider識別子、HTTP status、処理時間（`latency_ms`）、OS/curlエラー分類とする。秘密・本文を記録せずに、設定不正、資格情報不足、上流拒否、接続失敗、モデル障害を区別できること。Phase 2（H-05）により、HTTP要求ごとの`latency_ms`計測・記録、Provider別モデル探索失敗の警告ログ、Logger書込失敗検知（`write_error_count` / `last_write_error` と初回stderr出力）、および起動直後終了時のstderrパイプ捕捉を実装済みである。

## 10. ビルド・Portable配布

- CMake 3.25以上、Visual Studio 2022 C++ x64 toolchain。依存ソースは版とSHA-256で固定する。
- Release /MT、curl/SChannel、llama CPU、OpenMP/GPU無効、wxWidgets静的リンク。
- 両EXEにVerifyImportsを実行し、許可したWindows DLL以外への依存を検出する。
- 配布候補は両EXE、秘密を含まない設定例、操作説明、第三者ライセンス。モデル本体はサイズと再配布条件を別途確認する。
- build、依存ソース、テストEXE、実設定、ログ、個人用引き継ぎ情報は配布しない。
- APIキーはWinCredに属するため、フォルダを別PCへ移しても移行されない。別PCで再登録する。
- 一般公開前にWindows 10/11クリーン環境、ライセンス、パッケージ内容、長時間利用を検証する。自動更新・アンインストーラーは現段階の対象外。

## 11. 受入基準と現在の状態

| 要件群 | 受入確認 | 2026-10-13時点 |
|---|---|---|
| CFG-01～05、EMB-02 | 旧実設定の無変更読込、合成設定変換、URL中継、異常値・型不正拒否、Unicode/絵文字パス読込 | 自動試験済み（H-12/H-14修正済み） |
| CFG-06 | 旧形式保存確認・別名保存（Save Copy As）・キャンセル | 上書き保存・別名保存・キャンセルの確認処理およびConfigStore別名保存で旧原本が保持されることを自動試験済み（H-04） |
| CFG-07 | 原本保護、保存前検証、Flush/置換失敗、RAII一時ファイル削除 | 保存前検証・Flush検査・RAII削除・Unicodeパス保存を実装・自動試験済み（H-06/H-14）。OS障害注入は未実施 |
| GW-02、GW-08 | cache、本文413、CORS、固定ログ、APIフィールド厳密型検証 | ローカル模擬/HTTP試験済み（H-12修正済み） |
| GW-03 | SSE逐次到着、上流要求1回、終了、429、切断 | ローカル模擬上流で自動試験済み（H-01修正済み）。実Provider接続試験は未実施 |
| EMB-03～06 | モデル欠落・失敗時も外部POST成功、GUI劣化表示、推論順序比較・KVキャッシュ消去・長文/空入力/抽出失敗境界 | 欠落GGUF時のクラウド継続・503分離（H-03）、Rerank厳密弱順序比較・KVキャッシュ毎回クリア・特殊トークン整形・encoder/decoder分岐・抽出失敗/NaNの503化・トークン長上限400化（H-08）を実装・自動試験済み。実フルサイズGGUFモデルの品質評価は残課題 |
| UI-01～08 | GUIだけで初期設定→保存→起動→失敗診断 | UI-01～07の初期設定CRUD・プリセット・ファイル選択・状態区別・未保存保護・信頼性・診断修正を実装・ConfigStore/ProcessController統合試験済み（H-02/H-04/H-05/H-07/H-11）。対話型GUI手動通し確認とUIタイマー非同期化は残課題 |
| ライフサイクル | 処理中終了・停止・再アタッチ・再起動 | HTTP要求カウンター不変性・SSE中のshutdown待機・再起動cancel・カスタムAdmin名による停止検証・ポート変更時の新旧ポート待機を実装・自動試験済み（H-02/H-04/H-07修正済み） |
| 配布 | DLL検査、クリーンVM、個人情報除外 | DLL検査成功。公開用package検証未実施 |

残課題（Phase 4以降: H-09、H-10、SEC-L01、ARCH-L01、および実GGUF/実Provider/クリーンVM確認）は作業引き継ぎ書に整理する。

## 12. 改訂履歴

| 版 | 日付 | 内容 |
|---|---|---|
| 2.0 | 従来版 | 初期要件 |
| 2.1 | 2026-10-13 | 依頼者方針、旧設定互換、保存変換、Portableの範囲、実装差分と未検証項目を反映。同日Phase 1（H-01/H-02/H-13）、Phase 2（H-03/H-05/H-06/H-07/H-11/H-12/H-14）、Phase 3（H-04/H-08と回帰テスト）の実装状態を更新 |
