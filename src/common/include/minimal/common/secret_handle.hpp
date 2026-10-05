#pragma once

// A credential a connection sends, read when the request is built rather than
// copied out of config at startup: a secret stored with /secret set reaches
// the very next request, with no restart. Bound from a config reference
// ("env:NAME", "secret:NAME", "secret:<plugin>:NAME", or a literal) by
// config::SecretResolver; this type only carries the result.
//
// Copyable and cheap (the getter is shared). Get() is safe to call from any
// thread as long as the bound getter is -- the resolver's are.

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace minimal::common {

struct SecretValue {
    std::string value;
    // Where the value came from ("secrets.json", "env OPENAI_API_KEY",
    // "inline api_key") -- never the value itself, so it is safe to log.
    std::string source;
};

class SecretHandle {
public:
    using Getter = std::function<SecretValue()>;

    // No credential configured: a keyless local server is a normal state.
    SecretHandle() = default;

    // `identity` decides equality (two handles bound from the same reference
    // name the same credential); `description` is what logs and errors show.
    // For a literal key the identity is the key itself, so it is never exposed.
    SecretHandle(std::string identity, std::string description, Getter get)
        : identity_(std::move(identity)),
          description_(std::move(description)),
          get_(std::make_shared<const Getter>(std::move(get))) {}

    static SecretHandle Literal(std::string value) {
        std::string identity = value;
        return SecretHandle(std::move(identity), "inline api_key",
                            [value = std::move(value)] { return SecretValue{value, "inline api_key"}; });
    }

    bool configured() const { return get_ != nullptr; }

    // The current value. Throws std::runtime_error, with a message naming the
    // reference and how to fix it, when a configured reference does not
    // resolve right now (a secret not set yet, a host binding that refuses
    // this destination). Never called on an unconfigured handle.
    SecretValue Get() const {
        if (!get_) throw std::logic_error("SecretHandle::Get on an unconfigured handle");
        return (*get_)();
    }

    // The reference as configured ("secret:anthropic", "env FOO") or
    // "inline api_key" -- safe to log.
    const std::string& description() const { return description_; }

    friend bool operator==(const SecretHandle& a, const SecretHandle& b) {
        return a.configured() == b.configured() && a.identity_ == b.identity_;
    }

private:
    std::string identity_;
    std::string description_;
    std::shared_ptr<const Getter> get_;
};

}  // namespace minimal::common
