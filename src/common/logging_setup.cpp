#include "minimal/common/logging_setup.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace minimal::common {
namespace {

spdlog::level::level_enum ParseLevel(const std::string& name, std::string& error) {
    if (name == "trace") return spdlog::level::trace;
    if (name == "debug") return spdlog::level::debug;
    if (name == "info") return spdlog::level::info;
    if (name == "warn" || name == "warning") return spdlog::level::warn;
    if (name == "error") return spdlog::level::err;
    if (name == "critical") return spdlog::level::critical;
    if (name == "off") return spdlog::level::off;
    // Reported rather than silently defaulted: a misspelled level is exactly the
    // kind of thing that looks configured and does nothing.
    error = "unknown logging.level '" + name + "'; using info";
    return spdlog::level::info;
}

}  // namespace

std::string ConfigureLogging(const LoggingOptions& options) {
    std::string error;
    const auto level = ParseLevel(options.level, error);

    std::vector<spdlog::sink_ptr> sinks;
    // The console sink stays by default: a server started in a terminal should
    // keep printing there; the file is an addition, not a replacement. The
    // client opts out (see LoggingOptions::console).
    if (options.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    if (!options.path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(options.path).parent_path(), ec);
        try {
            // spdlog's own rotation, per ARCHITECTURE.md: "no custom rotation
            // code". Sizes are clamped to something sane rather than trusted --
            // max_size_mb: 0 would make every message rotate.
            const std::size_t max_bytes =
                static_cast<std::size_t>(options.max_size_mb > 0 ? options.max_size_mb : 10) *
                1024 * 1024;
            const std::size_t max_files =
                static_cast<std::size_t>(options.max_files > 0 ? options.max_files : 5);
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                options.path, max_bytes, max_files));
        } catch (const std::exception& e) {
            // A log file we cannot open must not stop the server: this
            // degrades rather than fails. With the console sink opted out
            // that would mean dropping everything, so the console sink comes
            // back for the degraded case despite the opt-out.
            if (sinks.empty()) {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            }
            error = "cannot open logging.path '" + options.path + "': " + e.what();
        }
    }

    auto logger = std::make_shared<spdlog::logger>("minimal", sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
    return error;
}

}  // namespace minimal::common
