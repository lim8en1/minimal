#pragma once

// The interpreter driver, embedded and passed as `python -I -c <source>` --
// no temp files, no asset path resolution. Not `python -i`: the line REPL is
// unusable over pipes (prompt echo, stderr interleaving, no framing).
//
// PROTOCOL (one JSON value per line, ASCII-escaped by json.dumps):
//   parent -> child, fd 0:      {"id": "py-N", "code": "..."}
//   child -> parent, fd 3:      {"id": "...", "ok": bool, "result": "repr or ''",
//                                "stdout": "...", "stderr": "...",
//                                "result_json": "..." (serializable results only),
//                                "exc": "ValueError", "exc_msg": "..." (failures only)}
//   startup, once:              {"ready": true, "version": "...", "executable": "..."}
//   malformed request:          {"bad_request": "..."}  (no id; logged, skipped)
//   SIGINT with nothing running: {"idle_interrupted": true}  (see below)
//
// LIVE STREAM (fd 4, unframed raw bytes): every chunk written to the RUNNING
// request's stdout capture is ALSO written through to fd 4 immediately -- no
// buffering, no threads, no framing. At most one request runs at a time (the
// strict FIFO below), so the parent knows which request owns the stream
// without any framing protocol; the parent bounds its own retention. Bounded
// by nothing here BY DESIGN: a print storm streams through, the parent's
// capped ring is the backpressure boundary, and the completion capture below
// stays the authoritative record. stderr is NOT streamed: it carries only the
// final traceback, which arrives with the response anyway.
//
// USER STDIN (fd 5): sys.stdin reads here, set ONCE globally (not per
// request). A blocking input() parks the driver's single thread until the
// parent feeds a line -- the intended semantics, interruptible like any
// other blocking call (SIGINT raises KeyboardInterrupt out of the read).
// There is no EOF: the parent holds its write end for the process's life.
//
// SIGINT SEMANTICS: the parent's soft interrupt targets the RUNNING request
// (its per-request catch turns the KeyboardInterrupt into a failed response).
// A SIGINT landing while the loop is idle would otherwise kill the driver --
// the read itself raises -- so the read is wrapped and reported instead.
//
// The serialized queue IS the driver: one read loop, strict FIFO. A blocking
// request (an MSRPC call held in flight, a long download) queues every later
// request behind it -- that is the chosen semantics; the parent reports the
// queue depth in each reply's `pending` count.
//
// SABOTAGE HARDENING (model error, not malice -- the permission gate is the
// boundary, same ruling as bash):
//   - fd 0 and fd 3 are the ONLY protocol channels, and fd 3 is dup'd to a
//     private name at startup; the child's REAL fd 1/2 point at a diagnostics
//     pipe. Executed code writing to "the real stdout" cannot corrupt
//     responses.
//   - executed code runs with sys.stdin reading the user-stdin pipe (fd 5):
//     `input()` blocks until the parent feeds a line (python_input) instead
//     of eating the next request frame -- see USER STDIN above.
//   - stdout/stderr are captured per request into capped buffers
//     (_MINIMAL_PY_CAPTURE env), so a print-storm bounds the pipe line size
//     instead of growing memory without limit.
//   - KeyboardInterrupt is caught PER REQUEST: a soft interrupt (SIGINT from
//     the parent) fails the running request, and the driver survives.
//   - anything that does corrupt the channels ends as EOF on the parent's
//     reader, which marks the interpreter dead and auto-restarts on the next
//     call -- degradation, never a wedge.

namespace minimal::tools_python {

inline constexpr const char* kDriverSource = R"PY(import ast, io, json, os, sys, traceback

_MAX = int(os.environ.get("_MINIMAL_PY_CAPTURE", str(1 << 20)))


class _Cap(io.TextIOBase):
    def __init__(self, stream_fd=None):
        self._parts = []
        self._len = 0
        self.dropped = 0
        # Non-None on the stdout cap: every chunk also streams through fd 4,
        # synchronously with the write call. A dead/absent reader degrades to
        # dropped bytes, never an exception in user code.
        self._fd = stream_fd

    def writable(self):
        return True

    def write(self, s):
        if self._len < _MAX:
            take = min(_MAX - self._len, len(s))
            self._parts.append(s[:take])
            self._len += take
            if take < len(s):
                self.dropped += len(s) - take
        else:
            self.dropped += len(s)
        if self._fd is not None and s:
            try:
                os.write(self._fd, s.encode("utf-8", "replace"))
            except OSError:
                pass
        return len(s)

    def flush(self):
        pass

    def value(self):
        text = "".join(self._parts)
        if self.dropped:
            text += "\n[... %d bytes truncated ...]" % self.dropped
        return text


def _main():
    proto_in = os.fdopen(os.dup(0), "r", encoding="utf-8", errors="replace")
    # Dup'd to a private fd and the original closed -- see the SABOTAGE
    # HARDENING note above. Leaving fd 3 itself open under proto_out let
    # executed code (os.write(3, ...)) inject its own protocol lines
    # directly, bypassing the response channel's only author being this
    # driver.
    proto_out = os.fdopen(os.dup(3), "w", encoding="utf-8")
    os.close(3)
    stream_out = 4
    user_stdin = os.fdopen(5, "r", encoding="utf-8", errors="replace")

    def emit(obj):
        proto_out.write(json.dumps(obj))
        proto_out.write("\n")
        proto_out.flush()

    sys.stdin = user_stdin
    sys.path.insert(0, os.getcwd())

    G = {"__name__": "__main__"}

    emit({"ready": True, "version": sys.version.split()[0], "executable": sys.executable})

    while True:
        try:
            line = proto_in.readline()
        except KeyboardInterrupt:
            # A soft interrupt with no request running: spend the signal here
            # and keep serving, so the parent can report "nothing was running"
            # instead of losing the interpreter.
            emit({"idle_interrupted": True})
            continue
        except EOFError:
            break
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as exc:
            emit({"bad_request": str(exc)})
            continue
        code = req.get("code")
        if not isinstance(code, str):
            emit({"bad_request": "missing 'code'"})
            continue
        out, err = _Cap(stream_out), _Cap()
        old = (sys.stdout, sys.stderr, sys.stdin)
        sys.stdout, sys.stderr = out, err
        ok = True
        result = ""
        value = None
        has_value = False
        exc_type = None
        exc_msg = None
        try:
            # A trailing expression's value is the result -- even when it
            # follows statements (which a plain eval-compile rejects). Split on
            # the ast: exec the statement prefix, eval the trailing Expr node.
            tree = ast.parse(code)
            trailing = None
            if tree.body and isinstance(tree.body[-1], ast.Expr):
                trailing = tree.body[-1].value
                tree.body = tree.body[:-1]
            if tree.body:
                exec(compile(tree, "<python-tool>", "exec"), G)
            if trailing is not None:
                value = eval(compile(ast.Expression(trailing), "<python-tool>", "eval"), G)
                has_value = True
                result = repr(value)
        except BaseException as e:
            # KeyboardInterrupt included: a soft interrupt fails THIS request
            # only -- the interpreter and its state survive.
            ok = False
            exc_type = type(e).__name__
            exc_msg = str(e)
            err.write(traceback.format_exc())
        finally:
            sys.stdout, sys.stderr, sys.stdin = old
        response = {"id": req.get("id"), "ok": ok, "result": result,
                    "stdout": out.value(), "stderr": err.value()}
        if exc_type is not None:
            response["exc"] = exc_type
            response["exc_msg"] = exc_msg
        if has_value:
            # The structured return: JSON when the value serializes cheaply,
            # absent otherwise (the repr above is the fallback display).
            try:
                rendered = json.dumps(value)
                if len(rendered) <= _MAX:
                    response["result_json"] = rendered
            except Exception:
                pass
        emit(response)


try:
    _main()
except BaseException:
    try:
        sys.stderr.write("python tool driver failed:\n" + traceback.format_exc())
    except BaseException:
        pass
)PY";

}  // namespace minimal::tools_python
