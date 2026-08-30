#pragma once

#include <memory>

#include <spdlog/spdlog.h>

namespace minimal::common {

// One process-wide logging handle that tools and plugins share. Deliberately
// thin: it forwards to an spdlog logger so the sink (console today, the
// rotating file from settings.json's `logging.*` later) can change without
// touching a single call site.
class Logger {
public:
    explicit Logger(std::shared_ptr<spdlog::logger> impl) : impl_(std::move(impl)) {}

    template <typename... Args>
    void debug(fmt::format_string<Args...> format, Args&&... args) const {
        if (impl_) impl_->debug(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info(fmt::format_string<Args...> format, Args&&... args) const {
        if (impl_) impl_->info(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(fmt::format_string<Args...> format, Args&&... args) const {
        if (impl_) impl_->warn(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error(fmt::format_string<Args...> format, Args&&... args) const {
        if (impl_) impl_->error(format, std::forward<Args>(args)...);
    }

private:
    std::shared_ptr<spdlog::logger> impl_;
};

// The process-wide instance, created once on first use with spdlog's default
// console logger. When the `logging.*` settings land they will configure the
// sink this wraps (rotating file) -- references to the Logger itself stay valid
// either way.
Logger& DefaultLogger();

}  // namespace minimal::common
