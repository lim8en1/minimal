#include "minimal/common/read_file.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace minimal::common {

std::string ReadFileOrEmpty(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    // Two failure classes used to be silent:
    //   - unbounded size: a giant (or hostile) file was slurped whole by
    //     every caller, and a cap request must fail LOUDLY, not truncate;
    //   - I/O error mid-read: a badbit read yielded a silently truncated
    //     string indistinguishable from a short file. eofbit/failbit after
    //     an rdbuf insertion are normal (an empty file sets failbit!), so
    //     only badbit -- a genuine I/O error -- is fatal here.
    if (in.bad()) {
        throw std::runtime_error("read failed (" + path.string() + "): I/O error");
    }
    const std::string contents = buffer.str();
    if (contents.size() > max_bytes) {
        throw std::runtime_error("file exceeds the " + std::to_string(max_bytes) +
                                 " byte read cap: " + path.string());
    }
    return contents;
}

}  // namespace minimal::common
