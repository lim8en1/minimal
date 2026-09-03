#include "minimal/common/read_file.hpp"

#include <fstream>
#include <sstream>

namespace minimal::common {

std::string ReadFileOrEmpty(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace minimal::common
