"""Host tests for the C15-2 REST mapping layer and source isolation."""

import asyncio
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "review"))
import c152_policy as p  # noqa: E402
import c152_rest_observer as rest  # noqa: E402


def run(coro):
    return asyncio.run(coro)


class ObserverMappingTests(unittest.TestCase):
    class Transport:
        def __init__(self, status, body):
            self.status = status
            self.body = body
            self.paths = []
            self.tokens = []

        async def request(self, method, path, token=None, body=None):
            self.paths.append((method, path))
            self.tokens.append(token)
            return self.status, self.body

    def test_login_uses_token(self):
        transport = self.Transport(200, {"token": "jwt"})
        obs = rest.TbRestObserver(transport, "dev", "u", "p")
        run(obs.start())
        run(obs.fetch_series(("net_mode",), 0, 1))
        self.assertEqual(transport.paths[0][0], "POST")
        self.assertEqual(transport.tokens[-1], "jwt")

    def test_status_mapping(self):
        cases = {401: "auth", 403: "forbidden", 404: "not_found",
                 429: "rate_limited", 500: "server", 502: "server",
                 418: "http_418"}
        for status, code in cases.items():
            with self.subTest(status=status):
                obs = rest.TbRestObserver(self.Transport(status, {}),
                                          "dev", "u", "p")
                with self.assertRaises(rest.ObserverError) as ctx:
                    run(obs.start())
                self.assertEqual(ctx.exception.code, code)

    def test_login_without_token_is_protocol(self):
        obs = rest.TbRestObserver(self.Transport(200, {}), "dev", "u", "p")
        with self.assertRaises(rest.ObserverError) as ctx:
            run(obs.start())
        self.assertEqual(ctx.exception.code, "protocol")

    def test_fetch_device_is_readonly_get(self):
        class T:
            def __init__(self):
                self.calls = []

            async def request(self, method, path, token=None, body=None):
                self.calls.append((method, path))
                if path.endswith("/login"):
                    return 200, {"token": "jwt"}
                return 200, {"id": {"id": "dev"},
                             "name": "Test Device C1"}

        transport = T()
        obs = rest.TbRestObserver(transport, "dev", "u", "p")
        run(obs.start())
        device = run(obs.fetch_device())
        self.assertEqual(device["name"], "Test Device C1")
        self.assertEqual(transport.calls[-1],
                         ("GET", "/api/device/dev"))

    def test_series_not_object_is_protocol(self):
        class T:
            async def request(self, method, path, token=None, body=None):
                return 200, {"token": "jwt"} if path.endswith("/login") else []

        obs = rest.TbRestObserver(T(), "dev", "u", "p")
        run(obs.start())
        with self.assertRaises(rest.ObserverError) as ctx:
            run(obs.fetch_series(("net_mode",), 0, 1))
        self.assertEqual(ctx.exception.code, "protocol")


class SourceIsolationTests(unittest.TestCase):
    def test_no_mqtt_or_device_token_logic(self):
        for name in ("c152_telemetry_rest.py", "c152_rest_observer.py"):
            text = (ROOT / "tools" / "review" / name).read_text(encoding="utf-8")
            low = text.lower()
            self.assertNotIn("paho", low, name)
            self.assertNotIn("iothub_mqtt_token", low, name)
            self.assertNotIn("v1/devices/me", low, name)

    def test_exit_codes(self):
        self.assertEqual(p.exit_code("PASS"), 0)
        self.assertEqual(p.exit_code("FAIL"), 1)
        self.assertEqual(p.exit_code("NOT_COVERED"), 2)
        self.assertEqual(p.exit_code("STOP"), 3)


if __name__ == "__main__":
    unittest.main()
