#!/usr/bin/env python3
"""The plugin-boundary contract, run against the real binaries.

Core plus one provider plugin is a working harness, and every other bundled
plugin can be turned off without breaking startup or a plain text turn:

  1. no plugins at all       -> --check fails (nothing can serve a model)
  2. provider-openai only    -> --check passes and a headless turn completes
  3. each bundled plugin off -> --check passes and a headless turn completes
     (the rest at their shipped default; provider-openai is skipped -- it is
     the one provider this config names)

The provider is a fake OpenAI-compatible endpoint served from this process.
Run as:
  plugin_matrix_test.py <minimal-server> <minimal> <bundled-plugin-dir> <seed-script> <bundled-dir>
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLY = "matrix-pong"
PLUGIN_SUFFIXES = (".so", ".dylib")


class FakeOpenAI(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        body = json.dumps({"object": "list", "data": [{"id": "m", "object": "model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length) or b"{}")
        message = {"role": "assistant", "content": REPLY}
        usage = {"prompt_tokens": 5, "completion_tokens": 1, "total_tokens": 6}
        if request.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            chunks = [
                {"id": "c1", "object": "chat.completion.chunk", "model": "m",
                 "choices": [{"index": 0, "delta": message, "finish_reason": None}]},
                {"id": "c1", "object": "chat.completion.chunk", "model": "m",
                 "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": usage},
            ]
            for chunk in chunks:
                self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
            self.wfile.write(b"data: [DONE]\n\n")
            return
        body = json.dumps({"id": "c1", "object": "chat.completion", "model": "m",
                           "choices": [{"index": 0, "message": message, "finish_reason": "stop"}],
                           "usage": usage}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def bundled_plugins(plugin_dir):
    """Basename -> marker-says-disabled, for every bundled server plugin file."""
    found = {}
    for dirpath, _, files in os.walk(plugin_dir):
        for name in files:
            if name.endswith(PLUGIN_SUFFIXES):
                stem = os.path.splitext(name)[0]
                found[name] = os.path.exists(os.path.join(dirpath, stem + ".disabled-by-default"))
    return found


class Arena:
    def __init__(self, args, port):
        self.server, self.client, self.plugin_dir, self.seed_script, self.bundled_dir = (
            os.path.abspath(arg) for arg in args)
        self.port = port
        self.root = tempfile.mkdtemp(prefix="minimal-matrix-")
        self.config = os.path.join(self.root, "config")
        self.env = dict(os.environ, MINIMAL_CONFIG_DIR=self.config,
                        XDG_DATA_HOME=os.path.join(self.root, "data"),
                        XDG_STATE_HOME=os.path.join(self.root, "state"),
                        XDG_RUNTIME_DIR=os.path.join(self.root, "run"))
        os.makedirs(self.config)
        subprocess.run(["cmake", f"-DMINIMAL_SEED_SOURCE={self.bundled_dir}",
                        f"-DMINIMAL_SEED_CONFIG_DIR={self.config}", "-P", self.seed_script],
                       check=True, capture_output=True)
        self.write("providers.json", {"providers": {"local": {
            "backend": "openai", "url": f"http://127.0.0.1:{port}/v1", "models": {"m": {}}}}})
        settings = json.load(open(os.path.join(self.config, "settings.json")))
        settings["default_model"] = "local/m"
        self.write("settings.json", settings)

    def write(self, name, doc):
        with open(os.path.join(self.config, name), "w") as handle:
            json.dump(doc, handle)

    def set_plugins(self, enabled):
        # cost-tracker fetches pricing at startup; point it at a closed port
        # so the matrix never touches the network.
        offline = {"rate_sources": [{"format": "litellm", "url": "http://127.0.0.1:9/x"}],
                   "rates_fetch_timeout_ms": 200}
        os.makedirs(os.path.join(self.config, "plugins.d"), exist_ok=True)
        self.write(os.path.join("plugins.d", "cost-tracker.json"), offline)
        entries = [{"file": name, "provenance": "bundled", "enabled": on}
                   for name, on in sorted(enabled.items())]
        self.write("plugins.json", {"plugins": entries})

    def check(self):
        result = subprocess.run([self.server, "--check"], env=self.env, capture_output=True,
                                text=True, timeout=60)
        return result.returncode, result.stdout + result.stderr

    def turn(self):
        socket = os.path.join(self.root, "s.sock")
        server = subprocess.Popen([self.server, "--listen", socket], env=self.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.time() + 30
            while not os.path.exists(socket):
                if server.poll() is not None or time.time() > deadline:
                    return False, "server did not start:\n" + (server.stdout.read() or "")
                time.sleep(0.05)
            result = subprocess.run(
                [self.client, "-p", "hi", "--server", socket, "--no-autospawn",
                 "--dangerously-bypass"],
                env=self.env, cwd=self.root, capture_output=True, text=True, timeout=60)
            ok = result.returncode == 0 and REPLY in result.stdout
            output = result.stdout + result.stderr
        finally:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=20)
            except subprocess.TimeoutExpired:
                server.kill()
        if not ok:
            # The server's side of a failed turn (a crash report, a fatal).
            output += "\n--- server ---\n" + (server.stdout.read() or "")[-6000:]
        return ok, output

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


def main(argv):
    if len(argv) != 6:
        print(__doc__)
        return 2
    http = ThreadingHTTPServer(("127.0.0.1", 0), FakeOpenAI)
    threading.Thread(target=http.serve_forever, daemon=True).start()
    port = http.server_address[1]

    plugins = bundled_plugins(argv[3])
    provider = next(name for name in plugins if name.startswith("provider-openai."))
    failures = []

    def case(label, enabled, expect_ok, run_turn, expect_text=None):
        arena = Arena(argv[1:], port)
        failed = len(failures)
        try:
            arena.set_plugins(enabled)
            code, output = arena.check()
            if (code == 0) != expect_ok:
                failures.append(f"{label}: --check exited {code}, expected "
                                f"{'success' if expect_ok else 'failure'}\n{output}")
            elif expect_text is not None and expect_text not in output:
                failures.append(f"{label}: --check output lacks '{expect_text}'\n{output}")
            elif run_turn:
                ok, output = arena.turn()
                if not ok:
                    failures.append(f"{label}: headless turn failed\n{output}")
        finally:
            arena.close()
        print(f"{'ok' if len(failures) == failed else 'FAIL'}: {label}")

    # Nothing provides the "openai" backend providers.json names.
    case("no plugins", {name: False for name in plugins}, expect_ok=False, run_turn=False,
         expect_text="openai")
    case("provider-openai only", {name: name == provider for name in plugins},
         expect_ok=True, run_turn=True)
    defaults = {name: not disabled for name, disabled in plugins.items()}
    for name in sorted(plugins):
        if name == provider or not defaults[name]:
            continue
        case(f"without {name}", dict(defaults, **{name: False}), expect_ok=True, run_turn=True)

    http.shutdown()
    for failure in failures:
        print("FAILED " + failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
