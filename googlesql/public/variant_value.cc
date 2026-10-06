//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "googlesql/public/variant_value.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "googlesql/public/json_value.h"
#include "googlesql/public/proto/type_annotation.pb.h"
#include "googlesql/public/proto_util.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/public/type.pb.h"
#include "googlesql/public/types/proto_type.h"
#include "googlesql/public/value.h"
#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"
#include "google/protobuf/descriptor.h"

namespace googlesql {

namespace {

Value VariantNull() { return Value::Json(JSONValue()); }

// Converts SQL NULL to Variant Null. If the value is not SQL NULL, returns the
// value as-is.
Value SqlNullToVariantNull(Value value) {
  return value.is_null() ? VariantNull() : value;
}

// Helper class that holds either an owned `JSONValue` (parsed on the fly from
// an unparsed string) or a borrowed `JSONValueConstRef`. It provides uniform,
// read-only access via `JSONValueConstRef` in both cases.
// `val` must be a non-null JSON Value. If `val` holds unparsed JSON, the
// Create method will attempt to parse it and return an error if parsing fails
// or if the value is null.
class ParsedJSONConstRef {
 public:
  static absl::StatusOr<ParsedJSONConstRef> Create(const Value& val) {
    if (!val.type()->IsJson()) {
      return absl::InvalidArgumentError("Value is not valid JSON");
    }
    if (val.is_unparsed_json()) {
      GOOGLESQL_ASSIGN_OR_RETURN(JSONValue json,
                       JSONValue::ParseJSONString(val.json_value_unparsed()));
      return ParsedJSONConstRef(std::move(json));
    }
    if (val.is_null()) {
      return absl::InvalidArgumentError("Value is null JSON");
    }
    return ParsedJSONConstRef(val.json_value());
  }

  JSONValueConstRef ref() const {
    return parsed_storage_.has_value() ? parsed_storage_->GetConstRef()
                                       : *borrowed_ref_;
  }

 private:
  explicit ParsedJSONConstRef(JSONValue json)
      : parsed_storage_(std::move(json)) {}
  explicit ParsedJSONConstRef(JSONValueConstRef ref) : borrowed_ref_(ref) {}

  std::optional<JSONValue> parsed_storage_;
  std::optional<JSONValueConstRef> borrowed_ref_;
};

// Tag structs for supported object types in the Variant View Framework:
// - ObjectTagStruct: Dispatches object operations (GetKeys, GetMembers,
//                    GetKeyValue) on SQL STRUCTs.
// - ObjectTagProto:  Dispatches object operations on PROTO values.
// - ObjectTagMap:    Dispatches object operations on MAP values with string
//                    keys.
// - ObjectTagJSON:   Dispatches object operations on JSON objects, carrying a
//                    ParsedJsonConstRef to avoid re-parsing unparsed JSON.
//
// These tags are grouped into ObjectTagVariant and dispatched via std::visit to
// provide a single source of truth for object discrimination and uniform
// access.
// Each of these tags implement the following functions:
//
//   Returns all the keys of the given Object.
//   absl::StatusOr<std::vector<std::string>> GetKeys(const Value& val) const;
//
//   Returns all key-value pairs (members) of the given Object. Only the
//   top-level Object is unpacked; nested Objects are returned as
//   VariantValueAdapter instances without recursive unpacking.
//   absl::StatusOr<std::vector<std::pair<std::string, VariantValueAdapter>>>
//   GetMembers(const Value& val) const;
//
//   Returns the value associated with the key in the Object.
//   Returns std::nullopt if the key is not found.
//   Requires IsObject() is true otherwise returns an error.
//   absl::StatusOr<std::optional<VariantValueAdapter>> GetKeyValue(
//       const Value& val, absl::string_view key) const;
//
// Note that every tag has its own semantics when implementing these
// functions. Those are explained in the comments of each tag.

struct ObjectTagStruct {
  // Returns the keys of a STRUCT value.
  // Unnamed fields are included as empty string keys.
  // If there are duplicate keys, only the first one is returned.
  absl::StatusOr<std::vector<std::string>> GetKeys(const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsStruct());
    std::vector<std::string> keys;
    const StructType* struct_type = val.type()->AsStruct();
    // We reserve space for all fields, but the number of keys actually returned
    // may be smaller if there are duplicate keys.
    keys.reserve(struct_type->num_fields());
    absl::flat_hash_set<std::string> seen_keys;
    for (int i = 0; i < struct_type->num_fields(); ++i) {
      const std::string& name = struct_type->field(i).name;
      if (seen_keys.insert(name).second) {
        keys.push_back(name);
      }
    }
    return keys;
  }

  // Returns the key-value pairs (members) of a STRUCT value.
  // Unnamed fields are included with empty string keys.
  // If there are duplicate keys, only the first one is returned.
  // If a field value is SQL NULL, it is exposed as a Variant Null.
  absl::StatusOr<
      std::vector<std::pair<std::string, internal::VariantValueAdapter>>>
  GetMembers(const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsStruct());
    std::vector<std::pair<std::string, internal::VariantValueAdapter>> members;
    const StructType* struct_type = val.type()->AsStruct();
    // We reserve space for all fields, but the number of members actually
    // returned may be smaller if there are duplicate keys.
    members.reserve(struct_type->num_fields());
    absl::flat_hash_set<std::string> seen_keys;
    for (int i = 0; i < struct_type->num_fields(); ++i) {
      const std::string& name = struct_type->field(i).name;
      if (seen_keys.insert(name).second) {
        Value child_val = SqlNullToVariantNull(val.field(i));
        members.emplace_back(
            name, internal::VariantValueAdapter(std::move(child_val)));
      }
    }
    return members;
  }

  // Returns the member of a STRUCT value for the given key. Allows accessing
  // unnamed fields via empty string key. The names are matched case-sensitively
  // Returns the first match for duplicate keys.
  // If the value found against a key is SQL NULL, returns a Variant Null.
  absl::StatusOr<std::optional<internal::VariantValueAdapter>> GetKeyValue(
      const Value& val, absl::string_view key) const {
    GOOGLESQL_RET_CHECK(val.type()->IsStruct());
    const StructType* struct_type = val.type()->AsStruct();
    for (int i = 0; i < struct_type->num_fields(); ++i) {
      if (struct_type->field(i).name == key) {
        Value child_val = SqlNullToVariantNull(val.field(i));
        return internal::VariantValueAdapter(std::move(child_val));
      }
    }
    return std::nullopt;
  }
};

struct ObjectTagProto {
  // Extracts a single field from a PROTO value as a VariantValueAdapter.
  // If the field value is not set, uses the default value according to default
  // options. If the resulting value is SQL NULL, returns a Variant Null.
  // TODO: Support complex types (nested messages, enums) for
  // Proto.
  static absl::StatusOr<internal::VariantValueAdapter> ReadFieldValue(
      const Value& val, const google::protobuf::FieldDescriptor* field) {
    FieldFormat::Format format = ProtoType::GetFormatAnnotation(field);
    TypeKind kind;
    GOOGLESQL_RETURN_IF_ERROR(ProtoType::FieldDescriptorToTypeKindBase(field, &kind));

    const Type* gsql_type = nullptr;
    if (field->is_repeated()) {
      gsql_type = types::ArrayTypeFromSimpleTypeKind(kind);
    } else {
      gsql_type = types::TypeFromSimpleTypeKind(kind);
    }

    if (gsql_type == nullptr) {
      return absl::UnimplementedError(
          absl::StrCat("Cannot extract nested field '", field->name(),
                       "' of type '", field->type_name(), "' from Object"));
    }

    Value default_value;
    GOOGLESQL_RETURN_IF_ERROR(GetProtoFieldDefault(ProtoFieldDefaultOptions{}, field,
                                         gsql_type, &default_value));

    Value output_value;
    GOOGLESQL_RETURN_IF_ERROR(ReadProtoField(field, format, gsql_type, default_value,
                                   val.proto_value(), &output_value));
    Value child_val = SqlNullToVariantNull(std::move(output_value));
    return internal::VariantValueAdapter(std::move(child_val));
  }

  // Returns the keys (field names) of a PROTO value.
  absl::StatusOr<std::vector<std::string>> GetKeys(const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsProto());
    std::vector<std::string> keys;
    const google::protobuf::Descriptor* descriptor = val.type()->AsProto()->descriptor();
    keys.reserve(descriptor->field_count());
    for (int i = 0; i < descriptor->field_count(); ++i) {
      keys.push_back(std::string(descriptor->field(i)->name()));
    }
    return keys;
  }

  // Returns the key-value pairs (members) of a PROTO value.
  // TODO: Support complex types (nested messages, enums) for
  // Proto.
  absl::StatusOr<
      std::vector<std::pair<std::string, internal::VariantValueAdapter>>>
  GetMembers(const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsProto());
    const google::protobuf::Descriptor* descriptor = val.type()->AsProto()->descriptor();
    std::vector<std::pair<std::string, internal::VariantValueAdapter>> members;
    members.reserve(descriptor->field_count());
    for (int i = 0; i < descriptor->field_count(); ++i) {
      const google::protobuf::FieldDescriptor* field = descriptor->field(i);
      GOOGLESQL_ASSIGN_OR_RETURN(internal::VariantValueAdapter adapter,
                       ReadFieldValue(val, field));
      members.emplace_back(std::string(field->name()), std::move(adapter));
    }
    return members;
  }

  // Returns the member of a PROTO value for the given key (field name). If the
  // value is not set, returns the default value for the field is set. Default
  // Options are used to determine the default value.
  // If the value found against a key is SQL NULL, returns a Variant Null.
  // TODO: Support complex types (nested messages, enums) for
  // Proto.
  absl::StatusOr<std::optional<internal::VariantValueAdapter>> GetKeyValue(
      const Value& val, absl::string_view key) const {
    GOOGLESQL_RET_CHECK(val.type()->IsProto());
    const google::protobuf::Descriptor* descriptor = val.type()->AsProto()->descriptor();
    const google::protobuf::FieldDescriptor* field = descriptor->FindFieldByName(key);
    if (field != nullptr) {
      GOOGLESQL_ASSIGN_OR_RETURN(internal::VariantValueAdapter adapter,
                       ReadFieldValue(val, field));
      return adapter;
    }
    return std::nullopt;
  }
};

struct ObjectTagMap {
  // Returns the keys of a MAP value. SQL NULL keys are not returned.
  absl::StatusOr<std::vector<std::string>> GetKeys(const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsMap() &&
              val.type()->AsMap()->key_type()->IsString());
    std::vector<std::string> keys;
    if (val.type()->AsMap()->key_type()->IsString()) {
      for (const auto& entry : val.map_entries()) {
        if (!entry.first.is_null()) {
          keys.push_back(entry.first.string_value());
        }
      }
    }
    return keys;
  }

  // Returns the key-value pairs (members) of a MAP value. SQL NULL keys are
  // omitted, and SQL NULL values are returned as Variant Null.
  absl::StatusOr<
      std::vector<std::pair<std::string, internal::VariantValueAdapter>>>
  GetMembers(const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsMap() &&
              val.type()->AsMap()->key_type()->IsString());
    std::vector<std::pair<std::string, internal::VariantValueAdapter>> members;
    for (const auto& [key, value] : val.map_entries()) {
      if (!key.is_null()) {
        Value child_val = SqlNullToVariantNull(value);
        members.emplace_back(key.string_value(), internal::VariantValueAdapter(
                                                     std::move(child_val)));
      }
    }
    return members;
  }

  // Returns the member of a MAP value for the given key. SQL NULL values are
  // returned as Variant Null.
  absl::StatusOr<std::optional<internal::VariantValueAdapter>> GetKeyValue(
      const Value& val, absl::string_view key) const {
    GOOGLESQL_RET_CHECK(val.type()->IsMap() &&
              val.type()->AsMap()->key_type()->IsString());
    auto map = val.map_entries();
    auto it = map.find(Value::String(key));
    if (it != map.end()) {
      Value child_val = SqlNullToVariantNull(it->second);
      return internal::VariantValueAdapter(std::move(child_val));
    }
    return std::nullopt;
  }
};

struct ObjectTagJSON {
  ParsedJSONConstRef json;

  // Returns the keys of a JSON value.
  absl::StatusOr<std::vector<std::string>> GetKeys(const Value& val) const {
    std::vector<std::string> keys;
    keys.reserve(json.ref().GetObjectSize());
    for (const auto& [key, value] : json.ref().GetMembers()) {
      keys.push_back(std::string(key));
    }
    return keys;
  }

  // Returns the key-value pairs (members) of a JSON value.
  absl::StatusOr<
      std::vector<std::pair<std::string, internal::VariantValueAdapter>>>
  GetMembers(const Value& val) const {
    std::vector<std::pair<std::string, internal::VariantValueAdapter>> members;
    members.reserve(json.ref().GetObjectSize());
    for (const auto& [key, value] : json.ref().GetMembers()) {
      members.emplace_back(std::string(key),
                           internal::VariantValueAdapter(
                               Value::Json(JSONValue::CopyFrom(value))));
    }
    return members;
  }

  // Returns the member of a JSON value for the given key.
  absl::StatusOr<std::optional<internal::VariantValueAdapter>> GetKeyValue(
      const Value& val, absl::string_view key) const {
    auto member = json.ref().GetMemberIfExists(key);
    if (member.has_value()) {
      return internal::VariantValueAdapter(
          Value::Json(JSONValue::CopyFrom(*member)));
    }
    return std::nullopt;
  }
};

// Tag structs for supported array types in the Variant View Framework:
// - ArrayTagSql:  Dispatches array operations on SQL ARRAYs.
// - ArrayTagJSON: Dispatches array operations on JSON arrays, carrying a
//                 ParsedJSONConstRef to avoid re-parsing unparsed JSON.
//
// These tags are grouped into ArrayTagVariant and dispatched via std::visit to
// provide a single source of truth for array discrimination and uniform
// access.
// Each of these tags implement the following functions:
//
//   Returns the size of the given Array.
//   absl::StatusOr<int> GetSize(const Value& val) const;
//
//   Returns all elements of the given Array. Only the top-level elements are
//   unpacked; nested Arrays or Objects are returned as VariantValueAdapter
//   instances without recursive unpacking.
//   absl::StatusOr<std::vector<VariantValueAdapter>> GetElements(
//       const Value& val) const;
//
//   Returns the element at the given index in the Array.
//   Returns std::nullopt if the index is out of bounds.
//   absl::StatusOr<std::optional<VariantValueAdapter>> GetElement(
//       const Value& val, int index) const;

struct ArrayTagSql {
  int GetSize(const Value& val) const { return val.num_elements(); }

  // Returns all elements in the Array.
  // If an element is SQL NULL, it is exposed as a Variant Null.
  absl::StatusOr<std::vector<internal::VariantValueAdapter>> GetElements(
      const Value& val) const {
    GOOGLESQL_RET_CHECK(val.type()->IsArray());
    std::vector<internal::VariantValueAdapter> elements;
    elements.reserve(val.num_elements());
    for (int i = 0; i < val.num_elements(); ++i) {
      Value child_val = SqlNullToVariantNull(val.element(i));
      elements.emplace_back(std::move(child_val));
    }
    return elements;
  }

  // Returns the element at the given index in the Array.
  // If the value found at the index is SQL NULL, returns a Variant Null.
  absl::StatusOr<std::optional<internal::VariantValueAdapter>> GetElement(
      const Value& val, int index) const {
    if (index < 0 || index >= val.num_elements()) {
      return std::nullopt;
    }
    Value child_val = SqlNullToVariantNull(val.element(index));
    return internal::VariantValueAdapter(std::move(child_val));
  }
};

struct ArrayTagJSON {
  ParsedJSONConstRef json;

  // Returns the size of the JSON array.
  int GetSize(const Value& val) const {
    return static_cast<int>(json.ref().GetArraySize());
  }

  // Returns all elements in the JSON array.
  absl::StatusOr<std::vector<internal::VariantValueAdapter>> GetElements(
      const Value& val) const {
    JSONValueConstRef json_ref = json.ref();
    std::vector<internal::VariantValueAdapter> elements;
    elements.reserve(json_ref.GetArraySize());
    for (const JSONValueConstRef& elem : json_ref.GetArrayElements()) {
      elements.emplace_back(Value::Json(JSONValue::CopyFrom(elem)));
    }
    return elements;
  }

  // Returns the element at the given index in the JSON array.
  absl::StatusOr<std::optional<internal::VariantValueAdapter>> GetElement(
      const Value& val, int index) const {
    JSONValueConstRef json_ref = json.ref();
    if (index < 0 || index >= json_ref.GetArraySize()) {
      return std::nullopt;
    }
    return internal::VariantValueAdapter(
        Value::Json(JSONValue::CopyFrom(json_ref.GetArrayElement(index))));
  }
};

using ArrayTagVariant = std::variant<ArrayTagSql, ArrayTagJSON>;
using ObjectTagVariant =
    std::variant<ObjectTagStruct, ObjectTagProto, ObjectTagMap, ObjectTagJSON>;

absl::StatusOr<ObjectTagVariant> GetObjectTag(const Value& value) {
  GOOGLESQL_RET_CHECK(value.is_valid());
  if (!value.is_null()) {
    if (value.type()->IsStruct()) {
      return ObjectTagStruct{};
    }
    if (value.type()->IsProto()) {
      return ObjectTagProto{};
    }
    if (value.type()->IsMap()) {
      if (value.type()->AsMap()->key_type()->IsString()) {
        return ObjectTagMap{};
      }
      return absl::InvalidArgumentError("Not an object");
    }
    if (value.type()->IsJson()) {
      GOOGLESQL_ASSIGN_OR_RETURN(ParsedJSONConstRef json,
                       ParsedJSONConstRef::Create(value));
      if (json.ref().IsObject()) {
        return ObjectTagJSON{std::move(json)};
      }
      return absl::InvalidArgumentError("Not an object");
    }
  }
  return absl::InvalidArgumentError("Not an object");
}

absl::StatusOr<ArrayTagVariant> GetArrayTag(const Value& value) {
  GOOGLESQL_RET_CHECK(value.is_valid());
  if (!value.is_null()) {
    if (value.type()->IsArray()) {
      return ArrayTagSql{};
    }
    if (value.type()->IsJson()) {
      GOOGLESQL_ASSIGN_OR_RETURN(ParsedJSONConstRef parsed_json,
                       ParsedJSONConstRef::Create(value));
      if (parsed_json.ref().IsArray()) {
        return ArrayTagJSON{std::move(parsed_json)};
      }
    }
  }
  return absl::InvalidArgumentError("Not an array");
}

}  // namespace

namespace internal {

// --- VariantValueAdapter ---

VariantValueAdapter::VariantValueAdapter(Value value)
    : value_(std::move(value)) {}

bool VariantValueAdapter::is_valid() const { return value_.is_valid(); }

bool VariantValueAdapter::is_null() const {
  return value_.is_valid() && value_.is_null();
}

bool VariantValueAdapter::IsPrimitive() const {
  if (!is_valid() || is_null()) return false;
  if (value_.type()->IsJson()) {
    auto parsed_json = ParsedJSONConstRef::Create(value_);
    if (!parsed_json.ok()) return false;
    JSONValueConstRef json_ref = parsed_json->ref();
    return json_ref.IsNumber() || json_ref.IsString() || json_ref.IsBoolean() ||
           json_ref.IsNull();
  }
  return value_.type()->IsSimpleType() || value_.type()->IsEnum();
}

bool VariantValueAdapter::IsObject() const {
  if (!is_valid() || is_null()) return false;
  if (value_.type()->IsJson()) {
    auto parsed_json = ParsedJSONConstRef::Create(value_);
    if (!parsed_json.ok()) return false;
    return parsed_json->ref().IsObject();
  }
  if (value_.type()->IsMap()) {
    return value_.type()->AsMap()->key_type()->IsString();
  }
  return value_.type()->IsStruct() || value_.type()->IsProto();
}

bool VariantValueAdapter::IsArray() const { return GetArrayTag(value_).ok(); }

bool VariantValueAdapter::IsVariantNull() const {
  if (!is_valid() || is_null()) return false;
  if (value_.type()->IsJson()) {
    auto parsed_json = ParsedJSONConstRef::Create(value_);
    if (!parsed_json.ok()) return false;
    return parsed_json->ref().IsNull();
  }
  return false;
}

absl::StatusOr<Value> VariantValueAdapter::GetPrimitiveValue() const {
  if (!IsPrimitive()) {
    return absl::InvalidArgumentError("Value is not a primitive.");
  }

  if (value_.type()->IsJson()) {
    GOOGLESQL_ASSIGN_OR_RETURN(ParsedJSONConstRef parsed_json,
                     ParsedJSONConstRef::Create(value_));
    JSONValueConstRef json_ref = parsed_json.ref();
    if (json_ref.IsInt64()) return Value::Int64(json_ref.GetInt64());
    if (json_ref.IsUInt64()) return Value::Uint64(json_ref.GetUInt64());
    if (json_ref.IsDouble()) return Value::Double(json_ref.GetDouble());
    if (json_ref.IsString()) {
      return Value::String(json_ref.GetStringRef());
    }
    if (json_ref.IsBoolean()) return Value::Bool(json_ref.GetBoolean());
    if (json_ref.IsNull()) return VariantNull();
    return absl::InternalError("Unexpected JSON primitive type");
  }
  return value_;
}

absl::StatusOr<std::vector<std::string>> VariantValueAdapter::GetKeys() const {
  GOOGLESQL_ASSIGN_OR_RETURN(ObjectTagVariant tag, GetObjectTag(value_));
  return std::visit(
      [this](const auto& tag_val) { return tag_val.GetKeys(value_); }, tag);
}

absl::StatusOr<std::vector<std::pair<std::string, VariantValueAdapter>>>
VariantValueAdapter::GetMembers() const {
  GOOGLESQL_ASSIGN_OR_RETURN(ObjectTagVariant tag, GetObjectTag(value_));
  return std::visit(
      [this](const auto& tag_val) { return tag_val.GetMembers(value_); }, tag);
}

bool VariantValueAdapter::HasKey(absl::string_view key) const {
  // TODO: Consider supporting HasKey directly in the ObjectTag
  // interface to avoid the overhead of GetKeyValue().
  auto val_or = GetKeyValue(key);
  return val_or.ok() && val_or->has_value();
}

absl::StatusOr<std::optional<VariantValueAdapter>>
VariantValueAdapter::GetKeyValue(absl::string_view key) const {
  GOOGLESQL_ASSIGN_OR_RETURN(ObjectTagVariant tag, GetObjectTag(value_));
  return std::visit(
      [this, key](const auto& tag_val) {
        return tag_val.GetKeyValue(value_, key);
      },
      tag);
}

absl::StatusOr<VariantValueAdapter> VariantValueAdapter::GetKeyValueIfExists(
    absl::string_view key) const {
  GOOGLESQL_ASSIGN_OR_RETURN(std::optional<VariantValueAdapter> val, GetKeyValue(key));
  if (!val.has_value()) {
    return absl::NotFoundError(absl::StrCat("Key not found: ", key));
  }
  return *val;
}

absl::StatusOr<int> VariantValueAdapter::GetArraySize() const {
  GOOGLESQL_RET_CHECK(is_valid());
  GOOGLESQL_ASSIGN_OR_RETURN(ArrayTagVariant tag, GetArrayTag(value_));
  return std::visit(
      [this](const auto& tag_val) { return tag_val.GetSize(value_); }, tag);
}

absl::StatusOr<std::vector<VariantValueAdapter>>
VariantValueAdapter::GetElements() const {
  GOOGLESQL_RET_CHECK(is_valid());
  GOOGLESQL_ASSIGN_OR_RETURN(ArrayTagVariant tag, GetArrayTag(value_));
  return std::visit(
      [this](const auto& tag_val) { return tag_val.GetElements(value_); }, tag);
}

absl::StatusOr<std::optional<VariantValueAdapter>>
VariantValueAdapter::GetElement(int index) const {
  GOOGLESQL_RET_CHECK(is_valid());
  GOOGLESQL_ASSIGN_OR_RETURN(ArrayTagVariant tag, GetArrayTag(value_));
  return std::visit(
      [this, index](const auto& tag_val) {
        return tag_val.GetElement(value_, index);
      },
      tag);
}

absl::StatusOr<VariantValueAdapter> VariantValueAdapter::GetElementIfExists(
    int index) const {
  GOOGLESQL_ASSIGN_OR_RETURN(std::optional<VariantValueAdapter> val, GetElement(index));
  if (!val.has_value()) {
    return absl::OutOfRangeError(absl::StrCat("Index out of bounds: ", index));
  }
  return *val;
}

std::string VariantValueAdapter::DebugString() const {
  if (!is_valid() || is_null()) {
    return value_.DebugString();
  }
  if (absl::StatusOr<Value> primitive_val = GetPrimitiveValue();
      primitive_val.ok()) {
    return primitive_val->DebugString();
  }
  if (absl::StatusOr<std::vector<std::pair<std::string, VariantValueAdapter>>>
          members = GetMembers();
      members.ok()) {
    absl::c_sort(*members, [](const auto& a, const auto& b) {
      return a.first < b.first;
    });
    return absl::StrCat("{",
                        absl::StrJoin(*members, ", ",
                                      [](std::string* out, const auto& entry) {
                                        absl::StrAppend(
                                            out, ToStringLiteral(entry.first),
                                            ": ", entry.second.DebugString());
                                      }),
                        "}");
  }
  if (absl::StatusOr<std::vector<VariantValueAdapter>> elements = GetElements();
      elements.ok()) {
    return absl::StrCat("[",
                        absl::StrJoin(*elements, ", ",
                                      [](std::string* out, const auto& elem) {
                                        absl::StrAppend(out,
                                                        elem.DebugString());
                                      }),
                        "]");
  }
  return value_.DebugString();
}

}  // namespace internal

}  // namespace googlesql
