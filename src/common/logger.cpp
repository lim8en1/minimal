#include "minimal/common/logger.hpp"

namespace minimal::common {

Logger& DefaultLogger() {
    // spdlog::default_logger() creates a multi-threaded color console logger on
    // first use. Future logging-settings wiring can call spdlog::set_default_logger()
    // with the rotating-file sink BEFORE this runs; after first use the instance
    // is fixed for the process lifetime (config is read once at startup anyway).
    static Logger instance(spdlog::default_logger());
    return instance;
}

}  // namespace minimal::common
