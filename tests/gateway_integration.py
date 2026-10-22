"""Real HTTP/WebSocket integration checks. Uses only Python's standard library."""
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import select
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest

DRIVER, STANDALONE = sys.argv[1:3]
sys.argv[1:] = []
ADMIN = "integration-admin-token"
VIEWER = "integration-viewer-token"


def test_environment(**options):
    # 集成测试不能继承开发者的真实 Gateway 存储路径或认证/TLS 配置。
    env = {key: value for key, value in os.environ.items() if not key.startswith("UAV_GATEWAY_")}
    env.update(options)
    return env


def launch(command, env=None, cwd=None):
    if env is None:
        env = test_environment(UAV_GATEWAY_STATE="")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=env, cwd=cwd)
    if not select.select([process.stdout], [], [], 10)[0]:
        process.kill()
        raise AssertionError("gateway did not become ready")
    line = process.stdout.readline().strip()
    if not line:
        raise AssertionError(process.stderr.read())
    return process, int(line.rsplit(" ", 1)[-1])


def stop(process):
    if process.poll() is None:
        process.terminate()
    try:
        _, stderr = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise AssertionError("gateway did not stop with open connections")
    if process.returncode != 0:
        raise AssertionError(f"gateway exited {process.returncode}: {stderr}")


def request(port, method, path, body=None, token=ADMIN, headers=None, tls_context=None):
    fields = dict(headers or {})
    if token is not None:
        fields["Authorization"] = "Bearer " + token
    if body is not None and not isinstance(body, str):
        body = json.dumps(body)
    if body is not None:
        fields.setdefault("Content-Type", "application/json")
    connection = (http.client.HTTPSConnection("127.0.0.1", port, timeout=3, context=tls_context)
                  if tls_context else http.client.HTTPConnection("127.0.0.1", port, timeout=3))
    try:
        connection.request(method, path, body=body, headers=fields)
        response = connection.getresponse()
        data = json.loads(response.read())
        return response.status, data
    finally:
        connection.close()


class WebSocket:
    def __init__(self, port, path, origin=None, tls_context=None):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        if tls_context:
            self.socket = tls_context.wrap_socket(self.socket, server_hostname="127.0.0.1")
        self.file = self.socket.makefile("rb")
        key = base64.b64encode(os.urandom(16)).decode()
        headers = [f"GET {path} HTTP/1.1", f"Host: 127.0.0.1:{port}", "Upgrade: websocket",
                   "Connection: Upgrade", f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
        if origin:
            headers.append("Origin: " + origin)
        self.socket.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
        self.status = int(self.file.readline().split()[1])
        received = {}
        while True:
            line = self.file.readline()
            if line == b"\r\n":
                break
            if not line:
                raise AssertionError("incomplete WebSocket handshake")
            name, value = line.decode().split(":", 1)
            received[name.lower()] = value.strip()
        if self.status == 101:
            expected = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            assert received["sec-websocket-accept"] == expected

    def send(self, text, opcode=1):
        payload = text.encode() if isinstance(text, str) else text
        mask = os.urandom(4)
        length = len(payload)
        prefix = bytes([0x80 | opcode])
        if length < 126:
            prefix += bytes([0x80 | length])
        else:
            prefix += bytes([0x80 | 126]) + struct.pack("!H", length)
        masked = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        self.socket.sendall(prefix + mask + masked)

    def receive(self):
        def read_exact(length):
            data = self.file.read(length)
            if len(data) != length:
                raise EOFError("WebSocket closed")
            return data

        fragments = bytearray()
        while True:
            header = read_exact(2)
            opcode, length = header[0] & 15, header[1] & 127
            if length == 126:
                length = struct.unpack("!H", read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", read_exact(8))[0]
            payload = read_exact(length)
            if opcode == 9:
                self.send(payload, opcode=10)
                continue
            if opcode == 10:
                continue
            if opcode == 8:
                raise EOFError("WebSocket closed")
            if opcode not in (0, 1):
                raise AssertionError(f"unexpected WebSocket opcode: {opcode}")
            fragments.extend(payload)
            if header[0] & 0x80:
                return json.loads(fragments)

    def close(self):
        self.file.close()
        self.socket.close()


class GatewayIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.process, cls.port = launch([DRIVER])

    @classmethod
    def tearDownClass(cls):
        stop(cls.process)

    def api(self, method, path, body=None, **kwargs):
        return request(self.port, method, path, body, **kwargs)

    def command(self, action, parameters=None, ttl=2000):
        return self.api("POST", "/api/v1/devices/camera/commands",
                        {"action": action, "parameters": parameters or {}, "ttl_ms": ttl})

    def finished(self, command_id):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            status, result = self.api("GET", "/api/v1/commands/" + command_id)
            self.assertEqual(status, 200)
            if result["data"]["state"] not in ("queued", "running"):
                return result["data"]
            time.sleep(0.02)
        self.fail("command did not finish")

    def ticket(self):
        status, body = self.api("POST", "/api/v1/ws-ticket", token=VIEWER)
        self.assertEqual(status, 201)
        return body["data"]["url"]

    def test_authentication_and_roles(self):
        self.assertEqual(self.api("GET", "/healthz", token=None)[0], 200)
        for token in (None, "incorrect"):
            self.assertEqual(self.api("GET", "/api/v1/devices", token=token)[0], 401)
        status, body = self.api("GET", "/api/v1/devices", token=VIEWER)
        self.assertEqual(status, 200)
        self.assertNotIn("config", body["data"][0])
        self.assertEqual(self.api("PUT", "/api/v1/devices/new", {"type": "radar"}, token=VIEWER)[0], 403)
        self.assertEqual(self.api("GET", "/api/v1/status", headers={"Origin": "https://foreign.invalid"})[0], 403)
        self.assertEqual(self.api("GET", "/api/v1/status", headers={"Origin": f"http://127.0.0.1:{self.port}"})[0], 200)

    def login(self, token=VIEWER):
        status, result = self.api("POST", "/api/v1/auth/login", {"token": token}, token=None)
        self.assertEqual(status, 201)
        return result["data"]

    def test_login_refresh_logout_and_websocket_revocation(self):
        self.assertEqual(self.api("POST", "/api/v1/auth/login", {"token": "wrong"}, token=None)[0], 401)
        session = self.login()
        access = session["access_token"]
        self.assertEqual(self.api("GET", "/api/v1/auth/me", token=access)[1]["data"]["role"], "viewer")
        self.assertEqual(self.api("PUT", "/api/v1/devices/forbidden", {"type": "radar"}, token=access)[0], 403)
        path = self.api("POST", "/api/v1/ws-ticket", token=access)[1]["data"]["url"]
        client = WebSocket(self.port, path)
        unused_ticket = self.api("POST", "/api/v1/ws-ticket", token=access)[1]["data"]["url"]
        try:
            self.assertEqual(client.receive()["type"], "snapshot")
            status, renewed = self.api("POST", "/api/v1/auth/refresh", {"refresh_token": session["refresh_token"]}, token=None)
            self.assertEqual(status, 200)
            self.assertEqual(self.api("GET", "/api/v1/auth/me", token=access)[0], 401)
            self.assertEqual(self.api("POST", "/api/v1/auth/refresh", {"refresh_token": session["refresh_token"]}, token=None)[0], 401)
            access = renewed["data"]["access_token"]
            self.assertEqual(self.api("POST", "/api/v1/auth/logout", token=access)[0], 200)
            self.assertEqual(self.api("GET", "/api/v1/auth/me", token=access)[0], 401)
            denied = WebSocket(self.port, unused_ticket)
            self.assertEqual(denied.status, 401)
            denied.close()
            with self.assertRaises((EOFError, ConnectionResetError)):
                for _ in range(4):
                    client.receive()
                self.fail("logged-out websocket remained authorized")
        finally:
            client.close()

    def test_session_expiry_and_refresh(self):
        session = self.login()
        time.sleep(2.1)
        self.assertEqual(self.api("GET", "/api/v1/status", token=session["access_token"])[0], 401)
        self.assertEqual(self.api("POST", "/api/v1/auth/refresh", {"refresh_token": session["refresh_token"]}, token=None)[0], 200)

    def test_playback_scope_owner_renewal_expiry_and_logout(self):
        session, other = self.login(), self.login()
        token = session["access_token"]
        status, result = self.api("POST", "/api/v1/playback", {"stream_id": "camera.main", "protocol": "webrtc"}, token=token)
        self.assertEqual(status, 200)
        grant = result["data"]
        path = "/api/v1/playback/" + grant["session_id"]
        def validate(media_token, stream="camera.main", protocol="webrtc", service="integration-media-token"):
            return self.api("POST", "/api/v1/media/authorize", {"token": media_token, "stream_id": stream, "protocol": protocol}, token=service)[0]
        self.assertEqual(validate(grant["media_token"]), 200)
        self.assertEqual(validate(grant["media_token"], protocol="rtsp"), 403)
        self.assertEqual(validate(grant["media_token"], stream="radar"), 403)
        self.assertEqual(validate(grant["media_token"], service=ADMIN), 401)
        self.assertEqual(self.api("GET", path, token=other["access_token"])[0], 404)
        self.assertEqual(self.api("DELETE", path, token=other["access_token"])[0], 404)
        status, renewed = self.api("POST", path + "/renew", token=token)
        self.assertEqual(status, 200)
        self.assertEqual(validate(grant["media_token"]), 403)
        self.assertEqual(validate(renewed["data"]["media_token"]), 200)
        self.assertEqual(self.api("POST", "/api/v1/auth/logout", token=token)[0], 200)
        self.assertEqual(validate(renewed["data"]["media_token"]), 403)
        _, result = self.api("POST", "/api/v1/playback", {"stream_id": "camera.main", "protocol": "webrtc"})
        grant = result["data"]
        self.assertEqual(self.api("DELETE", "/api/v1/playback/" + grant["session_id"])[0], 200)
        self.assertEqual(validate(grant["media_token"]), 403)
        _, result = self.api("POST", "/api/v1/playback", {"stream_id": "camera.main", "protocol": "webrtc"})
        time.sleep(2.1)
        self.assertEqual(validate(result["data"]["media_token"]), 403)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "camera.main", "protocol": "gb28181"})[1]["error"]["code"], "native_protocol")

    def test_command_cancellation_and_history(self):
        _, slow = self.command("slow")
        _, queued = self.command("echo")
        command_id = queued["data"]["id"]
        self.assertEqual(self.api("DELETE", "/api/v1/commands/" + command_id)[0], 200)
        self.assertEqual(self.finished(command_id)["state"], "cancelled")
        self.assertEqual(self.finished(slow["data"]["id"])["state"], "succeeded")
        self.assertEqual(self.api("DELETE", "/api/v1/commands/" + command_id)[0], 409)
        self.assertTrue(self.api("GET", "/api/v1/commands")[1]["data"])

    def test_stream_registry_audit_and_openapi(self):
        body = {"device_id": "radar", "codec": "h265", "protocols": ["webrtc"]}
        self.assertEqual(self.api("PUT", "/api/v1/streams/radar.test", body)[0], 201)
        self.assertEqual(self.api("GET", "/api/v1/streams/radar.test")[1]["data"]["codec"], "h265")
        self.assertEqual(self.api("PUT", "/api/v1/streams/camera.main", body)[0], 409)
        self.assertEqual(self.api("PUT", "/api/v1/streams/radar.test", dict(body, protocols=["webrtc", "webrtc"]))[0], 400)
        self.assertEqual(self.api("DELETE", "/api/v1/streams/radar.test")[0], 200)
        self.assertEqual(self.api("GET", "/api/v1/audit", token=VIEWER)[0], 403)
        status, audit = self.api("GET", "/api/v1/audit")
        self.assertEqual(status, 200)
        self.assertNotIn(ADMIN, json.dumps(audit))
        status, document = self.api("GET", "/openapi.json", token=None)
        self.assertEqual(status, 200)
        self.assertEqual(document["openapi"], "3.0.3")
        self.assertIn("/api/v1/auth/login", document["paths"])

    def test_device_configuration_and_unimplemented_backend(self):
        status, body = self.api("PUT", "/api/v1/devices/radar.new",
                                {"type": "radar", "config": {"host": "192.0.2.10", "port": 9000}})
        self.assertEqual(status, 201)
        self.assertEqual(body["data"]["status"]["state"], "not_implemented")
        self.assertEqual(self.api("GET", "/api/v1/devices/radar.new")[1]["data"]["config"]["port"], 9000)
        self.assertEqual(self.api("POST", "/api/v1/devices/radar.new/commands", {"action": "connect"})[0], 503)
        self.assertEqual(self.api("PUT", "/api/v1/devices/camera", {"type": "camera"})[0], 409)
        self.assertEqual(self.api("DELETE", "/api/v1/devices/camera")[0], 409)
        self.assertEqual(self.api("PUT", "/api/v1/devices/bad", {"type": "unknown"})[0], 400)
        self.assertEqual(self.api("DELETE", "/api/v1/devices/radar.new")[0], 200)
        self.assertEqual(self.api("GET", "/api/v1/devices/radar.new")[0], 404)

    def test_playback_resolves_before_authorization_without_leaking_url(self):
        body = {"stream_id": "camera.main", "protocol": "webrtc"}
        status, first = self.api("POST", "/api/v1/playback", body)
        self.assertEqual(status, 200)
        count = first["data"]["parameters"]["resolve_count"]
        status, denied = self.api("POST", "/api/v1/playback", body, token=None)
        self.assertEqual(status, 401)
        self.assertNotIn("media.invalid", json.dumps(denied))
        status, result = self.api("POST", "/api/v1/playback", body, token=VIEWER)
        self.assertEqual(status, 200)
        self.assertEqual(result["data"]["parameters"]["resolve_count"], count + 2)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "missing", "protocol": "webrtc"}, token=None)[0], 401)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "missing", "protocol": "webrtc"})[0], 404)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "camera.main", "protocol": "rtsp"})[0], 503)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "disabled.main", "protocol": "webrtc"})[0], 409)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "unsafe.main", "protocol": "webrtc"})[0], 502)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "error.main", "protocol": "webrtc"}, token=None)[0], 401)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "error.main", "protocol": "webrtc"})[0], 503)

    def test_command_results_and_backend_exception(self):
        for action, expected in (("echo", "succeeded"), ("fail", "failed"), ("throw", "failed"),
                                 ("large", "failed"), ("dispatch", "dispatched")):
            status, body = self.command(action, {"value": 42})
            self.assertEqual(status, 202)
            record = self.finished(body["data"]["id"])
            self.assertEqual(record["state"], expected)
            if action == "echo":
                self.assertEqual(record["result"]["data"], {"value": 42})
            if action == "throw":
                self.assertEqual(record["result"]["code"], "backend_exception")
                self.assertNotIn("internal detail", json.dumps(record))
            if action == "large":
                self.assertEqual(record["result"]["code"], "backend_result_too_large")
        self.assertEqual(self.command("unsupported")[0], 422)
        self.assertEqual(self.command("echo", ttl=0)[0], 400)

    def test_queue_capacity_expiry_and_nonblocking_http(self):
        status, slow = self.command("slow")
        self.assertEqual(status, 202)
        status, expiring = self.command("echo", ttl=100)
        self.assertEqual(status, 202)
        self.assertEqual(self.command("echo")[0], 429)
        started = time.monotonic()
        self.assertEqual(self.api("GET", "/api/v1/status")[0], 200)
        self.assertLess(time.monotonic() - started, 0.3)
        self.assertEqual(self.finished(slow["data"]["id"])["state"], "succeeded")
        self.assertEqual(self.finished(expiring["data"]["id"])["state"], "expired")

    def test_websocket_ticket_events_and_ping(self):
        path = self.ticket()
        socket_client = WebSocket(self.port, path, f"http://127.0.0.1:{self.port}")
        try:
            self.assertEqual(socket_client.status, 101)
            self.assertEqual(socket_client.receive()["type"], "snapshot")
            replay = WebSocket(self.port, path)
            self.assertEqual(replay.status, 401)
            replay.close()
            socket_client.send('{"type":"ping"}')
            while socket_client.receive()["type"] != "pong":
                pass
            status, body = self.command("echo", {"value": 99})
            self.assertEqual(status, 202)
            while True:
                message = socket_client.receive()
                if message["type"] == "command.result" and message["data"]["id"] == body["data"]["id"]:
                    self.assertEqual(message["data"]["state"], "succeeded")
                    break
            socket_client.send("not-json")
            while socket_client.receive()["type"] != "error":
                pass
        finally:
            socket_client.close()

    def test_slow_websocket_isolated_from_http(self):
        client = WebSocket(self.port, self.ticket())
        try:
            self.assertEqual(client.receive()["type"], "snapshot")
            status, body = self.command("flood")
            self.assertEqual(status, 202)
            self.finished(body["data"]["id"])
            self.assertEqual(self.api("GET", "/api/v1/status")[0], 200)
            # 客户端故意不读，输出达到上限后应关闭该会话，而非无限排队。
            with self.assertRaises((EOFError, ConnectionResetError)):
                for _ in range(300):
                    client.receive()
                self.fail("slow WebSocket remained connected after queue overflow")
        finally:
            client.close()

    def test_websocket_ticket_expiry_and_origin(self):
        denied = WebSocket(self.port, self.ticket(), "https://foreign.invalid")
        self.assertEqual(denied.status, 403)
        denied.close()
        path = self.ticket()
        time.sleep(1.1)
        expired = WebSocket(self.port, path)
        self.assertEqual(expired.status, 401)
        expired.close()

    def test_invalid_requests_and_body_limit(self):
        self.assertEqual(self.api("POST", "/api/v1/playback", "{")[0], 400)
        self.assertEqual(self.api("POST", "/api/v1/playback", "[]")[0], 400)
        self.assertEqual(self.api("POST", "/api/v1/playback", {"stream_id": "../bad", "protocol": "rtsp"})[0], 400)
        self.assertEqual(self.api("POST", "/api/v1/playback", " " * 70000)[0], 413)
        self.assertEqual(self.api("PATCH", "/api/v1/status")[0], 405)
        self.assertEqual(self.api("GET", "/api/v1/missing")[0], 404)


class StandaloneIntegration(unittest.TestCase):
    def environment(self, directory, **options):
        return test_environment(UAV_GATEWAY_TOKEN=ADMIN, UAV_GATEWAY_VIEW_TOKEN=VIEWER,
                                UAV_GATEWAY_STATE=str(Path(directory) / "state.json"), **options)

    def test_startup_configuration_without_environment_credentials(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = Path(directory) / "config" / "gateway.json"
            settings.parent.mkdir()
            settings.write_text(json.dumps({"admin_token": ADMIN, "viewer_token": VIEWER,
                                           "bind_address": "0.0.0.0", "port": 0}))
            # No --gateway-config, --bind, --port, or authentication environment variables.
            process, port = launch([STANDALONE], test_environment(UAV_GATEWAY_STATE=""), cwd=directory)
            try:
                self.assertEqual(request(port, "GET", "/api/v1/status")[0], 200)
                status, login = request(port, "POST", "/api/v1/auth/login", {"token": VIEWER}, token=None)
                self.assertEqual(status, 201)
                self.assertEqual(login["data"]["role"], "viewer")
                self.assertEqual(request(port, "GET", "/api/v1/status", token="wrong")[0], 401)
                self.assertNotIn(ADMIN, json.dumps(request(port, "GET", "/api/v1/status")[1]))
            finally:
                stop(process)

    def test_explicit_startup_file_and_override_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = Path(directory) / "settings.json"
            settings.write_text(json.dumps({"admin_token": "file-admin", "viewer_token": "file-viewer",
                                           "bind_address": "192.0.2.254", "port": 8080}))
            env = self.environment(directory, UAV_GATEWAY_BIND="127.0.0.1", UAV_GATEWAY_PORT="0")
            process, port = launch([STANDALONE, "--gateway-config", str(settings)], env)
            try:
                self.assertEqual(request(port, "GET", "/api/v1/status")[0], 200)
                self.assertEqual(request(port, "GET", "/api/v1/status", token="file-admin")[0], 401)
            finally:
                stop(process)
            env.update(UAV_GATEWAY_BIND="192.0.2.254", UAV_GATEWAY_PORT="1")
            process, port = launch([STANDALONE, "--gateway-config", str(settings),
                                    "--bind", "127.0.0.1", "--port", "0"], env)
            try:
                self.assertGreater(port, 1024)
                self.assertEqual(request(port, "GET", "/api/v1/status")[0], 200)
            finally:
                stop(process)

    def test_startup_configuration_errors_do_not_expose_credentials(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = Path(directory) / "gateway.json"
            secret = "configuration-fixture-secret"
            env = test_environment(UAV_GATEWAY_STATE="")
            for invalid in ("{", "[]", json.dumps({"admin_token": 42}),
                            json.dumps({"admin_token": secret, "bind_address": False}),
                            *(json.dumps({"admin_token": secret, "port": value}) for value in (-1, 65536, True, 8080.5, "8080")),
                            json.dumps({"admin_token": secret, "unknown": True})):
                with self.subTest(configuration=invalid):
                    settings.write_text(invalid)
                    result = subprocess.run([STANDALONE, "--gateway-config", str(settings)],
                                            env=env, capture_output=True, text=True, timeout=3)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn(secret, result.stdout + result.stderr)
            settings.unlink()
            result = subprocess.run([STANDALONE, "--gateway-config", str(settings)],
                                    env=env, capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot read", result.stderr)

    def test_startup_credentials_cannot_be_served_as_static_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = Path(directory) / "gateway.json"
            settings.write_text(json.dumps({"admin_token": ADMIN, "port": 0}))
            result = subprocess.run([STANDALONE, "--gateway-config", str(settings), "--web-root", directory],
                                    env=test_environment(UAV_GATEWAY_STATE=""), capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("outside web root", result.stderr)
            self.assertNotIn(ADMIN, result.stdout + result.stderr)

    def test_persistent_registry_preserves_runtime_adapters(self):
        with tempfile.TemporaryDirectory() as directory:
            env = self.environment(directory)
            process, port = launch([DRIVER], env)
            try:
                self.assertEqual(request(port, "PUT", "/api/v1/devices/saved", {"type": "radar"})[0], 201)
            finally:
                stop(process)
            state_file = Path(env["UAV_GATEWAY_STATE"])
            state = json.loads(state_file.read_text())
            for device in state["devices"]:
                if device["id"] == "camera":
                    device["type"] = "laser"
                    device["config"] = {"saved_override": True}
            for stream in state["streams"]:
                if stream["id"] == "camera.main":
                    stream["protocols"] = ["rtsp"]
            state_file.write_text(json.dumps(state))
            process, port = launch([DRIVER], env)
            try:
                camera = request(port, "GET", "/api/v1/devices/camera")[1]["data"]
                self.assertEqual(camera["type"], "camera")
                self.assertNotIn("saved_override", camera["config"])
                self.assertIn("echo", camera["commands"])
                self.assertEqual(request(port, "GET", "/api/v1/devices/saved")[0], 200)
                self.assertEqual(request(port, "POST", "/api/v1/playback", {"stream_id": "camera.main", "protocol": "webrtc"})[0], 200)
                status, command = request(port, "POST", "/api/v1/devices/camera/commands", {"action": "echo"})
                self.assertEqual(status, 202)
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    result = request(port, "GET", "/api/v1/commands/" + command["data"]["id"])[1]["data"]
                    if result["state"] == "succeeded": break
                    time.sleep(0.01)
                self.assertEqual(result["state"], "succeeded")
            finally:
                stop(process)

    def test_persistence_restart_deletion_and_exclusive_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            seed = Path(directory) / "seed.json"
            seed.write_text(json.dumps({"devices": [{"id": "seed", "type": "radar"}]}))
            env = self.environment(directory)
            args = [STANDALONE, "--port", "0", "--config", str(seed)]
            process, port = launch(args, env)
            try:
                self.assertEqual(request(port, "PUT", "/api/v1/devices/saved", {"type": "spectrum", "config": {"host": "192.0.2.1"}})[0], 201)
                self.assertEqual(request(port, "PUT", "/api/v1/streams/saved.main", {"device_id": "saved", "protocols": ["webrtc"]})[0], 201)
                self.assertEqual(request(port, "DELETE", "/api/v1/devices/seed")[0], 200)
                state = Path(env["UAV_GATEWAY_STATE"])
                self.assertEqual(state.stat().st_mode & 0o777, 0o600)
                self.assertNotIn(ADMIN, state.read_text())
                second = subprocess.run(args, env=env, capture_output=True, text=True, timeout=3)
                self.assertNotEqual(second.returncode, 0)
                self.assertIn("locked", second.stderr)
            finally:
                stop(process)
            process, port = launch(args, env)
            try:
                self.assertEqual(request(port, "GET", "/api/v1/devices/seed")[0], 404)
                self.assertEqual(request(port, "GET", "/api/v1/devices/saved")[1]["data"]["config"]["host"], "192.0.2.1")
                self.assertEqual(request(port, "GET", "/api/v1/streams/saved.main")[0], 200)
                self.assertEqual(request(port, "POST", "/api/v1/playback", {"stream_id": "saved.main", "protocol": "webrtc"})[0], 503)
                self.assertEqual(list(Path(directory).glob("*.tmp.*")), [])
            finally:
                stop(process)

    def test_failed_persistence_does_not_change_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            env = self.environment(directory)
            process, port = launch([STANDALONE, "--port", "0"], env)
            try:
                state = Path(env["UAV_GATEWAY_STATE"])
                state.mkdir() # 原子替换无法覆盖目录，模拟存储写入失败
                status, result = request(port, "PUT", "/api/v1/devices/not.saved", {"type": "radar"})
                self.assertEqual(status, 503)
                self.assertEqual(result["error"]["code"], "persistence_failed")
                self.assertEqual(request(port, "GET", "/api/v1/devices/not.saved")[0], 404)
                self.assertEqual(list(Path(directory).glob("*.tmp.*")), [])
            finally:
                stop(process)

    def test_static_assets_spa_paths_cors_and_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "web"
            root.mkdir()
            (root / "index.html").write_text("<!doctype html><title>Gateway asset fixture</title>")
            (root / "app.js").write_text("console.log('fixture');")
            secret = Path(directory) / "secret.txt"
            secret.write_text("PRIVATE")
            (root / "escape.txt").symlink_to(secret)
            env = self.environment(directory, UAV_GATEWAY_ORIGIN="https://console.example")
            process, port = launch([STANDALONE, "--port", "0", "--web-root", str(root)], env)
            def raw(method, path, headers=None):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                try:
                    connection.request(method, path, headers=headers or {})
                    reply = connection.getresponse()
                    return reply.status, dict(reply.getheaders()), reply.read()
                finally:
                    connection.close()
            try:
                status, headers, body = raw("GET", "/")
                self.assertEqual(status, 200)
                self.assertIn(b"Gateway asset fixture", body)
                self.assertIn("text/html", headers["Content-Type"])
                self.assertIn("X-Request-ID", headers)
                self.assertEqual(raw("GET", "/", {"If-None-Match": headers["ETag"]})[0], 304)
                self.assertEqual(raw("GET", "/", {"If-None-Match": "W/" + headers["ETag"]})[2], b"")
                self.assertEqual(raw("GET", "/app.js?v=1")[0], 200)
                self.assertEqual(raw("HEAD", "/app.js")[2], b"")
                for path in ("/webrtc", "/webrtc/", "/webrtc?view=all"):
                    self.assertEqual(raw("GET", path)[2], body)
                    self.assertEqual(raw("HEAD", path)[0], 200)
                    self.assertEqual(raw("HEAD", path)[2], b"")
                self.assertEqual(raw("GET", "/settings", {"Accept": "text/html"})[2], body)
                self.assertEqual(raw("GET", "/missing.js")[0], 404)
                for path in ("/../secret.txt", "/%2e%2e/secret.txt", "/escape.txt", "/.env"):
                    self.assertEqual(raw("GET", path)[0], 403, path)
                self.assertEqual(raw("GET", "/%00.txt")[0], 400)
                self.assertEqual(request(port, "GET", "/api/v1/missing")[0], 404)
                status, headers, _ = raw("OPTIONS", "/api/v1/devices", {"Origin": "https://console.example", "Access-Control-Request-Method": "PUT", "Access-Control-Request-Headers": "authorization,content-type"})
                self.assertEqual(status, 204)
                self.assertEqual(headers["Access-Control-Allow-Origin"], "https://console.example")
                self.assertIn("Authorization", headers["Access-Control-Allow-Headers"])
                self.assertEqual(raw("OPTIONS", "/api/v1/devices", {"Origin": "https://foreign.example"})[0], 403)
                self.assertEqual(request(port, "GET", "/readyz", token=None)[1]["data"]["ready"], True)
            finally:
                stop(process)

    def test_https_wss_and_shutdown_during_tls_handshake(self):
        with tempfile.TemporaryDirectory() as directory:
            cert, key = Path(directory) / "cert.pem", Path(directory) / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                            "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
            context = ssl.create_default_context(cafile=str(cert))
            env = self.environment(directory)
            process, port = launch([STANDALONE, "--port", "0", "--tls-cert", str(cert), "--tls-key", str(key)], env)
            client, idle = None, None
            try:
                self.assertEqual(request(port, "GET", "/api/v1/status", tls_context=context)[0], 200)
                path = request(port, "POST", "/api/v1/ws-ticket", tls_context=context)[1]["data"]["url"]
                client = WebSocket(port, path, tls_context=context)
                self.assertEqual(client.status, 101)
                self.assertEqual(client.receive()["type"], "snapshot")
                client.send('{"type":"ping"}')
                while client.receive()["type"] != "pong":
                    pass
                idle = socket.create_connection(("127.0.0.1", port))
                stop(process)
            finally:
                if client: client.close()
                if idle: idle.close()
                if process.poll() is None: stop(process)

    def test_login_rate_limit_and_invalid_tls_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            env = self.environment(directory)
            process, port = launch([STANDALONE, "--port", "0"], env)
            try:
                for _ in range(20):
                    self.assertEqual(request(port, "POST", "/api/v1/auth/login", {"token": "bad"}, token=None)[0], 401)
                self.assertEqual(request(port, "POST", "/api/v1/auth/login", {"token": ADMIN}, token=None)[0], 429)
            finally:
                stop(process)
            failed = subprocess.run([STANDALONE, "--port", "0", "--tls-cert", "missing.pem"], env=env, capture_output=True, text=True, timeout=3)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("TLS requires", failed.stderr)

    def test_shutdown_during_device_command(self):
        process, port = launch([DRIVER])
        try:
            status, body = request(port, "POST", "/api/v1/devices/camera/commands", {"action": "slow"})
            self.assertEqual(status, 202)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                record = request(port, "GET", "/api/v1/commands/" + body["data"]["id"])[1]["data"]
                if record["state"] == "running":
                    break
            self.assertEqual(record["state"], "running")
            self.assertEqual(request(port, "POST", "/api/v1/devices/camera/commands", {"action": "echo"})[0], 202)
            stop(process)
        finally:
            if process.poll() is None:
                stop(process)

    def test_registry_and_shutdown_with_idle_http_and_websocket(self):
        with tempfile.TemporaryDirectory() as directory:
            registry = Path(directory) / "registry.json"
            registry.write_text(json.dumps({"devices": [{"id": "radar", "type": "radar"}],
                "streams": [{"id": "radar.stream", "device_id": "radar", "protocols": ["webrtc"]}]}))
            env = test_environment(UAV_GATEWAY_TOKEN=ADMIN, UAV_GATEWAY_STATE=str(Path(directory) / "state.json"))
            process, port = launch([STANDALONE, "--port", "0", "--config", str(registry)], env)
            idle = socket.create_connection(("127.0.0.1", port))
            ws_client = None
            try:
                self.assertEqual(request(port, "GET", "/api/v1/devices")[1]["data"][0]["id"], "radar")
                self.assertEqual(request(port, "POST", "/api/v1/playback",
                    {"stream_id": "radar.stream", "protocol": "webrtc"})[0], 503)
                path = request(port, "POST", "/api/v1/ws-ticket")[1]["data"]["url"]
                ws_client = WebSocket(port, path)
                self.assertEqual(ws_client.status, 101)
                stop(process)
            finally:
                idle.close()
                if ws_client:
                    ws_client.close()
                if process.poll() is None:
                    stop(process)

    def test_token_required_and_invalid_configuration(self):
        env = test_environment()
        env["UAV_GATEWAY_TOKEN"] = "" # Ignore an administrator's locally configured default token.
        result = subprocess.run([STANDALONE, "--port", "0"], env=env,
                                capture_output=True, text=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("UAV_GATEWAY_TOKEN", result.stderr)
        result = subprocess.run([STANDALONE, "--port", "-1"], env=env,
                                capture_output=True, text=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
