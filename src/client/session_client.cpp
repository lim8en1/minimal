#include "minimal/client/session_client.hpp"

#include <utility>

namespace minimal::client {

SessionClient::SessionClient(std::string server_address)
    : server_address_(std::move(server_address)) {}

SessionClient::~SessionClient() = default;

}  // namespace minimal::client
