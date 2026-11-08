import http.client
import json
import sys
import threading
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_bridge import make_server


class ContractTest(unittest.TestCase):
    def setUp(self):
        self.token = 'local-test-only-token-12345678'
        self.server = make_server(self.token, 0)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def post(self, text='今日やることは？', rid='r1', token=None, confirmed=True):
        c = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=5)
        body = json.dumps(dict(text=text, request_id=rid, device_id='core2-01', confirmed=confirmed))
        c.request('POST', '/v1/voice/commands', body,
                  {'Authorization': 'Bearer ' + (self.token if token is None else token),
                   'Idempotency-Key': rid, 'Content-Type': 'application/json'})
        response = c.getresponse()
        data = response.read()
        self.assertEqual(int(response.getheader('Content-Length')), len(data))
        c.close()
        return response.status, json.loads(data)

    def test_all_v1_intents(self):
        for i, text in enumerate(['今日やることは？', '天気は？', '買い物メモに牛乳',
                                  '思いつきを記録して', '最新ニュースを一件', 'イオン感謝デーはいつ？']):
            status, data = self.post(text, str(i))
            self.assertEqual(status, 200)
            self.assertTrue(data['ok'])
            self.assertTrue(data['reply'].startswith('テスト:'))

    def test_auth(self):
        self.assertEqual(self.post(token='wrong')[0], 401)

    def test_unsupported(self):
        self.assertEqual(self.post('ファイルを削除して')[0], 403)

    def test_confirmation_required(self):
        self.assertEqual(self.post(confirmed=False)[0], 400)

    def test_idempotency(self):
        first = self.post()
        self.assertEqual(self.post(), first)
        self.assertEqual(len(self.server.seen), 1)
        self.assertEqual(self.post('天気は？')[0], 409)

    def test_input_limit(self):
        self.assertEqual(self.post('あ' * 1025)[0], 400)


if __name__ == '__main__':
    unittest.main()
