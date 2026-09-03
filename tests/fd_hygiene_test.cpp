// fd hygiene: the serve-stage sweep that marks every open descriptor
// close-on-exec, so bash commands, MCP stdio children and hook actions inherit
// only what their spawner explicitly dups.
//
// The sweep's real guarantee is structural (no inherited server fds), which a
// unit test cannot observe end to end without running the server binary; what
// IS testable is the mechanism: a descriptor opened without O_CLOEXEC comes
// out marked.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include "minimal/common/fd_hygiene.hpp"
#include "temp_dir.hpp"

TEST_CASE("MarkInheritedFdsCloseOnExec marks descriptors opened without O_CLOEXEC",
          "[fd_hygiene]") {
    minimal::testing::TempDir dir;
    const auto probe = dir.path() / "probe";
    const int fd = ::open(probe.c_str(), O_RDWR | O_CREAT, 0644);
    REQUIRE(fd >= 0);
    REQUIRE((::fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);

    minimal::common::MarkInheritedFdsCloseOnExec();

    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    ::close(fd);
}

TEST_CASE("already-marked descriptors are left alone and stdio is never touched",
          "[fd_hygiene]") {
    minimal::testing::TempDir dir;
    const auto probe = dir.path() / "probe";
    const int fd = ::open(probe.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(fd >= 0);

    minimal::common::MarkInheritedFdsCloseOnExec();

    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    ::close(fd);
    // stdio is exempt by definition: children legitimately keep 0/1/2. The
    // sweep must not have marked them even if the ambient environment did not.
    for (const int stdio : {0, 1, 2}) {
        const int flags = ::fcntl(stdio, F_GETFD);
        if (flags < 0) continue;  // no such descriptor in this harness
        CHECK((flags & FD_CLOEXEC) == 0);
    }
}
