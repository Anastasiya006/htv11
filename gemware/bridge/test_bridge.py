"""End-to-end tests for gemmi_bridge on a throwaway port + temp state dir.

Run:  python3 gemware/bridge/test_bridge.py
Drives a fake device over the real HTTP interface, so no ESP32 is needed.
"""
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gemmi_bridge as gb

PORT = 8761


def req(method, path, body=None, accept=None):
    url = f"http://127.0.0.1:{PORT}{path}"
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(url, data=data, method=method)
    if data:
        r.add_header("Content-Type", "application/json")
    if accept:
        r.add_header("Accept", accept)
    try:
        with urllib.request.urlopen(r, timeout=5) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def next_event(device, after=0):
    return req("GET", f"/v1/device/next?device_id={device}&after={after}",
               accept="text/plain")


def fields(body):
    return body.decode().rstrip("\n").split("\n")


def post(body):
    return req("POST", "/v1/events", body)


def ack(device, event_id):
    return req("POST", "/v1/device/ack", {"device_id": device, "event_id": event_id})


def respond(device, event_id, choice):
    return req("POST", "/v1/device/respond",
               {"device_id": device, "event_id": event_id, "choice": choice})


class BridgeTest(unittest.TestCase):
    store_dir = None
    store_path = None
    server = None

    @classmethod
    def setUpClass(cls):
        cls.store_dir = tempfile.mkdtemp(prefix="gemmi-test-")
        cls.store_path = Path(cls.store_dir) / "events.jsonl"
        gb.BridgeHandler.store = gb.EventStore(cls.store_path)
        gb.BridgeHandler.token = None
        gb.BridgeHandler.lockdown_path = Path(cls.store_dir) / "bridge.lockdown"
        cls.server = ThreadingHTTPServer(("127.0.0.1", PORT), gb.BridgeHandler)
        cls.server.daemon_threads = True
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        req("DELETE", "/v1/events")
        # Establish dev1's join mark while the queue is empty so later posts
        # stream to it normally. Fresh-device join behaviour is tested
        # separately with dedicated device ids.
        next_event("dev1")

    def test_audio_disabled_returns_conflict(self):
        self.assertEqual(req("POST", "/v1/audio/test", {"sound": "ready"})[0], 409)

    def test_audio_hooks_and_test_endpoint(self):
        played = []
        audio = gb.AudioService("chimes", player=lambda path: played.append(path.read_bytes()))
        gb.BridgeHandler.audio = audio
        try:
            self.assertEqual(post({"kind": "ask", "agent": "audio-agent", "message": "Proceed?"})[0], 201)
            self.assertEqual(req("POST", "/v1/audio/test", {"sound": "ready"})[0], 202)
            self.assertEqual(req("POST", "/v1/audio/test", {"sound": []})[0], 400)
            self.assertEqual(req("POST", "/v1/audio/test", ["ready"])[0], 400)
            audio.jobs.join()
            self.assertEqual(len(played), 2)
            status, body = req("GET", "/health")
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)["audio"]["played"], 2)
        finally:
            gb.BridgeHandler.audio = None
            audio.close()

    def test_notify_deliver_and_ack(self):
        st, _ = post({"kind": "status", "agent": "a1", "message": "hello"})
        self.assertEqual(st, 201)
        st, body = next_event("dev1")
        self.assertEqual(st, 200)
        f = fields(body)
        self.assertEqual(f[1], "status")
        self.assertEqual(f[2], "a1")
        self.assertEqual(f[6], "hello")
        self.assertEqual(ack("dev1", int(f[0]))[0], 204)
        self.assertEqual(next_event("dev1", int(f[0]))[0], 204)

    def test_ask_fields_and_choices(self):
        post({"kind": "ask", "agent": "a2", "message": "deploy?",
              "choices": ["yes", "no"]})
        st, body = next_event("dev1")
        f = fields(body)
        self.assertEqual(f[1], "ask")
        self.assertEqual(f[4], "yes|no")
        self.assertEqual(f[5], "")

    def test_answer_recorded(self):
        post({"kind": "ask", "agent": "a3", "message": "go?",
              "choices": ["y", "n"]})
        st, body = next_event("dev1")
        event_id = int(fields(body)[0])
        self.assertEqual(respond("dev1", event_id, "y")[0], 200)
        st, body = req("GET", f"/v1/events/{event_id}")
        event = json.loads(body)
        self.assertEqual(event["response"]["choice"], "y")
        self.assertEqual(event["response"]["device_id"], "dev1")

    def test_respond_rejects_bad_choice_and_conflict(self):
        post({"kind": "ask", "agent": "a4", "message": "q",
              "choices": ["a", "b"]})
        st, body = next_event("dev1")
        event_id = int(fields(body)[0])
        self.assertEqual(respond("dev1", event_id, "nope")[0], 400)
        self.assertEqual(respond("dev1", event_id, "a")[0], 200)
        self.assertEqual(respond("dev1", event_id, "a")[0], 200)   # idempotent
        self.assertEqual(respond("dev1", event_id, "b")[0], 400)   # conflict

    def test_respond_requires_ask_kind(self):
        post({"kind": "status", "agent": "a5", "message": "m"})
        st, body = next_event("dev1")
        event_id = int(fields(body)[0])
        self.assertEqual(respond("dev1", event_id, "x")[0], 400)

    def test_open_ask_resurfaces_unconsumed(self):
        post({"kind": "status", "agent": "a6", "message": "old"})
        post({"kind": "ask", "agent": "a6", "message": "open?", "choices": ["x"]})
        # First delivery is the earliest unconsumed: the status.
        st, body = next_event("dev1")
        self.assertEqual(fields(body)[1], "status")
        ack("dev1", int(fields(body)[0]))
        st, body = next_event("dev1", int(fields(body)[0]))
        f = fields(body)
        self.assertEqual(f[1], "ask")
        # Simulated reboot: firmware resets its cursor to 0 and the same
        # unconsumed ask must resurface.
        st, body = next_event("dev1", 0)
        self.assertEqual(fields(body)[1], "ask")

    def test_fresh_device_joins_at_open_ask(self):
        post({"kind": "status", "agent": "a7", "message": "old backlog"})
        post({"kind": "ask", "agent": "a7", "message": "open", "choices": ["x"]})
        st, body = next_event("dev-new")
        self.assertEqual(fields(body)[1], "ask")
        self.assertEqual(fields(body)[6], "open")

    def test_fresh_device_without_open_ask_starts_empty(self):
        post({"kind": "status", "agent": "a7", "message": "backlog"})
        self.assertEqual(next_event("dev-fresh2")[0], 204)

    def test_validation(self):
        self.assertEqual(post({"kind": "bogus"})[0], 400)
        self.assertEqual(post({"kind": "ask", "agent": "a", "message": "q",
                               "choices": ["1", "2", "3", "4"]})[0], 400)
        self.assertEqual(post({"kind": "ask", "agent": "a"})[0], 400)
        r = urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/events",
                                   data=b"not json", method="POST")
        try:
            urllib.request.urlopen(r, timeout=5)
            self.fail("expected 400")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 400)
        self.assertEqual(ack("dev1", "abc")[0], 400)
        self.assertEqual(ack("dev1", True)[0], 400)   # JSON bool is not an id
        self.assertEqual(respond("dev1", 1, None)[0], 400)

    def test_events_listing(self):
        post({"kind": "status", "agent": "a8", "message": "m"})
        post({"kind": "ask", "agent": "a8", "message": "open?", "choices": ["x"]})
        st, body = req("GET", "/v1/events")
        self.assertEqual(st, 200)
        view = json.loads(body)
        self.assertEqual(len(view["events"]), 2)
        ask_id = view["events"][1]["id"]
        self.assertEqual(view["open_asks"], [ask_id])
        respond("dev1", ask_id, "x")
        st, body = req("GET", "/v1/events")
        self.assertEqual(json.loads(body)["open_asks"], [])

    def test_clear(self):
        post({"kind": "status", "agent": "a8", "message": "m"})
        st, body = next_event("dev1")
        ack("dev1", int(fields(body)[0]))
        self.assertEqual(req("DELETE", "/v1/events")[0], 204)
        self.assertEqual(next_event("dev1")[0], 204)

    def test_ids_monotonic_across_clear(self):
        post({"kind": "status", "agent": "a8", "message": "m"})
        st, body = next_event("dev1")
        first_id = int(fields(body)[0])
        req("DELETE", "/v1/events")
        # Reload the file like a restarted bridge would.
        store = gb.EventStore(self.store_path)
        self.assertGreater(store.next_id, first_id)
        st, body = post({"kind": "status", "agent": "a8", "message": "m2"})
        self.assertGreater(json.loads(body)["id"], first_id)

    def test_persistence_across_restart(self):
        post({"kind": "ask", "agent": "a9", "message": "survive?",
              "choices": ["yes", "no"]})
        st, body = next_event("dev9")
        event_id = int(fields(body)[0])
        respond("dev9", event_id, "yes")
        store = gb.EventStore(self.store_path)  # reload like a restart
        self.assertEqual(len(store.events), 1)
        self.assertIn(event_id, store.consumed["dev9"])
        self.assertEqual(store.events[0]["response"]["choice"], "yes")

    def test_concurrent_devices(self):
        post({"kind": "ask", "agent": "a10", "message": "both?", "choices": ["ok"]})
        st, b1 = next_event("devA")
        st, b2 = next_event("devB")
        eid = int(fields(b1)[0])
        self.assertEqual(int(fields(b2)[0]), eid)
        self.assertEqual(respond("devA", eid, "ok")[0], 200)
        # Same choice from another device is still idempotent.
        self.assertEqual(respond("devB", eid, "ok")[0], 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
