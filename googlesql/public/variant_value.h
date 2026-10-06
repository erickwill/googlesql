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

#ifndef GOOGLESQL_PUBLIC_VARIANT_VALUE_H_
#define GOOGLESQL_PUBLIC_VARIANT_VALUE_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {

namespace internal {

// VariantValueAdapter adapts a GoogleSQL value to provide views aligned with
// the Variant data model. In addition to storing a `googlesql::Value`, for
// certain types, it provides the following views:
// 1. Primitive: A primitive value is a value that is not an object or
//    array. Examples include int64, string, bool, null, etc. JSON primitive
//    values are also exposed as Primitives.
// 2. Object: An object is an unordered collection of key-value pairs, where the
//    key is a string and the value is a Variant. JSON Objects, Maps with string
//    keys, Protos and Structs are exposed as Object. Each of these types have
//    different semantics when exposed as an Object, but they are all accessed
//    via the same interface.
// 3. Array: An array is an ordered list of Variant values. Any array-like type
//    in GoogleSQL (e.g., JSON Array, Array of any type) is exposed
//    as an Array.
// Note that the above views are not applicable to all types that can be stored
// in a Variant. For example, a MAP with non-string keys is not exposed as an
// any of the above views, in which case, only the underlying `googlesql::Value`
// can be used.
// Invariant: For any variant value, at most one of IsPrimitive(), IsObject(),
//            and IsArray() is true.
//
// This class provides ergonomic accessors aligned with the Variant View
// Framework (Primitive, Object, Array) described above.
//
// Variant Null vs SQL NULL:
// There are two kinds of nulls within the Variant model:
// 1. SQL NULL: The absence of a value (e.g., untyped NULL or typed SQL NULL
//    like `Value::NullInt64()`). Checked via `is_null()`.
// 2. Variant null: It is a primitive value Similar to JSON `null` is a valid
//    value. Checked via `IsVariantNull()`. It is considered a primitive;
//    therefore `IsPrimitive()` returns true for it, while `IsObject()` and
//    `IsArray()` return false.
//
// In this view, when accessing child fields or
// elements in Arrays, Structs, Maps, or Protos, any encountered SQL NULL is
// exposed as a Variant null.
//
class VariantValueAdapter {
 public:
  explicit VariantValueAdapter(Value value);

  // Returns whether the Variant is valid. It would be invalid if
  // it was assigned an invalid value.
  bool is_valid() const;
  // Returns true if the Variant is SQL NULL. A SQL Variant means the underlying
  // value is an untyped NULL or a typed NULL.
  bool is_null() const;

  // Returns whether the Variant represents a primitive value. Returns false if
  // the Variant is invalid or SQL NULL.
  bool IsPrimitive() const;
  // Returns whether the Variant represents an object. Returns false if the
  // Variant is invalid or SQL NULL.
  bool IsObject() const;
  // Returns whether the Variant represents an array. Returns false if the
  // Variant is invalid or SQL NULL.
  bool IsArray() const;

  // Variant nulls are distinct from SQL NULL Variants. These can be created by
  // storing a JSON null inside a Variant, or accessing a SQL NULL element /
  // field inside an Array or Object. Returns false if the Variant is invalid or
  // SQL NULL.
  bool IsVariantNull() const;

  // If the Variant represents the underlying value as a primitive, returns the
  // primitive value. Returns an error if the Variant is not a primitive.
  absl::StatusOr<Value> GetPrimitiveValue() const;

  // If the Variant represents the underlying value as an object,
  // returns the keys of the object.
  // Requires IsObject() to be true. Otherwise, returns an error.
  // TODO: DO NOT USE. This function would be removed because
  // it provides no benefit over GetMembers().
  absl::StatusOr<std::vector<std::string>> GetKeys() const;

  // If the Variant represents the underlying value as an object,
  // returns all key-value pairs (members) of the object. Only the top-level
  // object is unpacked; nested objects are returned as VariantValueAdapter
  // instances without recursive unpacking.
  // Requires IsObject() to be true. Otherwise, returns an error.
  // TODO: Support complex proto fields (nested messages, enums).
  absl::StatusOr<std::vector<std::pair<std::string, VariantValueAdapter>>>
  GetMembers() const;

  // If the Variant represents the underlying value as an object,
  // returns whether the key exists in the object.
  // If the Variant is not an object, returns false.
  bool HasKey(absl::string_view key) const;

  // If the Variant represents the underlying value as an object,
  // returns the value associated with the key.
  // Returns std::nullopt if the key is not found.
  // Requires IsObject() is true otherwise returns an error.
  // TODO: Support complex proto fields (nested messages, enums).
  absl::StatusOr<std::optional<VariantValueAdapter>> GetKeyValue(
      absl::string_view key) const;

  // If the Variant represents the underlying value as an object,
  // returns the value associated with the key.
  // Returns an error if the key is not found.
  // Requires IsObject() is true otherwise returns an error.
  // TODO: Support complex proto fields (nested messages, enums).
  absl::StatusOr<VariantValueAdapter> GetKeyValueIfExists(
      absl::string_view key) const;

  // If the Variant represents the underlying value as an array,
  // returns the size of the array.
  // Requires IsArray() to be true. Otherwise, returns an error.
  absl::StatusOr<int> GetArraySize() const;

  // If the Variant represents the underlying value as an array,
  // returns all elements of the array. Only the top-level elements are
  // unpacked;
  // nested arrays or objects are returned as VariantValueAdapter instances
  // without recursive unpacking.
  // Requires IsArray() to be true. Otherwise, returns an error.
  absl::StatusOr<std::vector<VariantValueAdapter>> GetElements() const;

  // If the Variant represents the underlying value as an array,
  // returns the element at the given index.
  // Returns std::nullopt if the index is out of bounds.
  // Requires IsArray() to be true. Otherwise, returns an error.
  absl::StatusOr<std::optional<VariantValueAdapter>> GetElement(
      int index) const;

  // If the Variant represents the underlying value as an array,
  // returns the element at the given index.
  // Returns an error if the index is out of bounds.
  // Requires IsArray() to be true. Otherwise, returns an error.
  absl::StatusOr<VariantValueAdapter> GetElementIfExists(int index) const;

  // Returns a debug string representation of the Variant value projected
  // through the Simple View.
  // For Primitive view, the underlying value's DebugString is returned.
  // For Array view, the elements are recursively converted to
  // their DebugString representations.
  // For Object view, the values are recursively converted to
  // their DebugString representations. The key-value pairs are
  // lexicographically sorted by key.
  // SQL NULL Variants are represented as "NULL" while Variant Nulls are
  // represented as "null".
  std::string DebugString() const;

 private:
  Value value_;
};

}  // namespace internal

// VariantValueView is an alias for internal::VariantValueAdapter. This alias
// is intended to be used in public APIs such that internal details of the
// VariantValueAdapter are hidden.
using VariantValueView = internal::VariantValueAdapter;

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_VARIANT_VALUE_H_
