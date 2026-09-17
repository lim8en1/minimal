#include "hf_tokenizer.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace minimal::memory {
namespace {

// The basic tokenizer's whitespace + punctuation split (BERT's rule), with
// optional lowercasing + accent stripping left to the config flag. CJK
// characters are split out like BERT does (each its own token).
bool IsWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool IsPunctuation(char c) {
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) ||
           (c >= 123 && c <= 126);
}

bool IsCjk(char c) {
    // CJK unified ideographs + extensions A/B, the ranges BERT's basic
    // tokenizer special-cases; multi-byte UTF-8 sequences start >= 0x80 so
    // this only ever sees their first byte -- enough for the split decision.
    const auto u = static_cast<unsigned char>(c);
    return (u >= 0xE4 && u <= 0xE9) || u == 0xF0;  // coarse: any 3-byte CJK-class lead
}

std::vector<std::string> BasicTokenize(const std::string& text, bool lowercase) {
    std::vector<std::string> out;
    std::string token;
    auto flush = [&] {
        if (!token.empty()) {
            if (lowercase) {
                for (auto& c : token) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            out.push_back(token);
            token.clear();
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (IsWhitespace(c)) {
            flush();
        } else if (IsPunctuation(c)) {
            flush();
            out.emplace_back(1, c);
        } else if (IsCjk(c) && static_cast<unsigned char>(c) >= 0x80) {
            // Split before/after multi-byte CJK runs: emit the whole UTF-8
            // sequence as its own token. The lead byte determines length.
            flush();
            const auto lead = static_cast<unsigned char>(c);
            const std::size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (i + len <= text.size()) {
                out.emplace_back(text.substr(i, len));
                i += len - 1;
            }
        } else {
            token += c;
        }
    }
    flush();
    return out;
}

}  // namespace

HfTokenizer HfTokenizer::Load(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("memory: cannot open tokenizer file " + path);
    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("memory: tokenizer.json is not valid JSON: ") +
                                 error.what());
    }

    // Model type gate: only WordPiece is implemented. The `model` object's
    // "type" field is authoritative.
    const auto model = json.find("model");
    if (model == json.end() || !model->is_object()) {
        throw std::runtime_error("memory: tokenizer.json has no model object");
    }
    const auto type = model->find("type");
    if (type == model->end() || !type->is_string() || type->get<std::string>() != "WordPiece") {
        throw std::runtime_error(
            "memory: tokenizer.json model type is not WordPiece (sentencepiece/BPE models "
            "are not supported by this backend yet)");
    }

    HfTokenizer out;
    const auto vocab = model->find("vocab");
    if (vocab == model->end() || !vocab->is_object()) {
        throw std::runtime_error("memory: tokenizer.json has no model.vocab object");
    }
    for (auto it = vocab->begin(); it != vocab->end(); ++it) {
        if (it.value().is_number_integer()) {
            out.vocab_[it.key()] = it.value().get<int>();
        }
    }
    if (out.vocab_.empty()) {
        throw std::runtime_error("memory: tokenizer.json vocab is empty");
    }

    const auto unk = model->find("unk_token");
    if (unk != model->end() && unk->is_string()) {
        const auto found = out.vocab_.find(unk->get<std::string>());
        if (found != out.vocab_.end()) out.unk_id_ = found->second;
    }
    // Special tokens may live under added_tokens or model's own fields; the
    // robust path: look the canonical names up in the vocab.
    auto id_of = [&](const char* token) -> int {
        const auto found = out.vocab_.find(token);
        return found != out.vocab_.end() ? found->second : -1;
    };
    out.cls_id_ = id_of("[CLS]");
    out.sep_id_ = id_of("[SEP]");
    out.pad_id_ = id_of("[PAD]");
    if (out.cls_id_ < 0 || out.sep_id_ < 0) {
        throw std::runtime_error("memory: tokenizer.json vocab has no [CLS]/[SEP] tokens");
    }

    // max_len: model_max_length under tokenizer_options or top level; 512
    // default (BERT-family).
    if (auto it = json.find("tokenizer_options"); it != json.end() && it->is_object()) {
        if (auto ml = it->find("model_max_length"); ml != it->end() && ml->is_number()) {
            out.max_len_ = ml->get<int>();
        }
    }
    if (auto it = json.find("normalizer"); it != json.end() && it->is_object()) {
        // Lowercase when the normalizer says so; the BertNormalizer case is
        // the one v1 models use.
        if (auto lower = it->find("lowercase"); lower != it->end() && lower->is_boolean()) {
            out.lowercase_ = lower->get<bool>();
        }
    }
    return out;
}

std::vector<int> HfTokenizer::Encode(const std::string& text) const {
    std::vector<int> ids;
    ids.push_back(cls_id_);
    for (const auto& word : BasicTokenize(text, lowercase_)) {
        // Greedy longest-match WordPiece over the vocab.
        std::size_t pos = 0;
        while (pos < word.size() && static_cast<int>(ids.size()) < max_len_ - 1) {
            std::size_t end = word.size();
            std::optional<int> matched;
            while (end > pos) {
                std::string piece = word.substr(pos, end - pos);
                if (pos > 0) piece = continuation_prefix_ + piece;
                const auto found = vocab_.find(piece);
                if (found != vocab_.end()) {
                    matched = found->second;
                    break;
                }
                --end;
            }
            if (!matched) {
                ids.push_back(unk_id_);
                break;  // remaining subtokens of this word are lost -- BERT's
                        // behavior on a full-word miss is per-char retries; the
                        // unk is the pragmatic equivalent for v1
            }
            ids.push_back(*matched);
            pos = end;
        }
        if (static_cast<int>(ids.size()) >= max_len_ - 1) break;
    }
    ids.push_back(sep_id_);
    return ids;
}

}  // namespace minimal::memory
