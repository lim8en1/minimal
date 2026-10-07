#pragma once

// The bundled prompt editor as the prompt_editor slot's factory
// (registries.hpp PromptEditorFactory): one PromptEditorState and editor
// component per instance.

#include <memory>
#include <vector>

#include "minimal/client/plugins/registries.hpp"

namespace minimal::client::prompt_editor {

class Factory final : public plugins::PromptEditorFactory {
public:
    std::vector<plugins::PromptEditorAction> Actions() const override;
    std::unique_ptr<plugins::PromptEditor> Make(plugins::PromptEditorHost& host,
                                                const Theme& theme) const override;
};

}  // namespace minimal::client::prompt_editor
