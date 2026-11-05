#pragma once

// The interpreter driver, embedded and passed as `python -I -c <source>` --
// no temp files, no asset path resolution. Not `python -i`: the line REPL is
// unusable over pipes (prompt echo, stderr interleaving, no framing).
//
// PROTOCOL (one JSON value per line, ASCII-escaped by json.dumps):
//   parent -> child, fd 0:      {"id": "py-N", "code": "..."}
//   child -> parent, fd 3:      {"id": "...", "ok": bool, "result": "repr or ''",
//                                "stdout": "...", "stderr": "...",
//                                "streamed": <bytes written to fd 4 for this request>,
//                                "exc": "ValueError", "exc_msg": "..." (failures only)}
//   startup, once:              {"ready": true, "version": "...", "executable": "..."}
//   malformed request:          {"bad_request": "...", "id": "..." (when recoverable)}
//   SIGINT with nothing running: {"idle_interrupted": true}  (see below)
//
// Every free-text field (stdout, stderr, result, exc_msg) is capped at _MAX
// characters; the parent derives its protocol-line limit from the same
// setting (python_interpreter.cpp MaxLineBytes).
//
// LIVE STREAM (fd 4, unframed raw bytes): every chunk written to the RUNNING
// request's stdout capture is ALSO written through to fd 4 immediately. At
// most one request runs at a time (the strict FIFO below), and the response's
// `streamed` count tells the parent exactly where one request's bytes end and
// the next one's begin -- a reader that falls behind cannot misattribute a
// finished request's tail to its successor. Bounded by nothing here BY
// DESIGN: the parent's capped ring is the backpressure boundary, and the
// completion capture stays the authoritative record. stderr is NOT streamed:
// it carries only the final traceback, which arrives with the response.
//
// USER STDIN (fd 5): sys.stdin reads here, set ONCE globally (not per
// request). A blocking input() parks the driver's single thread until the
// parent feeds a line -- interruptible like any other blocking call.
// There is no EOF: the parent holds its write end for the process's life.
//
// SIGINT SEMANTICS: one handler, one state variable -- no per-window patches.
//   USER  (user code running): raise KeyboardInterrupt; the request fails and
//         the interpreter survives.
//   IDLE  (parked in the request read): emit {"idle_interrupted"} and keep
//         reading -- the parent reports "nothing was running".
//   BOOK  (driver bookkeeping: parsing a request, assembling a reply): never
//         raise. Remembered, then spent at the next state change: entering
//         USER fails that request at once (it was the parent's queue head),
//         entering IDLE reports it as idle. A reply emitted in between
//         clears it -- that completion IS the reaction to the signal.
// USER is entered and left only inside the per-request try, so a raise can
// never escape the loop. The handler is re-installed after every request in
// case executed code replaced it.
//
// The serialized queue IS the driver: one read loop, strict FIFO. A blocking
// request (an MSRPC call held in flight, a long download) queues every later
// request behind it -- that is the chosen semantics.
//
// SABOTAGE HARDENING (model error, not malice -- the permission gate is the
// boundary, same ruling as bash):
//   - fd 0 and fd 3 are the ONLY protocol channels, and fd 3 is dup'd to a
//     private name at startup; the child's REAL fd 1/2 point at a diagnostics
//     pipe the parent attributes to the running request (subprocess output).
//   - executed code runs with sys.stdin reading the user-stdin pipe (fd 5).
//   - stdout/stderr are captured per request into capped buffers
//     (_MINIMAL_PY_CAPTURE env), so a print-storm bounds the line size.
//   - anything that does corrupt the channels ends as a kill on the parent's
//     side and an auto-restart on the next call -- degradation, never a wedge.

namespace minimal::tools_python {

inline constexpr const char* kDriverSource = R"PY(import ast, io, json, linecache, os, signal, sys, threading, traceback

_MAX = int(os.environ.get("_MINIMAL_PY_CAPTURE", str(1 << 20)))

_IDLE, _BOOK, _USER = 0, 1, 2
_st = {"mode": _BOOK, "pending": False, "emit": None}


def _clean(text):
    # Lone surrogates (surrogateescape'd filenames, broken decodes) would
    # reach the parent as unpaired \udXXX escapes, which its JSON parser
    # rejects -- and a rejected line reads as corruption. Replace them.
    return text.encode("utf-8", "replace").decode("utf-8")


def _cap(text):
    if len(text) > _MAX:
        text = text[:_MAX] + "\n[... %d chars truncated ...]" % (len(text) - _MAX)
    return _clean(text)


def _on_sigint(signum, frame):
    mode = _st["mode"]
    if mode == _USER:
        raise KeyboardInterrupt
    if mode == _IDLE:
        try:
            _st["emit"]({"idle_interrupted": True})
        except BaseException:
            pass
        return
    _st["pending"] = True


class _Cap(io.TextIOBase):
    encoding = "utf-8"
    errors = "replace"

    def __init__(self, stream_fd=None):
        self._parts = []
        self._len = 0
        self.dropped = 0
        self.streamed = 0
        # Non-None on the stdout cap: every chunk also streams through fd 4.
        # A dead/absent reader degrades to dropped bytes, never an exception
        # in user code. Locked: user threads print through the same object.
        self._fd = stream_fd
        self._lock = threading.Lock()

    def writable(self):
        return True

    def isatty(self):
        return False

    def write(self, s):
        if not isinstance(s, str):
            raise TypeError("write() argument must be str, not %s" % type(s).__name__)
        with self._lock:
            if self._len < _MAX:
                take = min(_MAX - self._len, len(s))
                self._parts.append(s[:take])
                self._len += take
                self.dropped += len(s) - take
            else:
                self.dropped += len(s)
            if self._fd is not None and s:
                data = s.encode("utf-8", "replace")
                try:
                    while data:
                        n = os.write(self._fd, data)
                        self.streamed += n
                        data = data[n:]
                except OSError:
                    self._fd = None
        return len(s)

    def flush(self):
        pass

    def value(self):
        text = "".join(self._parts)
        if self.dropped:
            text += "\n[... %d chars truncated ...]" % self.dropped
        return text


def _main():
    proto_in = os.fdopen(os.dup(0), "r", encoding="utf-8", errors="replace")
    # fd 0 is REPLACED with /dev/null, not left as the request pipe: code
    # doing raw fd I/O (subprocess.run() without stdin=, os.system) inherits
    # the OS-level fd, and a live request pipe there would let a child
    # consume or block on the next queued request frame.
    devnull_in = os.open(os.devnull, os.O_RDONLY)
    os.dup2(devnull_in, 0)
    os.close(devnull_in)
    # Dup'd to a private fd and the original closed: executed code writing
    # os.write(3, ...) must not be able to inject protocol lines.
    proto_out = os.fdopen(os.dup(3), "w", encoding="utf-8")
    os.close(3)
    stream_out = 4
    user_stdin = os.fdopen(5, "r", encoding="utf-8", errors="replace")

    def emit(obj):
        proto_out.write(json.dumps(obj))
        proto_out.write("\n")
        proto_out.flush()

    _st["emit"] = emit
    signal.signal(signal.SIGINT, _on_sigint)
    driver_file = _main.__code__.co_filename

    sys.stdin = user_stdin
    sys.path.insert(0, os.getcwd())

    G = {"__name__": "__main__"}

    emit({"ready": True, "version": sys.version.split()[0], "executable": sys.executable})

    while True:
        # Spend a remembered SIGINT in BOOK (a signal during the emit only
        # sets pending), then enter IDLE -- and look again: one that landed
        # between the check and the switch was only remembered, and the read
        # below would sit on it until the next request.
        while True:
            if _st["pending"]:
                _st["pending"] = False
                emit({"idle_interrupted": True})
            _st["mode"] = _IDLE
            if not _st["pending"]:
                break
            _st["mode"] = _BOOK
        line = proto_in.readline()
        _st["mode"] = _BOOK
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
        rid = req.get("id") if isinstance(req, dict) else None
        code = req.get("code") if isinstance(req, dict) else None
        if not isinstance(rid, str) or not isinstance(code, str):
            bad = {"bad_request": "missing 'id' or 'code'"}
            if isinstance(rid, str):
                bad["id"] = rid
            emit(bad)
            continue

        out, err = _Cap(stream_out), _Cap()
        old = (sys.stdout, sys.stderr, sys.stdin)
        ok = True
        result = ""
        exc_type = None
        exc_msg = None
        # A per-request filename registered in linecache: tracebacks show the
        # offending source lines, and inspect.getsource() works on functions
        # defined by earlier requests.
        fname = "<%s>" % rid
        linecache.cache[fname] = (len(code), None, code.splitlines(True), fname)
        try:
            try:
                sys.stdout, sys.stderr = out, err
                _st["mode"] = _USER
                if _st["pending"]:
                    _st["pending"] = False
                    raise KeyboardInterrupt
                # A trailing expression's value is the result -- even when it
                # follows statements. Split on the ast: exec the statement
                # prefix, eval the trailing Expr node.
                tree = ast.parse(code, fname)
                trailing = None
                if tree.body and isinstance(tree.body[-1], ast.Expr):
                    trailing = tree.body[-1].value
                    tree.body = tree.body[:-1]
                if tree.body:
                    exec(compile(tree, fname, "exec"), G)
                if trailing is not None:
                    value = eval(compile(ast.Expression(trailing), fname, "eval"), G)
                    # None is not a result (the REPL rule): a trailing
                    # print(...) must not append "ok: None" to every reply.
                    if value is not None:
                        result = repr(value)
            finally:
                _st["mode"] = _BOOK
                sys.stdout, sys.stderr, sys.stdin = old
        except BaseException as e:
            ok = False
            exc_type = type(e).__name__
            try:
                exc_msg = str(e)
            except BaseException:
                exc_msg = "<unprintable %s>" % exc_type
            # Driver frames are noise in a user's traceback: start at the
            # first frame of the executed code.
            tb = e.__traceback__
            while tb is not None and tb.tb_frame.f_code.co_filename == driver_file:
                tb = tb.tb_next
            if isinstance(e, SyntaxError):
                tb = None
            try:
                err.write("".join(traceback.format_exception(type(e), e, tb)))
            except BaseException:
                err.write("%s: %s\n" % (exc_type, exc_msg))
        response = {"id": rid, "ok": ok, "result": _cap(result),
                    "stdout": _clean(out.value()), "stderr": _clean(err.value()),
                    "streamed": out.streamed}
        if exc_type is not None:
            response["exc"] = exc_type
            response["exc_msg"] = _cap(exc_msg)
        # A reply is the reaction to any signal that landed since the
        # request started: the completion is what the parent waits for.
        _st["pending"] = False
        emit(response)
        if signal.getsignal(signal.SIGINT) is not _on_sigint:
            signal.signal(signal.SIGINT, _on_sigint)


try:
    _main()
except BaseException:
    try:
        sys.stderr.write("python tool driver failed:\n" + traceback.format_exc())
    except BaseException:
        pass
)PY";

}  // namespace minimal::tools_python
