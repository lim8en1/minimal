#include "endpoint.hpp"

#include <stdexcept>
#include <utility>

namespace minimal::checklist {

std::string ChecklistEndpoint::Call(const std::string& method, const std::string& json_payload) {
    auto reply = rpc_.Handle(method, json_payload);
    if (!reply.ok) throw std::runtime_error(reply.error);
    return std::move(reply.payload);
}

bool ChecklistEndpoint::Subscribe(const std::string& method, const std::string& /*json_payload*/,
                                  std::shared_ptr<plugins::PluginSubscriptionSink> sink) {
    if (method != "changes") return false;
    hub_.AddSink(std::move(sink));
    return true;
}

}  // namespace minimal::checklist
