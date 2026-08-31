#pragma once

#include <memory>

#include <spdlog/spdlog.h>

namespace minimal::common {

// One process-wide logging handle that tools and plugins share. Deliberately
// thin: it forwards to spdlog's CURRENT default logger on every call, so the
// sink (console default, the rotating file from settings.json's `logging.*`)
// can be swapped by ConfigureLogging without touching a single call site.
//
// The indirection is load-bearing. An earlier version pinned
// spdlog::default_logger() on first use -- and first use happens during
// plugin-config parsing, BEFORE ConfigureLogging runs -- so the file sink was
// wired into a logger nothing used anymore, and logging.path was silently
// inert. Resolving per call is what makes the swap real.
class Logger {
public:
    Logger() = default;

    template <typename... Args>
    void debug(fmt::format_string<Args...> format, Args&&... args) const {
        current()->debug(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info(fmt::format_string<Args...> format, Args&&... args) const {
        current()->info(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(fmt::format_string<Args...> format, Args&&... args) const {
        current()->warn(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error(fmt::format_string<Args...> format, Args&&... args) const {
        current()->error(format, std::forward<Args>(args)...);
    }

private:
    static std::shared_ptr<spdlog::logger> current() { return spdlog::default_logger(); }
};

// The process-wide instance. Stateless: what it logs TO follows whatever
// spdlog::set_default_logger() has installed.
Logger& DefaultLogger();

}  // namespace minimal::common
