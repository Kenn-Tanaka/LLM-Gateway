# 設定ファイルの互換読込

`schema_version: "2.0"` の現行設定に加えて、バージョン指定のない旧Gateway設定を読み込めます。読込時にファイルは変更しません。個人のテスト設定はプロジェクトへコピー・同梱せず、実行時にパスを指定してください。

```powershell
& .\bin\MaintenanceTool.exe --config 'C:\path\to\gateway.json'
# GUIを使用しない場合
& .\bin\llm-gateway.exe --config 'C:\path\to\gateway.json'
```

GUIで「Save Config」または「Save & Restart」を実行すると、形式変更の確認後、指定ファイルへ正規化された2.0形式を保存します。旧Gatewayとのファイル共有を続ける場合は、元ファイルを保存操作の対象にせず、作業用コピーを指定してください。

## 変換と動作

| 旧設定 | 内部設定・動作 |
|---|---|
| `listen.address` | `listen.host`。ループバックIPv4のみ許可 |
| `proxy.max_request_body_mb` | MiBからバイトへ変換し、HTTP受信上限にも反映 |
| `proxy.connect_timeout_sec` | `timeouts.upstream_connect_ms` |
| `proxy.read_timeout_sec` | `timeouts.upstream_read_ms`。現在のcurl中継では総転送時間上限 |
| `proxy.write_timeout_sec` | `timeouts.upstream_write_ms`。上流アップロードの進捗停止期限 |
| `model_cache.enabled / ttl_sec` | モデル探索キャッシュの有効状態・TTL。成功した空一覧もキャッシュ |
| トップレベル `cors` | `security.cors`へ移動。methods、headers、max-ageも反映 |
| `allowlist: [{"type":"glob","pattern":"provider/*"}]` | 現行Allowlistへ変換 |
| `logging.level: "info"` | `INFO`に変換 |
| `logging.file` | 指定した固定ファイルへ追記。日別ローテーションは適用しない |

旧設定には組み込みエンジンを要求せず、`embedded_engine.enabled=false`で開始します。設定形式にかかわらず、組み込みが無効ならモデルファイルの存在検査を行いません。

旧ProviderのAPIルートURLには `/v1` を補完します。例：`https://openrouter.ai/api` は内部で `https://openrouter.ai/api/v1` として使用します。末尾が既に `/v1` のLM Studio等には重複追加しません。接続先のホスト、ポート、認証方式は変更しません。

`logging.file`の相対パスは旧実装と同じくGatewayの作業ディレクトリ基準です。管理GUIから起動した場合はGateway EXEのディレクトリになります。GUIログタブもその場所を参照します。

## 対応範囲

- 現行形式と旧形式の混在や未知フィールドは拒否します。
- 旧Allowlistは、完全一致、`*`、末尾`/*`の前方一致に対応します。任意位置のglobや正規表現は対応しません。
- CORSの許可メソッド指定はエンドポイントを新設するものではありません。Gatewayで未実装のメソッド・パスは従来どおり未対応です。
- CORS無効時はOrigin付き要求を拒否し、OriginなしのネイティブClientは利用できます。
- この変更は旧設定の読込互換です。旧Gatewayの全API・ストリーミング動作の完全互換を保証するものではありません。

## 検証

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
# 個人用ファイルは読込だけ検証。実Providerへは接続しない。
& .\build\Release\legacy_compat_test.exe 'C:\path\to\gateway.json'
```

互換テストは配布用ではない合成設定とローカル模擬Providerを使用します。実際のクラウドAPIキー、Tailscale内ホスト、モデルのダウンロードを必要としません。
