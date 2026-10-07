#include "tool_result_caps.hpp"

#include <algorithm>
#include <variant>

#include "minimal/agent/policy.hpp"
#include "minimal/common/units.hpp"
#include "minimal/tools/tool_output_spill.hpp"

namespace minimal::agent {

std::vector<const llm::ToolUseBlock*> ToolUsesOf(const llm::Message& message) {
    std::vector<const llm::ToolUseBlock*> out;
    for (const auto& block : message.content) {
        if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) {
            out.push_back(use);
        }
    }
    return out;
}

void CapToolResult(llm::ToolResultBlock& block, std::int64_t max_bytes,
                   tools::ToolOutputSpill* spill) {
    if (max_bytes <= 0) return;
    const auto cap = static_cast<std::size_t>(max_bytes);

    std::size_t total = 0;
    for (const auto& item : block.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&item)) total += text->text.size();
    }
    if (total <= cap) return;

    std::string full;
    full.reserve(total);
    for (const auto& item : block.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&item)) full += text->text;
    }
    // Start and end, cut on UTF-8 boundaries: an error or a summary is as
    // likely at the end of a long result as at its start.
    std::string kept =
        spill != nullptr
            ? tools::CapWithSpill(std::move(full), cap, *spill)
            : tools::HeadTail(full, cap) + "\n[Result cut to " + std::to_string(cap) + " of " +
                  std::to_string(total) + " bytes (the start and the end).]";
    block.content.clear();
    block.content.push_back(llm::TextBlock{std::move(kept)});
}

std::size_t CapToolDisplay(llm::ToolResultBlock& block, std::int64_t max_bytes) {
    if (max_bytes <= 0 || block.display.is_null()) return 0;
    const std::size_t size =
        block.display.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size();
    if (size <= static_cast<std::size_t>(max_bytes)) return 0;
    block.display = nullptr;
    return size;
}

std::optional<int> FitOutputCap(std::optional<int> cap, std::optional<std::int64_t> window,
                                std::int64_t input_tokens) {
    if (!cap || !window || *window <= 0) return cap;
    const std::int64_t margin =
        std::max(kOutputCapMinMarginTokens, common::PercentOf(*window, kOutputCapMarginPercent));
    const std::int64_t room = std::max(kOutputCapFloorTokens, *window - input_tokens - margin);
    return static_cast<int>(std::min<std::int64_t>(*cap, room));
}

bool ElideLargestToolResult(std::vector<llm::Message>& history) {
    llm::ToolResultBlock* largest = nullptr;
    std::size_t largest_size = 0;
    for (auto& message : history) {
        for (auto& block : message.content) {
            auto* result = std::get_if<llm::ToolResultBlock>(&block);
            if (result == nullptr) continue;
            std::size_t size = 0;
            for (const auto& item : result->content) {
                if (const auto* text = std::get_if<llm::TextBlock>(&item)) size += text->text.size();
            }
            if (size > largest_size) {
                largest_size = size;
                largest = result;
            }
        }
    }
    if (largest == nullptr || static_cast<std::int64_t>(largest_size) <= kElisionMinBytes) {
        return false;
    }
    CapToolResult(*largest, kElisionTargetBytes);
    largest->display = nullptr;   // it described the full result, not the stub
    return true;
}

}  // namespace minimal::agent
