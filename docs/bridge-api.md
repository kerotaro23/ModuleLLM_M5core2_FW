# M5 Voice Bridge V1 契約

これはCore2が接続する**Pi側にこれから実装するAPIの仕様**です。OpenClaw固有のAPIを推測して実装したものではありません。

- Core2 → `http://<PiのRFC1918 IPv4>:8765/v1/voice/commands`
- `Authorization: Bearer <端末専用トークン>`
- `Content-Type: application/json`
- `Idempotency-Key: <request_id>`
- リダイレクト不可。成功はHTTP 200、Content-Length必須、本文4096 bytes以下、replyはUTF-8で1024 bytes以下。

要求例:

```json
{"request_id":"core2-01-8ac012fe-1","device_id":"core2-01","text":"今日やることは？","confirmed":true}
```

成功例:

```json
{"request_id":"core2-01-8ac012fe-1","ok":true,"intent":"todo.read","reply":"今日は買い物があります。"}
```

| 許可するintent | 用途 |
|---|---|
| `todo.read` | 今日のTODO取得 |
| `weather.read` | 設定済み地域の天気 |
| `shopping.append` | 買い物メモへの追記 |
| `memo.append` | 思いつきの記録 |
| `news.read` | 最新ニュース1件 |
| `aeon.thanks_day.read` | 感謝デー日程の取得 |

Piは**実行前**に認証・device_idへの権限・命令許可リスト・入力サイズ・確認フラグを検証してください。Core2の応答intent検証は、既に実行された命令を取り消せません。任意プロンプトをそのまま汎用Gatewayに渡してツール実行を許可するAPIにはしないでください。許可されない／曖昧な命令は403/422を返します。認証失敗は401、同じキーで異なる内容は409。秘密情報や内部スタックトレースを返答文に含めないでください。

記録系はトークンに紐づく端末とrequest_idで永続的に重複排除し、同じ内容の再要求には同じ結果を返してください。メモ書き込みと重複排除記録は一貫して確定させる必要があります。端末は自動再送しませんが、通信切断で実行結果が不明になる場合があります。異なるrequest_idの同文を自動で同一操作と見なす設計にはしていません。

HTTPはLAN内でも暗号化されません。V1は信頼できる隔離LANを前提にし、外部公開やポート転送はしないでください。Gatewayは引き続きPiのlocalhostだけで待ち受けます。トークンは端末ごとに失効可能にし、Wi-FiやLANを信頼できない用途では次版でHTTPSを実装してください。

Piのログは時刻・認識文・命令・結果に限定し、トークンや音声データは記録しません。ファームウェアは音声をHTTP送信せず、認識文と返答をRAM上に保持します。

## ローカルモック

`tools/mock_bridge.py` は127.0.0.1だけで待ち受け、認証・許可intent・重複要求の契約をテストする固定応答サーバーです。実際のデータ取得、保存、Gateway呼び出しは行いません。認識は限定語彙の前方一致のみであり、本番の許可判定として流用しないでください。

```bash
# テスト専用値。実機トークンは不要。
MOCK_DEVICE_TOKEN=local-test-only-token-12345678 python3 tools/mock_bridge.py
```

クラウドから実機への接続用サービスではありません。自動テストは一時ポートで起動し、終了時に停止します。
