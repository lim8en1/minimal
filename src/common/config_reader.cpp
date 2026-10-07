#include "minimal/common/config_reader.hpp"
#include "minimal/common/policy.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace minimal::config {

bool IsCommentKey(std::string_view key) { return key.rfind("//", 0) == 0; }

namespace {

std::string Quoted(const nlohmann::json& value) {
    std::string text = value.dump();
    if (text.size() > common::kConfigErrorValuePreviewChars) {
        text = text.substr(0, common::kConfigErrorValuePreviewChars) + "...";
    }
    return text;
}

bool HasCrLf(const std::string& text) {
    return text.find('\r') != std::string::npos || text.find('\n') != std::string::npos;
}

}  // namespace

// The fields one describe-mode run has seen, in read order. One per
// SchemaRecording; every Reader over the recorded file shares it.
class SchemaRecorder {
public:
    explicit SchemaRecorder(std::string file) : file_(std::move(file)) {}

    const std::string& file() const { return file_; }

    // The field at `path`, created on first sight. Later notes merge into it:
    // MaybeEnum notes "string" first, then "enum".
    nlohmann::json& Field(const std::string& path) {
        const auto it = index_.find(path);
        if (it != index_.end()) return fields_[it->second];
        index_.emplace(path, fields_.size());
        fields_.push_back({{"path", path}, {"apply", "restart"}});
        return fields_.back();
    }

    // A field only Describe()d or Default()ed -- read through MaybeObject,
    // an opaque subtree -- is "json": edited as a JSON value.
    nlohmann::json Schema() const {
        nlohmann::json fields = nlohmann::json::array();
        for (const auto& field : fields_) {
            fields.push_back(field);
            if (!field.contains("type")) fields.back()["type"] = "json";
        }
        return {{"file", file_}, {"fields", std::move(fields)}};
    }

    // Takes the fields under `prefix` ("rate_sources[]."), their paths
    // relative to it: an object list's item fields, recorded by running the
    // item reader over a sample.
    nlohmann::json Extract(const std::string& prefix) {
        nlohmann::json taken = nlohmann::json::array();
        std::vector<nlohmann::json> kept;
        for (auto& field : fields_) {
            const std::string path = field.value("path", "");
            if (path.rfind(prefix, 0) != 0) {
                kept.push_back(std::move(field));
                continue;
            }
            field["path"] = path.substr(prefix.size());
            if (!field.contains("type")) field["type"] = "json";
            taken.push_back(std::move(field));
        }
        fields_ = std::move(kept);
        index_.clear();
        for (std::size_t i = 0; i < fields_.size(); ++i) index_.emplace(fields_[i]["path"], i);
        return taken;
    }

private:
    std::string file_;
    std::vector<nlohmann::json> fields_;
    std::map<std::string, std::size_t> index_;
};

namespace {

// The recording a Reader constructed on this thread attaches to.
thread_local std::shared_ptr<SchemaRecorder> t_recorder;

}  // namespace

SchemaRecording::SchemaRecording(std::string file_label)
    : recorder_(std::make_shared<SchemaRecorder>(std::move(file_label))),
      previous_(std::move(t_recorder)) {
    t_recorder = recorder_;
}

SchemaRecording::~SchemaRecording() { t_recorder = std::move(previous_); }

nlohmann::json SchemaRecording::Schema() const { return recorder_->Schema(); }

Reader::Reader(const nlohmann::json& node, std::string file, std::string path, WarningSink warn)
    : node_(&node), file_(std::move(file)), path_(std::move(path)), warn_(std::move(warn)) {
    if (!node.is_object()) {
        throw ConfigError(file_ + ": " + (path_.empty() ? "the top level" : path_) +
                          ": must be an object, got " + Quoted(node));
    }
    if (t_recorder && t_recorder->file() == file_) recorder_ = t_recorder;
}

void Reader::Note(std::string_view key, const char* type, nlohmann::json extra) {
    if (!recorder_) return;
    nlohmann::json& field = recorder_->Field(Join(key));
    field["type"] = type;
    if (extra.is_object()) field.update(extra);
}

void Reader::Describe(std::string_view key, FieldMeta meta) {
    if (!recorder_) return;
    nlohmann::json& field = recorder_->Field(Join(key));
    if (!meta.doc.empty()) field["doc"] = std::move(meta.doc);
    field["apply"] = meta.apply == ApplyPolicy::kLive ? "live" : "restart";
    if (meta.secret) field["secret"] = true;
    if (meta.format == FieldFormat::kModelRef) field["format"] = "model_ref";
    if (!meta.empty_label.empty()) field["empty_label"] = std::move(meta.empty_label);
}

void Reader::RelevantWhen(std::string_view key, std::string_view field,
                          std::initializer_list<std::string_view> values) {
    if (!recorder_) return;
    nlohmann::json in = nlohmann::json::array();
    for (const auto value : values) in.push_back(std::string(value));
    recorder_->Field(Join(key))["when"] = {{"field", Join(field)}, {"in", std::move(in)}};
}

void Reader::RelevantWhenSet(std::string_view key, std::string_view field) {
    if (!recorder_) return;
    recorder_->Field(Join(key))["when"] = {{"field", Join(field)}, {"set", true}};
}

void Reader::Default(std::string_view key, nlohmann::json value) {
    if (!recorder_) return;
    recorder_->Field(Join(key))["default"] = std::move(value);
}

std::string Reader::Join(std::string_view key) const {
    if (key.empty()) return path_;
    if (path_.empty()) return std::string(key);
    return path_ + "." + std::string(key);
}

std::string Reader::Where(std::string_view key) const {
    const std::string joined = Join(key);
    return file_ + ": " + (joined.empty() ? "the top level" : joined);
}

void Reader::Fail(std::string_view key, const std::string& message) const {
    throw ConfigError(Where(key) + ": " + message);
}

void Reader::Warn(std::string_view key, const std::string& message) const {
    if (warn_) warn_(Where(key) + ": " + message);
}

bool Reader::Has(std::string_view key) const { return node_->contains(std::string(key)); }

const nlohmann::json* Reader::Find(std::string_view key) {
    queried_.emplace(key);
    const auto it = node_->find(std::string(key));
    if (it == node_->end()) return nullptr;
    consumed_.emplace(key);
    return &*it;
}

const nlohmann::json* Reader::Raw(std::string_view key) {
    Note(key, "json");
    return Find(key);
}

void Reader::Accept(std::string_view key) {
    queried_.emplace(key);
    consumed_.emplace(key);
}

std::optional<bool> Reader::MaybeBool(std::string_view key) {
    Note(key, "boolean");
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_boolean()) Fail(key, "must be true or false, got " + Quoted(*value));
    return value->get<bool>();
}

std::optional<std::string> Reader::MaybeString(std::string_view key, bool allow_empty) {
    Note(key, "string");
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_string()) Fail(key, "must be a string, got " + Quoted(*value));
    std::string text = value->get<std::string>();
    if (!allow_empty && text.empty()) Fail(key, "must not be empty");
    return text;
}

std::string Reader::String(std::string_view key, std::string fallback, bool allow_empty) {
    if (recorder_) Default(key, fallback);
    auto value = MaybeString(key, allow_empty);
    return value ? std::move(*value) : std::move(fallback);
}

std::string Reader::RequireString(std::string_view key) {
    Note(key, "string", {{"required", true}});
    if (!Has(key)) Fail(key, "is required");
    return *MaybeString(key, /*allow_empty=*/false);
}

std::optional<std::int64_t> Reader::MaybeInt(std::string_view key, std::int64_t min,
                                             std::int64_t max) {
    if (recorder_) {
        nlohmann::json range = nlohmann::json::object();
        if (min != std::numeric_limits<std::int64_t>::min()) range["minimum"] = min;
        if (max != std::numeric_limits<std::int64_t>::max()) range["maximum"] = max;
        Note(key, "integer", std::move(range));
    }
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_number_integer()) Fail(key, "must be an integer, got " + Quoted(*value));
    if (value->is_number_unsigned() &&
        value->get<std::uint64_t>() > static_cast<std::uint64_t>(max)) {
        Fail(key, "must be at most " + std::to_string(max) + ", got " + Quoted(*value));
    }
    const auto number = value->get<std::int64_t>();
    if (number < min) {
        Fail(key, "must be at least " + std::to_string(min) + ", got " + Quoted(*value));
    }
    if (number > max) {
        Fail(key, "must be at most " + std::to_string(max) + ", got " + Quoted(*value));
    }
    return number;
}

std::int64_t Reader::Int(std::string_view key, std::int64_t fallback, std::int64_t min,
                         std::int64_t max) {
    if (recorder_) Default(key, fallback);
    return MaybeInt(key, min, max).value_or(fallback);
}

std::optional<double> Reader::MaybeDouble(std::string_view key, double min, double max) {
    if (recorder_) {
        nlohmann::json range = nlohmann::json::object();
        if (std::isfinite(min)) range["minimum"] = min;
        if (std::isfinite(max)) range["maximum"] = max;
        Note(key, "number", std::move(range));
    }
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_number()) Fail(key, "must be a number, got " + Quoted(*value));
    const double number = value->get<double>();
    if (!std::isfinite(number) || number < min || number > max) {
        Fail(key, "must be between " + nlohmann::json(min).dump() + " and " +
                      nlohmann::json(max).dump() + ", got " + Quoted(*value));
    }
    return number;
}

double Reader::Double(std::string_view key, double fallback, double min, double max) {
    if (recorder_) Default(key, fallback);
    return MaybeDouble(key, min, max).value_or(fallback);
}

std::optional<std::string> Reader::MaybeEnum(std::string_view key,
                                             std::initializer_list<std::string_view> choices) {
    auto value = MaybeString(key);
    if (recorder_) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto choice : choices) list.push_back(std::string(choice));
        Note(key, "enum", {{"choices", std::move(list)}});
    }
    if (!value) return std::nullopt;
    for (const auto choice : choices) {
        if (*value == choice) return value;
    }
    std::string list;
    for (const auto choice : choices) {
        list += (list.empty() ? "\"" : ", \"") + std::string(choice) + "\"";
    }
    Fail(key, "must be one of " + list + ", got \"" + *value + "\"");
}

std::string Reader::Enum(std::string_view key, std::string fallback,
                         std::initializer_list<std::string_view> choices) {
    if (recorder_) Default(key, fallback);
    auto value = MaybeEnum(key, choices);
    return value ? std::move(*value) : std::move(fallback);
}

std::optional<std::vector<std::string>> Reader::MaybeStringList(std::string_view key,
                                                                bool allow_empty_entries) {
    Note(key, "string_list");
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_array()) Fail(key, "must be a list of strings, got " + Quoted(*value));
    std::vector<std::string> out;
    for (const auto& item : *value) {
        if (!item.is_string()) Fail(key, "must contain only strings, got " + Quoted(item));
        if (!allow_empty_entries && item.get<std::string>().empty()) {
            Fail(key, "must not contain empty strings");
        }
        out.push_back(item.get<std::string>());
    }
    return out;
}

std::vector<std::string> Reader::StringList(std::string_view key, bool allow_empty_entries) {
    if (recorder_) Default(key, nlohmann::json::array());
    return MaybeStringList(key, allow_empty_entries).value_or(std::vector<std::string>{});
}

std::optional<std::map<std::string, std::string>> Reader::MaybeStringMap(std::string_view key) {
    Note(key, "string_map");
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_object()) Fail(key, "must be an object of strings, got " + Quoted(*value));
    std::map<std::string, std::string> out;
    for (const auto& [name, item] : value->items()) {
        if (IsCommentKey(name)) continue;
        const std::string where = std::string(key) + "." + name;
        if (!item.is_string()) Fail(where, "must be a string, got " + Quoted(item));
        if (HasCrLf(name)) Fail(where, "key contains a CR or LF");
        if (HasCrLf(item.get<std::string>())) Fail(where, "contains a CR or LF");
        out.emplace(name, item.get<std::string>());
    }
    return out;
}

std::optional<Reader> Reader::MaybeObject(std::string_view key) {
    const auto* value = Find(key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_object()) Fail(key, "must be an object, got " + Quoted(*value));
    return Reader(*value, file_, Join(key), warn_);
}

Reader Reader::Object(std::string_view key) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    if (auto child = MaybeObject(key)) return std::move(*child);
    return Reader(kEmpty, file_, Join(key), warn_);
}

std::vector<std::pair<std::string, const nlohmann::json*>> Reader::Entries(std::string_view key) {
    Note(key, "json");
    std::vector<std::pair<std::string, const nlohmann::json*>> out;
    const auto* value = Find(key);
    if (value == nullptr) return out;
    if (!value->is_object()) Fail(key, "must be an object, got " + Quoted(*value));
    for (const auto& [name, item] : value->items()) {
        if (IsCommentKey(name)) continue;
        out.emplace_back(name, &item);
    }
    return out;
}

Reader Reader::Child(const nlohmann::json& node, const std::string& segment) const {
    return Reader(node, file_, Join(segment), warn_);
}

void Reader::ObjectList(std::string_view key, const nlohmann::json& sample,
                        const std::function<void(Reader& item)>& each) {
    Note(key, "object_list");
    if (const auto* list = Find(key)) {
        if (!list->is_array()) Fail(key, "must be a list of objects, got " + Quoted(*list));
        for (std::size_t i = 0; i < list->size(); ++i) {
            const std::string segment = std::string(key) + "[" + std::to_string(i) + "]";
            const auto& node = (*list)[i];
            if (!node.is_object()) Fail(segment, "must be an object, got " + Quoted(node));
            Reader item = Child(node, segment);
            each(item);
        }
    }
    if (!recorder_) return;
    {
        Reader item = Child(sample, std::string(key) + "[]");
        each(item);
    }
    nlohmann::json items = recorder_->Extract(Join(key) + "[].");
    nlohmann::json& field = recorder_->Field(Join(key));
    field["items"] = std::move(items);
    field["item_template"] = sample;
}

void Reader::Finish(UnknownKeys policy) {
    for (const auto& [key, unused] : node_->items()) {
        (void)unused;
        if (IsCommentKey(key) || consumed_.count(key) != 0) continue;
        std::string known;
        for (const auto& name : queried_) known += (known.empty() ? "" : ", ") + name;
        const std::string message =
            "unknown field" + (known.empty() ? std::string() : " (known: " + known + ")");
        if (policy == UnknownKeys::kError) Fail(key, message);
        Warn(key, message + ", ignored");
    }
}

}  // namespace minimal::config
