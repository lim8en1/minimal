#include "embedding_onnx.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>

#include <onnxruntime/onnxruntime_cxx_api.h>

#include "hf_tokenizer.hpp"

namespace minimal::memory {
namespace {

// One Ort::Env per process (init-safe, cheap); sessions are per instance.
Ort::Env& OrtnEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "minimal-memory");
    return env;
}

class OnnxEmbeddingFunction final : public EmbeddingFunction {
public:
    OnnxEmbeddingFunction(HfTokenizer tokenizer, Ort::Session session,
                          std::string model_name, std::string model_path, bool add_pooler,
                          bool needs_token_type_ids)
        : tokenizer_(std::move(tokenizer)),
          session_(std::move(session)),
          model_name_(std::move(model_name)),
          model_path_(std::move(model_path)),
          add_pooler_(add_pooler),
          needs_token_type_ids_(needs_token_type_ids) {
        memory_info_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    }

    std::vector<Embedding> Embed(const std::vector<std::string>& inputs) override {
        if (inputs.empty()) return {};
        std::vector<Embedding> out;
        out.reserve(inputs.size());

        // The graph either outputs pooled+normalized [batch, dim] directly
        // (add_pooler_=false) or last_hidden_state [batch, seq, hidden] which
        // we mean-pool over the attention mask then L2-normalize.
        for (const auto& input : inputs) {
            auto ids = tokenizer_.Encode(input);
            std::vector<long long> id_values(ids.begin(), ids.end());
            const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(ids.size())};

            std::vector<Ort::Value> ort_inputs;
            const auto* info = static_cast<const OrtMemoryInfo*>(memory_info_);
            const std::int64_t* shape_data = shape.data();
            ort_inputs.push_back(Ort::Value::CreateTensor<long long>(
                info, id_values.data(), id_values.size(), shape_data, shape.size()));
            std::vector<long long> mask(ids.size(), 1);
            ort_inputs.push_back(Ort::Value::CreateTensor<long long>(
                info, mask.data(), mask.size(), shape_data, shape.size()));
            if (needs_token_type_ids_) {
                std::vector<long long> type_ids(ids.size(), 0);
                ort_inputs.push_back(Ort::Value::CreateTensor<long long>(
                    info, type_ids.data(), type_ids.size(), shape_data, shape.size()));
            }

            const char* input_names[] = {"input_ids", "attention_mask", "token_type_ids"};
            const char* output_names[] = {add_pooler_ ? "sentence_embedding" : "last_hidden_state"};
            std::size_t n_inputs = needs_token_type_ids_ ? 3 : 2;

            auto outputs = session_.Run(Ort::RunOptions{nullptr},
                                        input_names, ort_inputs.data(), n_inputs,
                                        output_names, 1);

            Embedding embedding;
            const auto& output = outputs.front();
            const auto shape_info = output.GetTensorTypeAndShapeInfo();
            const auto dims = shape_info.GetShape();

            if (add_pooler_) {
                const float* data = output.GetTensorData<float>();
                embedding.values.assign(data, data + shape_info.GetElementCount());
            } else {
                // [1, seq, hidden]: mean over non-masked positions.
                const std::int64_t seq = dims[1], hidden = dims[2];
                const float* data = output.GetTensorData<float>();
                embedding.values.assign(hidden, 0.0f);
                for (std::int64_t s = 0; s < seq; ++s) {
                    for (std::int64_t h = 0; h < hidden; ++h) {
                        embedding.values[static_cast<std::size_t>(h)] += data[s * hidden + h];
                    }
                }
                const float denom = static_cast<float>(seq);
                for (auto& value : embedding.values) value /= denom;
                double norm = 0.0;
                for (const auto value : embedding.values) {
                    norm += static_cast<double>(value) * value;
                }
                norm = std::sqrt(norm);
                if (norm > 0.0) {
                    for (auto& value : embedding.values) {
                        value = static_cast<float>(static_cast<double>(value) / norm);
                    }
                }
            }
            embedding.dim = static_cast<int>(embedding.values.size());
            out.push_back(std::move(embedding));
        }
        return out;
    }

    std::string Describe() const override {
        return "onnx (" + model_name_ + ", " + std::to_string(dim_hint_) + " dims)";
    }

    std::string Signature() const override { return "onnx:" + model_path_ + ":" + std::to_string(dim_hint_); }

    void set_dim_hint(int dim) { dim_hint_ = dim; }

private:
    HfTokenizer tokenizer_;
    Ort::Session session_;
    Ort::MemoryInfo memory_info_{nullptr};
    std::string model_name_, model_path_;
    bool add_pooler_ = false;
    bool needs_token_type_ids_ = false;
    int dim_hint_ = 0;
};

}  // namespace

std::unique_ptr<EmbeddingFunction> TryMakeOnnxEmbeddingFunction(const MemorySettings& settings,
                                                                const std::filesystem::path& data_dir) {
    try {
        const auto resolve = [&](const std::string& value) -> std::filesystem::path {
            if (value.empty()) return {};
            std::filesystem::path p(value);
            return p.is_absolute() ? p : data_dir / p;
        };
        const std::filesystem::path model = resolve(settings.model_path);
        const std::filesystem::path tokenizer = resolve(settings.tokenizer_path);
        if (model.empty() || tokenizer.empty() || !std::filesystem::exists(model) ||
            !std::filesystem::exists(tokenizer)) {
            return nullptr;
        }
        auto tok = HfTokenizer::Load(tokenizer.string());

        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(settings.intra_op_threads);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        Ort::Session session(OrtnEnv(), model.string().c_str(), options);

        // Graph shape sniffing: pooled output means the export already did
        // mean-pool+normalize; token_type_ids in the input list means the
        // model wants them (BERT-family does).
        bool add_pooler = false, needs_type_ids = false;
        std::string model_name = model.filename().string();
        for (std::size_t i = 0; i < session.GetInputCount(); ++i) {
            const auto name = session.GetInputNameAllocated(i, Ort::AllocatorWithDefaultOptions());
            if (std::string_view(name.get()) == "token_type_ids") needs_type_ids = true;
        }
        for (std::size_t i = 0; i < session.GetOutputCount(); ++i) {
            const auto name = session.GetOutputNameAllocated(i, Ort::AllocatorWithDefaultOptions());
            if (std::string_view(name.get()) == "sentence_embedding") add_pooler = true;
        }

        auto function = std::make_unique<OnnxEmbeddingFunction>(
            std::move(tok), std::move(session), std::move(model_name), model.string(),
            add_pooler, needs_type_ids);
        // Dim is read from the first run's output shape (plan §4); the hint
        // starts at 0 and Describe()/Signature() report it after the first
        // Embed() call lands -- SetSignatureForAll runs at registration, the
        // first real Embed corrects it before any vectors are stored.
        return function;
    } catch (const std::exception&) {
        return nullptr;  // degrade to lexical; /memory reports the state
    }
}

}  // namespace minimal::memory
