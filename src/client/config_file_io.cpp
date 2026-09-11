#include "minimal/client/config_file_io.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace minimal::client {

bool TryReadConfigFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool WriteConfigFileAtomic(const std::filesystem::path& path, std::string_view contents) {
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << contents;
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

}  // namespace minimal::client
