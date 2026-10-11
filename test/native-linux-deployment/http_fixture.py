"""Loopback HTTP work with request handshakes and explicitly released replies."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading

from common import require


class LocalHttp:
    def __init__(self, gate):
        self.gate = gate
        self.lock = threading.Lock()
        self.received = {}
        self.release_events = {}
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                with owner.lock:
                    received = owner.received.setdefault(self.path, threading.Event())
                    release = owner.release_events.get(self.path)
                owner.gate.event("http.request", path=self.path)
                received.set()
                if release is not None:
                    release.wait(60)
                try:
                    self.send_response(204)
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    # Killing/stopping the daemon deliberately cancels held
                    # clients; that disconnect is an expected fixture outcome.
                    owner.gate.event("http.client.closed", path=self.path)

            def log_message(self, format_string, *arguments):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def payload(self, name, hold=False):
        path = f"/{name}"
        with self.lock:
            self.received[path] = threading.Event()
            if hold:
                self.release_events[path] = threading.Event()
        return {"url": f"http://127.0.0.1:{self.server.server_port}{path}"}

    def wait(self, name):
        with self.lock:
            event = self.received[f"/{name}"]
        require(event.wait(15), f"No HTTP request for {name}")

    def release(self, name):
        with self.lock:
            self.release_events[f"/{name}"].set()

    def close(self):
        with self.lock:
            for event in self.release_events.values():
                event.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        require(not self.thread.is_alive(), "HTTP fixture did not stop")

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.close()
