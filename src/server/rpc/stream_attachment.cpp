#include "minimal/rpc/stream_attachment.hpp"

#include <stdexcept>

namespace minimal::rpc {

EventTranslator& StreamAttachment::TransportOf(agent::Session& session) {
    auto* translator = dynamic_cast<EventTranslator*>(session.transport().get());
    if (translator == nullptr) {
        throw std::logic_error("session " + session.info().session_id +
                               " has no rpc::EventTranslator transport");
    }
    return *translator;
}

StreamAttachment::StreamAttachment(agent::Session& session)
    : session_(session), transport_(TransportOf(session)) {
    handle_ = transport_.queue().Subscribe();
    try {
        session_.NotifyAttached();
    } catch (...) {
        transport_.queue().Unsubscribe(handle_);  // never leave a subscriber nobody counted
        throw;
    }
    attached_ = true;
}

std::size_t StreamAttachment::SubscriberCount() const {
    return transport_.queue().subscriber_count();
}

bool StreamAttachment::PublishTo(v1::ServerEvent event) const {
    return transport_.queue().PublishTo(handle_, std::move(event));
}

void StreamAttachment::ReplayOpenPrompts() const {
    transport_.pending().DeliverOpenPrompts([this](v1::ServerEvent prompt) {
        transport_.queue().PublishToIfAbsent(handle_, std::move(prompt));
    });
}

std::optional<v1::ServerEvent> StreamAttachment::Pop() const {
    return transport_.queue().Pop(handle_);
}

void StreamAttachment::Detach() {
    if (!attached_) return;
    attached_ = false;
    transport_.queue().Unsubscribe(handle_);
    session_.NotifyDetached();
}

}  // namespace minimal::rpc
