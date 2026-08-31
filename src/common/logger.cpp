#include "minimal/common/logger.hpp"

namespace minimal::common {

Logger& DefaultLogger() {
    // Stateless: the returned handle resolves spdlog's CURRENT default logger
    // on every call (see Logger in the header). ConfigureLogging swaps that
    // default for the console+file logger early in startup, and every later
    // log line -- including those logged before the swap -- simply follows.
    static Logger instance;
    return instance;
}

}  // namespace minimal::common
