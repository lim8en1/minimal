#pragma once

// hf_tokenizer -- the HF fast-tokenizer (tokenizer.json) reader for BERT-style
// embedders: WordPiece vocab, greedy longest-match, [CLS]/[SEP], lowercase
// basic preprocessing, truncation. The ORT backend's only companion; pure
// function of the JSON file, unit-testable without any model.
//
// v1 scope, deliberately narrow: the tokenizer shapes of the models the plan
// names (all-MiniLM-L6-v2, bge-small-en-v1.5, nomic-embed-text). Unigram
// (sentencepiece-style) models and BPE-with-merges are NOT v1 -- the parser
// reports them as unsupported and the caller degrades to "lexical only".

#include <string>
#include <unordered_map>
#include <vector>

namespace minimal::memory {

class HfTokenizer {
public:
    // Throws std::runtime_error with a self-describing message on an
    // unparsable or unsupported tokenizer.json.
    static HfTokenizer Load(const std::string& path);

    // Token ids for ONE text, [CLS]-wrapped, truncated to max_len.
    std::vector<int> Encode(const std::string& text) const;

    int cls_id() const { return cls_id_; }
    int sep_id() const { return sep_id_; }
    int pad_id() const { return pad_id_; }
    int max_len() const { return max_len_; }

private:
    std::unordered_map<std::string, int> vocab_;
    int unk_id_ = 0;
    int cls_id_ = 0;
    int sep_id_ = 0;
    int pad_id_ = 0;
    int max_len_ = 512;
    bool lowercase_ = true;
    // The WordPiece "##" continuation prefix as defined by the vocab.
    std::string continuation_prefix_ = "##";
};

}  // namespace minimal::memory
