#include "minimal/common/read_file.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace minimal::common {

std::string ReadFileOrEmpty(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream in(path);
    if (!in) return {};

    // Two failure classes used to be silent:
    //   - unbounded size: a giant (or hostile) file was slurped whole by
    //     every caller, and a cap request must fail LOUDLY, not truncate;
    //   - I/O error mid-read: a badbit read yielded a silently truncated
    //     string indistinguishable from a short file. eofbit/failbit are
    //     normal here (they are how a short final read and end-of-file
    //     announce themselves), so only badbit -- a genuine I/O error -- is
    //     fatal.
    //
    // Read in bounded chunks rather than `buffer << in.rdbuf()`: the cap used
    // to be checked AFTER the entire file was already in memory, so it bounded
    // nothing at all -- a multi-GB file produced bad_alloc or an OOM kill
    // instead of the error below. Stopping one byte PAST the cap is what
    // distinguishes "too big" from "exactly at the cap", and is the most this
    // ever allocates. Deliberately not std::filesystem::file_size: that
    // re-resolves the path (a TOCTOU against the open fd) and answers 0 for
    // things that are not regular files, which would silently return "".
    std::string contents;
    char chunk[64 * 1024];
    while (contents.size() <= max_bytes) {
        const auto want = std::min(sizeof(chunk), max_bytes + 1 - contents.size());
        in.read(chunk, static_cast<std::streamsize>(want));
        const auto got = in.gcount();
        if (got <= 0) break;
        contents.append(chunk, static_cast<std::size_t>(got));
    }

    if (in.bad()) {
        throw std::runtime_error("read failed (" + path.string() + "): I/O error");
    }
    if (contents.size() > max_bytes) {
        throw std::runtime_error("file exceeds the " + std::to_string(max_bytes) +
                                 " byte read cap: " + path.string());
    }
    return contents;
}

}  // namespace minimal::common
