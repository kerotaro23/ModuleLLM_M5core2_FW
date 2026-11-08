#!/usr/bin/env python3
"""Local contract fixture ONLY. No Gateway calls, real data, or persistent writes."""
import hmac
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer


def classify(text):
    rules = [
        ('今日やること', 'todo.read', 'テスト: 今日の予定はありません。'),
        ('天気', 'weather.read', 'テスト: 天気の取得は未接続です。'),
        ('買い物メモに', 'shopping.append', 'テスト: 買い物メモの受付を模擬しました。保存はしていません。'),
        ('思いつきを記録', 'memo.append', 'テスト: メモの受付を模擬しました。保存はしていません。'),
        ('最新ニュース', 'news.read', 'テスト: ニュースの取得は未接続です。'),
        ('イオン感謝デー', 'aeon.thanks_day.read', 'テスト: 日程の取得は未接続です。'),
    ]
    # Deliberately small fixture vocabulary, not a production intent classifier.
    for trigger, intent, reply in rules:
        if text.startswith(trigger):
            return intent, reply
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass  # No transcript or token logging.

    def respond(self, code, body):
        raw = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):
        if self.path != '/v1/voice/commands':
            return self.respond(404, {'ok': False})
        supplied = self.headers.get('Authorization', '')
        if not hmac.compare_digest(supplied.encode(), ('Bearer ' + self.server.token).encode()):
            return self.respond(401, {'ok': False})
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 4096:
                raise ValueError()
            self.connection.settimeout(3)
            data = json.loads(self.rfile.read(length))
            rid = data['request_id']
            if not isinstance(rid, str) or not 1 <= len(rid) <= 64:
                raise ValueError()
            if self.headers.get('Idempotency-Key') != rid or data.get('confirmed') is not True:
                raise ValueError()
            if data.get('device_id') != 'core2-01':
                raise ValueError()
            text = data['text']
            if not isinstance(text, str) or not 0 < len(text.encode()) <= 1024:
                raise ValueError()
        except (KeyError, TypeError, ValueError, TimeoutError):
            return self.respond(400, {'ok': False})
        if rid in self.server.seen:
            old_text, result = self.server.seen[rid]
            return self.respond(200, result) if old_text == text else self.respond(409, {'ok': False})
        match = classify(text)
        if not match:
            return self.respond(403, {'ok': False, 'error': 'unsupported_intent'})
        if len(self.server.seen) >= 128:
            return self.respond(503, {'ok': False, 'error': 'fixture_capacity'})
        result = {'request_id': rid, 'ok': True, 'intent': match[0], 'reply': match[1]}
        self.server.seen[rid] = (text, result)
        self.respond(200, result)


def make_server(token, port=8765):
    if len(token) < 24:
        raise ValueError('MOCK_DEVICE_TOKEN must contain at least 24 characters')
    server = HTTPServer(('127.0.0.1', port), Handler)
    server.token, server.seen = token, {}
    return server


if __name__ == '__main__':
    make_server(os.environ['MOCK_DEVICE_TOKEN']).serve_forever()
