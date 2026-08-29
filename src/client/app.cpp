// `minimal` entry point: TUI by default, headless with -p.
//
// The headless mode is not a TUI flag bolted on -- it is a separate client against
// the same SessionService, and the reason PermissionRunMode::AllowOnly exists.
// Building it first is what proves the proto is a real boundary rather than a
// TUI-shaped hole, and it is the natural end-to-end test harness.

#include <cstdio>
#include <cstring>

#include "minimal/common/abi_tag.hpp"

namespace {

int RunHeadless(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // TODO: connect, CreateSession(project_root = cwd), send one UserChatTurn,
    // stream ServerEvents to stdout as text or JSONL, exit on stop_reason.
    // Defaults to PermissionRunMode::AllowOnly so an Ask-gated tool auto-denies
    // instead of blocking on a human who was never attached.
    std::puts("minimal -p: not yet implemented");
    return 0;
}

int RunTui(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // TODO: FTXUI frame -- tab bar / session view / optional panel / status bar,
    // plus a modal layer. See ARCHITECTURE.md -- TUI layout.
    std::puts("minimal: TUI not yet implemented");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("minimal %s\n", minimal::common::Version());
            return 0;
        }
        if (std::strcmp(argv[i], "-p") == 0 || std::strcmp(argv[i], "--print") == 0) {
            return RunHeadless(argc, argv);
        }
    }
    return RunTui(argc, argv);
}
