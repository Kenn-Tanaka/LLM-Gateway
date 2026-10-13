# LLM Gateway

Windows 向けの OpenAI 互換ローカルゲートウェイ（`llm-gateway.exe`）と、設定・起動・資格情報管理用 GUI（`MaintenanceTool.exe`）です。外部 Provider（OpenRouter、LM Studio、Ollama 等）への中継と、任意で GGUF による組み込み Embedding / Rerank を提供します。

## 初回起動（GUI のみで完結）

1. `MaintenanceTool.exe` を起動し、`Open Config...` で設定ファイルを選びます（無ければ新規作成の状態で始まります）。配布物の `config/llm-gateway.external-only.example.json` をコピーして `llm-gateway.json` として使うこともできます。
2. Providers 欄で `Preset: OpenRouter` 等を選び、`Set Selected Provider Key...` で API キーを登録します（Windows 資格情報マネージャーに保存。設定ファイルには書かれません）。
3. Credentials 欄でクライアント用／管理用トークンを生成します（コピー後 30 秒でクリップボードを消去しますが、GUI を開いたままにする必要があります）。
4. `Save Config` で保存し、`Start` で起動します。状態バナーが「未保存／再起動が必要」を示します。

CLI での起動: `llm-gateway.exe --config <path>`。ヘルス確認: `http://127.0.0.1:8765/health`。

## 診断

- ログ: 既定は `%LOCALAPPDATA%\LLM-Gateway\logs`。GUI のログ欄は最新 5000 行のみ表示します（ファイルには全て残ります）。
- 組み込みモデルが無い／壊れている場合でも外部 Provider は動作し、組み込みモデル宛のみ `503` になります（Health の `embedded` が `error`）。
- 設定の旧 v1.0 形式は読込時に変換され、保存時に上書き／別名保存を選べます（`CONFIG_COMPATIBILITY.md` 参照）。

## 資格情報の移行

API キーとトークンは Windows 資格情報マネージャー（ユーザー単位）に保存され、別 PC・別ユーザーへはコピーされません。新しい環境では GUI から再登録してください。

## 組み込み推論（任意）

GGUF モデル（例: bge-m3、bge-reranker-v2-m3）は同梱されません。各自で入手し、GUI の `Browse...` でパスを指定します。モデルごとのライセンスに従ってください。

## ビルドと配布

- ビルド: CMake（Visual Studio 2022, Release）。依存は版とSHA-256を固定して取得します。
- 配布物の作成: `powershell -ExecutionPolicy Bypass -File scripts/package.ps1`（許可リスト方式。実設定・ログ・build・引き継ぎ書は含みません。`dist/` に ZIP と SHA-256 を出力）。

## ライセンス

本プロジェクトは MIT License（Copyright (c) 2026 HAIA Project、`LICENSE` 参照）で配布します。サードパーティの表示は `NOTICE.md` と `THIRD_PARTY_LICENSES/` を参照してください。
