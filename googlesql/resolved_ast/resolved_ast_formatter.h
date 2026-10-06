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

#ifndef GOOGLESQL_RESOLVED_AST_RESOLVED_AST_FORMATTER_H_
#define GOOGLESQL_RESOLVED_AST_RESOLVED_AST_FORMATTER_H_

// The Resolved AST tree printing framework separates structural navigation and
// view abstraction from string formatting. It consists of two primary layers:
//
// Non-Owning View Layer:
//   Provides non-owning views (ResolvedASTNodeView, ResolvedASTFieldView) over
//   ResolvedNode instances and their fields. The overall model is a tree
//   structure where each node has a list of fields, and each field represents
//   either a scalar value or a list of child nodes.
//
//   These view classes expose properties (scalar values, is_column_created bits
//   etc.) and support field and child node iteration, without any formatting or
//   layout logic. Instances of these views are created lazily when needed. In
//   general, creating these views is very cheap, except that while constructing
//   ResolvedASTNodeView we also gather all its fields eagerly.
//
// Tree Renderer Layer (RenderResolvedAST):
//   Standalone rendering function that converts ResolvedASTNodeView trees into
//   ASCII or Unicode box-drawing character tree representations (supporting
//   both standard hierarchical and linear pipeline chains.

#include <optional>
#include <string>
#include <vector>

#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/base/nullability.h"
#include "absl/strings/string_view.h"

namespace googlesql {

class ResolvedASTFieldView;
class ResolvedScan;

// Non-owning view of a ResolvedNode. This view can wrap a null node, and all
// methods work correctly in that case.
class ResolvedASTNodeView {
 public:
  // Constructs a view for `node` using `config`.
  explicit ResolvedASTNodeView(const ResolvedNode* /*absl_nullable*/ node);

  bool is_null() const { return node_ == nullptr; }

  const ResolvedNode* /*absl_nullable*/ node() const { return node_; }

  // Returns this node's kind (e.g. RESOLVED_LITERAL, RESOLVED_FILTER_SCAN).
  // Returns std::nullopt if node is null.
  std::optional<ResolvedNodeKind> node_kind() const;

  // Returns the name string displayed for this node (normally
  // node_kind_string(), but may be customized).
  std::string node_name(
      const ResolvedNode::DebugStringConfig& config = {}) const;

  // Returns the number of fields in this node.
  int num_fields() const { return static_cast<int>(fields_.size()); }

  // Returns the i-th field of this node. The index i must be in the range
  // [0, num_fields()).
  ResolvedASTFieldView field(int i) const;

  // If this node is a ResolvedScan, this returns its pipe input scan by calling
  // ResolvedScan::GetPipeInputScan(). Otherwise, returns nullptr.
  const ResolvedScan* /*absl_nullable*/ pipe_input_scan() const;

  // Returns the sequence of scans forming the linear pipeline chain leading up
  // to and including this scan.
  // If this node is a ResolvedScan with upstream pipe inputs, returns
  // [base_source_scan, pipe_op_1, pipe_op_2, ..., this_scan].
  // Otherwise, returns a single-element vector containing *this.
  std::vector<ResolvedASTNodeView> GetLinearPipelineChain() const;

 private:
  const ResolvedNode* /*absl_nullable*/ node_;
  std::vector<ResolvedNode::DebugStringField> fields_;
};

// Non-owning view of an individual field of a ResolvedNode. This view can only
// be constructed for a non-null field.
class ResolvedASTFieldView {
 public:
  // Constructs a view for `field` using `config`.
  explicit ResolvedASTFieldView(const ResolvedNode::DebugStringField& field);

  // Field name (e.g., "column", "argument_list", "expr", "type").
  absl::string_view name() const { return field_->name; }

  // Returns true if this field represents a ResolvedColumn created in the
  // parent node. This comes from column_is_created on the Field definition or
  // ColumnListIsCreatedColumns() for scan nodes.
  bool is_column_created() const { return field_->column_created; }

  // If this is a semantically meaningful (not ignorable) field, is_accessed()
  // indicates whether the field has been accessed. If a semantically meaningful
  // field has not been accessed by the consumer of the tree, it indicates that
  // some feature is unimplemented, and the ResolvedNode will fail
  // CheckFieldsAccessed(). If DebugStringConfig::print_accessed is true, then a
  // "{*}" marker will be printed next to the field name if it has been
  // accessed, and "{ }" if it has not.
  bool is_accessed() const { return field_->accessed; }

  // Returns true if this field holds a scalar value rather than child nodes.
  bool is_scalar() const { return field_->nodes.empty(); }

  // Scalar string value for the Resolved AST field. (e.g., \"foo\", "1", or raw
  // multiline text).
  //
  // E.g. the node Literal(type=STRING, value="foo") has two fields:
  //   - name: "type", scalar_value: "STRING"
  //   - name: "value", scalar_value: \"foo\"
  absl::string_view scalar_value() const { return field_->value; }

  // Returns the number of child nodes under this field (including null fields).
  // For non-repeated nodes, this is 0 or 1. For repeated nodes, this is the
  // number of elements in the repeated field.
  int num_child_nodes() const { return static_cast<int>(field_->nodes.size()); }

  // Returns the i-th child node of this field. Returns a null view if i is out
  // of bounds. For optional nodes, this will always be called with i = 0.
  ResolvedASTNodeView child_node(int i) const;

  // Returns true if this field holds `pipe_input_to_elide` as its single child
  // node.
  bool IsPipeInputScan(
      const ResolvedScan* /*absl_nullable*/ pipe_input_to_elide) const;

 private:
  const ResolvedNode::DebugStringField* /*absl_nonnull*/ field_;
};

// Renders the Resolved AST as a string using the config. This is the
// implementation of ResolvedNode::DebugString().
std::string RenderResolvedAST(const ResolvedNode* root,
                              const ResolvedNode::DebugStringConfig& config);

}  // namespace googlesql

#endif  // GOOGLESQL_RESOLVED_AST_RESOLVED_AST_FORMATTER_H_
