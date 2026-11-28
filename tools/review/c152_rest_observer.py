"""Read-only ThingsBoard REST observer for C15-2 (telemetry + attributes).

No MQTT. The device Access Token is never used. Credentials are held in
memory only and never logged. HTTP is injectable so Host tests can run with
fake transports and no network access.

Error mapping (`ObserverError.code`):
  auth, forbidden, not_found, rate_limited, server, timeout, network,
  protocol, http_<status>
"""

from __future__ import annotations

import asyncio
import json
import socket
import urllib.error
import urllib.parse
import urllib.request


class ObserverError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code + (":" + detail if detail else ""))
        self.code = code
        self.detail = detail


def _classify_status(status: int) -> None:
    if status == 200:
        return
    if status == 401:
        raise ObserverError("auth")
    if status == 403:
        raise ObserverError("forbidden")
    if status == 404:
        raise ObserverError("not_found")
    if status == 429:
        raise ObserverError("rate_limited")
    if 500 <= status < 600:
        raise ObserverError("server")
    raise ObserverError("http_%d" % status)


class UrllibTransport:
    """Blocking urllib wrapped in a thread; no proxy, no redirects storage."""

    def __init__(self, base: str, timeout: float = 10.0) -> None:
        self.base = base.rstrip("/")
        self.timeout = timeout
        self._opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}))

    async def request(self, method: str, path: str, token: str | None = None,
                      body: dict | None = None):
        return await asyncio.to_thread(self._request, method, path, token, body)

    def _request(self, method, path, token, body):
        headers = {"Accept": "application/json"}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        if token:
            headers["X-Authorization"] = "Bearer " + token
        req = urllib.request.Request(self.base + path, data=data,
                                     headers=headers, method=method)
        try:
            with self._opener.open(req, timeout=self.timeout) as resp:
                raw = resp.read()
                if not raw:
                    return resp.status, None
                try:
                    return resp.status, json.loads(raw)
                except ValueError as exc:
                    raise ObserverError("protocol", "invalid_json") from exc
        except urllib.error.HTTPError as exc:
            return exc.code, None
        except (TimeoutError, socket.timeout) as exc:
            raise ObserverError("timeout", type(exc).__name__) from exc
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", None)
            if isinstance(reason, (TimeoutError, socket.timeout)):
                raise ObserverError("timeout", "urlopen_timeout") from exc
            raise ObserverError("network", type(reason).__name__) from exc
        except OSError as exc:
            raise ObserverError("network", type(exc).__name__) from exc


class TbRestObserver:
    def __init__(self, transport, device_id: str, user: str, password: str
                 ) -> None:
        self.transport = transport
        self.device_id = device_id
        self._user = user
        self._password = password
        self._token: str | None = None

    async def start(self) -> None:
        status, body = await self.transport.request(
            "POST", "/api/auth/login",
            body={"username": self._user, "password": self._password})
        _classify_status(status)
        if not isinstance(body, dict) or not body.get("token"):
            raise ObserverError("protocol", "login_no_token")
        self._token = body["token"]

    def _require_token(self) -> str:
        if not self._token:
            raise ObserverError("auth", "not_logged_in")
        return self._token

    async def fetch_device(self) -> dict:
        """Read-only device entity used to bind the UUID to a TB device name."""
        path = "/api/device/" + urllib.parse.quote(self.device_id)
        status, body = await self.transport.request(
            "GET", path, token=self._require_token())
        _classify_status(status)
        if not isinstance(body, dict):
            raise ObserverError("protocol", "device_not_object")
        return body

    async def fetch_series(self, keys, start_ms: int, end_ms: int) -> dict:
        query = urllib.parse.urlencode({
            "keys": ",".join(keys),
            "startTs": int(start_ms),
            "endTs": int(end_ms),
            "limit": 500,
        })
        path = ("/api/plugins/telemetry/DEVICE/%s/values/timeseries?%s"
                % (urllib.parse.quote(self.device_id), query))
        status, body = await self.transport.request(
            "GET", path, token=self._require_token())
        _classify_status(status)
        if not isinstance(body, dict):
            raise ObserverError("protocol", "series_not_object")
        return body

    async def fetch_attributes(self, keys) -> list:
        query = urllib.parse.urlencode({"keys": ",".join(keys)})
        path = ("/api/plugins/telemetry/DEVICE/%s/values/attributes?%s"
                % (urllib.parse.quote(self.device_id), query))
        status, body = await self.transport.request(
            "GET", path, token=self._require_token())
        _classify_status(status)
        if not isinstance(body, list):
            raise ObserverError("protocol", "attributes_not_list")
        return body
