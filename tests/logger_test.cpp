#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "minimal/common/logger.hpp"
#include "minimal/common/logging_setup.hpp"

namespace fs = std::filesystem;

TEST_CASE("ConfigureLogging's swap is visible through DefaultLogger", "[logging][invariant]") {
    // The logging.path contract: after ConfigureLogging installs the file sink,
    // lines logged through DefaultLogger() reach the file. The earlier
    // DefaultLogger() pinned spdlog's default logger ON FIRST USE -- and first
    // use happens during plugin-config parsing, before ConfigureLogging runs --
    // so the file sink was wired into a logger nobody logged to anymore, and
    // logging.path was silently inert while the console kept working.
    //
    // This test reproduces that order: log through DefaultLogger() FIRST
    // (pinning, in the old code), then ConfigureLogging, then log again. The
    // second line must land in the file.
    fs::path log = fs::temp_directory_path() /
                   ("minimal-logger-test-" + std::to_string(::getpid()) + ".log");
    std::error_code ec;
    fs::remove(log, ec);

    minimal::common::DefaultLogger().info("before the swap");  // first use, pre-configure

    const std::string problem = minimal::common::ConfigureLogging(
        {.level = "info", .path = log.string(), .max_size_mb = 5, .max_files = 1});
    REQUIRE(problem.empty());
    minimal::common::DefaultLogger().warn("after the swap");  // warn flushes the file sink

    // Back to console-only, so other tests are not affected by this default.
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("minimal", console));

    std::ifstream in(log);
    REQUIRE(in.is_open());
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(contents.find("after the swap") != std::string::npos);
    // The pre-swap line is console-only by design: ConfigureLogging has not run
    // yet, so there was no file for it to go to.
    CHECK(contents.find("before the swap") == std::string::npos);
    fs::remove(log, ec);
}

namespace {

// What `write` sends to file descriptor `fd` while it runs.
std::string CaptureFd(int fd, const std::function<void()>& write) {
    std::fflush(nullptr);
    int pipe_fds[2];
    REQUIRE(::pipe(pipe_fds) == 0);
    const int saved = ::dup(fd);
    ::dup2(pipe_fds[1], fd);
    write();
    std::fflush(nullptr);
    ::dup2(saved, fd);
    ::close(saved);
    ::close(pipe_fds[1]);
    std::string out;
    char buffer[4096];
    for (ssize_t n; (n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0;) out.append(buffer, n);
    ::close(pipe_fds[0]);
    return out;
}

}  // namespace

TEST_CASE("console_stderr keeps log lines off stdout", "[logging]") {
    // `minimal status-bar` prints its result on stdout: a warning logged
    // there landed inside the block meant for pasting.
    std::string out;
    const std::string err = CaptureFd(STDERR_FILENO, [&out] {
        out = CaptureFd(STDOUT_FILENO, [] {
            REQUIRE(minimal::common::ConfigureLogging({.console_stderr = true}).empty());
            minimal::common::DefaultLogger().warn("console-stderr-probe");
            spdlog::default_logger()->flush();
        });
    });
    CHECK(out.find("console-stderr-probe") == std::string::npos);
    CHECK(err.find("console-stderr-probe") != std::string::npos);
    REQUIRE(minimal::common::ConfigureLogging({}).empty());   // back to the default
}
