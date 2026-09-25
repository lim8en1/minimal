#include "minimal/mcp/signal_protocol.hpp"

#include <nlohmann/json.hpp>

namespace minimal::mcp {

bool ParseSignalNotification(const std::string& raw, SignalFrame& frame, std::string& error) {
    error.clear();
    // Parse leniently: a malformed line from a channel server is a logged
    // drop, never an exception into the read thread that carries it.
    const nlohmann::json message = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (!message.is_object()) {
        error = "dropped a non-JSON frame";
        return false;
    }
    const auto method = message.find("method");
    if (method == message.end() || !method->is_string() ||
        method->get<std::string>() != kSignalNotificationMethod) {
        return false;  // some other MCP notification: the server's own business
    }
    const auto params = message.find("params");
    if (params == message.end() || !params->is_object()) {
        error = "dropped a signal frame without params";
        return false;
    }
    const auto signal = params->find("signal");
    const auto content = params->find("content");
    if (signal == params->end() || !signal->is_string() || content == params->end() ||
        !content->is_string()) {
        error = "dropped a signal frame missing string 'signal'/'content'";
        return false;
    }
    frame.signal = signal->get<std::string>();
    frame.content = content->get<std::string>();
    frame.meta.clear();
    if (const auto meta = params->find("meta"); meta != params->end() && meta->is_object()) {
        // A non-string value is decoration the router could not render:
        // dropped here, like a bad key is dropped there.
        for (const auto& [key, value] : meta->items()) {
            if (value.is_string()) frame.meta.emplace(key, value.get<std::string>());
        }
    }
    return true;
}

}  // namespace minimal::mcp
