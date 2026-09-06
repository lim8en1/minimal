#pragma once

// The interpreter driver, embedded and passed as `python -I -c <source>` --
// no temp files, no asset path resolution. Not `python -i`: the line REPL is
// unusable over pipes (prompt echo, stderr interleaving, no framing).
//
// PROTOCOL (one JSON value per line, ASCII-escaped by json.dumps):
//   parent -> child, fd 0:      {"id": "py-N", "code": "..."}
//   child -> parent, fd 3:      {"id": "...", "ok": bool, "result": "repr or ''",
//                                "stdout": "...", "stderr": "..."}
//   startup, once:              {"ready": true, "version": "...", "executable": "..."}
//   malformed request:          {"bad_request": "..."}  (no id; logged, skipped)
//   SIGINT with nothing running: {"idle_interrupted": true}  (see below)
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
//   - executed code runs with sys.stdin replaced by an object whose every
//     read raises EOFError, so `input()` fails immediately instead of eating
//     the next request frame.
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
    def __init__(self):
        self._parts = []
        self._len = 0
        self.dropped = 0

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
        return len(s)

    def flush(self):
        pass

    def value(self):
        text = "".join(self._parts)
        if self.dropped:
            text += "\n[... %d bytes truncated ...]" % self.dropped
        return text


class _NoStdin:
    def _dead(self, *args, **kwargs):
        raise EOFError("stdin is reserved by the python tool protocol")

    read = readline = readlines = __iter__ = _dead
    __next__ = _dead

    def fileno(self):
        raise EOFError("stdin is reserved by the python tool protocol")


def _main():
    proto_in = os.fdopen(os.dup(0), "r", encoding="utf-8", errors="replace")
    proto_out = os.fdopen(3, "w", encoding="utf-8")

    def emit(obj):
        proto_out.write(json.dumps(obj))
        proto_out.write("\n")
        proto_out.flush()

    sys.stdin = _NoStdin()
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
        out, err = _Cap(), _Cap()
        old = (sys.stdout, sys.stderr, sys.stdin)
        sys.stdout, sys.stderr, sys.stdin = out, err, _NoStdin()
        ok = True
        result = ""
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
                result = repr(eval(compile(ast.Expression(trailing), "<python-tool>", "eval"), G))
        except BaseException:
            # KeyboardInterrupt included: a soft interrupt fails THIS request
            # only -- the interpreter and its state survive.
            ok = False
            err.write(traceback.format_exc())
        finally:
            sys.stdout, sys.stderr, sys.stdin = old
        emit({"id": req.get("id"), "ok": ok, "result": result,
              "stdout": out.value(), "stderr": err.value()})


try:
    _main()
except BaseException:
    try:
        sys.stderr.write("python tool driver failed:\n" + traceback.format_exc())
    except BaseException:
        pass
)PY";

}  // namespace minimal::tools_python
