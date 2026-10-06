#pragma once

// store policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

namespace minimal::store {

// Lock-file acquisitions retried when the path is replaced under us (the
// locked fd and the path name different files). One retry covers a single
// concurrent replace; a path that keeps churning is refused, not raced.
inline constexpr int kMaxLockIdentityAttempts = 3;

}  // namespace minimal::store
