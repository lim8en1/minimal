#pragma once

#include <memory>

#include <spdlog/spdlog.h>

namespace minimal::common {

// One process-wide logging handle that tools and plugins share. Deliberately
// thin: it forwards to spdlog's CURRENT default logger on every call, so the
// sink (console default, the rotating file from settings.json's `logging.*`)
// can be swapped by ConfigureLogging without touching a single call site.
// Lines logged before ConfigureLogging runs (config-dir resolution, settings
// parsing) go to spdlog's built-in console logger; everything after follows
// the swap.
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
    // default_logger_raw(), not default_logger(): the latter takes spdlog's
    // registry-wide mutex and copies a shared_ptr on every call, ahead of the
    // level check, serializing every thread on output that is then discarded.
    // The raw pointer is safe because the swap happens exactly once, while the
    // process is still single-threaded (see ConfigureLogging); no thread can
    // be mid-call on the logger being replaced.
    static spdlog::logger* current() { return spdlog::default_logger_raw(); }
};

// The process-wide instance. Stateless: what it logs TO follows whatever
// spdlog::set_default_logger() has installed.
Logger& DefaultLogger();

}  // namespace minimal::common
