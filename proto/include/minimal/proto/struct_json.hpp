#pragma once

// JSON <-> google.protobuf.Struct, the one conversion server and client share
// (tool input crosses the wire as a Struct). Header-only: it lives beside the
// generated bindings both sides already link, not in common/, which has no
// protobuf.
//
// Struct's number model is IEEE double -- it has no integer type. So a JSON
// integer crosses as a double and comes back as an integer again (integral
// doubles are narrowed on the way back), which keeps `{"offset": 3}` printing as
// `3` rather than `3.0` in prompts and logs. The cost is the mirror case: a
// deliberate float `3.0` also returns as `3`. That trade is right for this
// payload -- tool input is overwhelmingly integers (offsets, limits, timeouts)
// and every consumer reads it through nlohmann's numeric coercion.
//
// Only objects can be a Struct. A non-object json (an array or a bare scalar)
// is not representable and yields an empty Struct; tool input is an object by
// schema, so this is a wire-validation case, not a lossy path in practice.
//
// Strings and keys headed onto the wire are scrubbed to valid UTF-8: a proto3
// `string` holding ill-formed UTF-8 makes the peer reject the whole message.

#include <cstdint>
#include <string>

#include <google/protobuf/struct.pb.h>
#include <nlohmann/json.hpp>

#include "minimal/common/numeric.hpp"
#include "minimal/common/utf8.hpp"

namespace minimal::proto {

inline google::protobuf::Value JsonToValue(const nlohmann::json& value) {
    google::protobuf::Value out;
    switch (value.type()) {
        case nlohmann::json::value_t::null:
            out.set_null_value(google::protobuf::NULL_VALUE);
            break;
        case nlohmann::json::value_t::boolean:
            out.set_bool_value(value.get<bool>());
            break;
        case nlohmann::json::value_t::number_integer:
            out.set_number_value(static_cast<double>(value.get<std::int64_t>()));
            break;
        case nlohmann::json::value_t::number_unsigned:
            out.set_number_value(static_cast<double>(value.get<std::uint64_t>()));
            break;
        case nlohmann::json::value_t::number_float:
            out.set_number_value(value.get<double>());
            break;
        case nlohmann::json::value_t::string:
            out.set_string_value(common::ScrubUtf8(value.get_ref<const std::string&>()));
            break;
        case nlohmann::json::value_t::array: {
            auto* list = out.mutable_list_value();
            for (const auto& element : value) *list->add_values() = JsonToValue(element);
            break;
        }
        case nlohmann::json::value_t::object: {
            auto* fields = out.mutable_struct_value()->mutable_fields();
            for (const auto& [key, element] : value.items()) {
                (*fields)[common::ScrubUtf8(key)] = JsonToValue(element);
            }
            break;
        }
        // Binary has no JSON (and so no Struct) representation at all; it cannot
        // arrive from a parsed LLM response. Discarded rather than approximated.
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            out.set_null_value(google::protobuf::NULL_VALUE);
            break;
    }
    return out;
}

inline nlohmann::json ValueToJson(const google::protobuf::Value& value) {
    switch (value.kind_case()) {
        case google::protobuf::Value::kNullValue:
            return nullptr;
        case google::protobuf::Value::kNumberValue: {
            const double number = value.number_value();
            if (common::IsExactInt64(number)) return static_cast<std::int64_t>(number);
            return number;
        }
        case google::protobuf::Value::kStringValue:
            return value.string_value();
        case google::protobuf::Value::kBoolValue:
            return value.bool_value();
        case google::protobuf::Value::kStructValue: {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& [key, field] : value.struct_value().fields()) {
                out[key] = ValueToJson(field);
            }
            return out;
        }
        case google::protobuf::Value::kListValue: {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& element : value.list_value().values()) {
                out.push_back(ValueToJson(element));
            }
            return out;
        }
        case google::protobuf::Value::KIND_NOT_SET:
            break;
    }
    return nullptr;
}

inline google::protobuf::Struct JsonToStruct(const nlohmann::json& value) {
    google::protobuf::Struct out;
    if (!value.is_object()) return out;
    auto* fields = out.mutable_fields();
    for (const auto& [key, element] : value.items()) {
        (*fields)[common::ScrubUtf8(key)] = JsonToValue(element);
    }
    return out;
}

inline nlohmann::json StructToJson(const google::protobuf::Struct& value) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, field] : value.fields()) out[key] = ValueToJson(field);
    return out;
}

}  // namespace minimal::proto
