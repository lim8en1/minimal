#include "minimal/client/config_file_io.hpp"

#include <fstream>
#include <iterator>

#include "minimal/common/atomic_write.hpp"

namespace minimal::client {

bool TryReadConfigFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool WriteConfigFileAtomic(const std::filesystem::path& path, std::string_view contents) {
    // The shared writer: a same-directory temp file, fsync, rename -- and a
    // unique temp name, so two clients saving at once cannot collide.
    return common::write_file(path, contents).ok;
}

}  // namespace minimal::client
