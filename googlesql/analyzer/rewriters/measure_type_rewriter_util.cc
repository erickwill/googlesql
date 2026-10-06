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

#include "googlesql/analyzer/rewriters/measure_type_rewriter_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/analyzer/annotation_propagator.h"
#include "googlesql/analyzer/rewriters/measure_collector.h"
#include "googlesql/analyzer/rewriters/measure_dependency_graph.h"
#include "googlesql/common/measure_utils.h"
#include "googlesql/public/builtin_function.pb.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/types/annotation.h"
#include "googlesql/public/types/measure_type.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/column_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_builder.h"
#include "googlesql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "googlesql/resolved_ast/resolved_ast_rewrite_visitor.h"
#include "googlesql/resolved_ast/resolved_ast_visitor.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/resolved_ast/rewrite_utils.h"
#include "googlesql/base/case.h"
#include "absl/algorithm/container.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "googlesql/base/check.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {

////////////////////////////////////////////////////////////////////////
// Utility functions.
////////////////////////////////////////////////////////////////////////

bool IsMeasureAggFunction(const ResolvedExpr* expr) {
  if (!expr->Is<ResolvedAggregateFunctionCall>()) {
    return false;
  }
  const ResolvedAggregateFunctionCall* agg_fn =
      expr->GetAs<ResolvedAggregateFunctionCall>();
  const Function* function = agg_fn->function();
  return function->NumSignatures() == 1 &&
         function->signatures()[0].context_id() == FN_AGG &&
         function->IsGoogleSQLBuiltin();
}

absl::StatusOr<ResolvedColumn> GetInvokedMeasureColumn(
    const ResolvedAggregateFunctionCall* aggregate_fn) {
  GOOGLESQL_RET_CHECK(aggregate_fn != nullptr);
  GOOGLESQL_RET_CHECK(aggregate_fn->argument_list().size() == 1);
  const ResolvedExpr* arg = aggregate_fn->argument_list()[0].get();
  GOOGLESQL_RET_CHECK(arg->Is<ResolvedColumnRef>());
  return arg->GetAs<ResolvedColumnRef>()->column();
}

////////////////////////////////////////////////////////////////////////
// Logic to find unsupported query shapes for the measure type rewriter.
////////////////////////////////////////////////////////////////////////

// Returns an error if `input` contains a query shape that is unsupported by
// the measure type rewriter.
class UnsupportedQueryShapeFinder : public ResolvedASTVisitor {
 public:
  static absl::Status HasUnsupportedQueryShape(
      const ResolvedNode* input, const LanguageOptions& language_options) {
    // First, gather information about measure columns that need to be expanded.
    UnsupportedQueryShapeFinder unsupport_query_shape_finder(language_options);
    return input->Accept(&unsupport_query_shape_finder);
  }

  // Find measure columns invoked via the `AGG` function and place them in
  // `invoked_measure_columns_`.
  absl::Status VisitResolvedSubqueryExpr(
      const ResolvedSubqueryExpr* node) override {
    if (node->subquery_type() == ResolvedSubqueryExpr::SCALAR) {
      GOOGLESQL_RET_CHECK(node->subquery()->column_list_size() == 1);
      if (node->subquery()->column_list(0).type()->IsMeasureType()) {
        return absl::UnimplementedError(
            "Measure type rewriter does not support scalar subqueries that "
            "emit measure columns");
      }
    }
    return DefaultVisit(node);
  }

  // TODO: b/350555383  - Support this shape in the future
  absl::Status VisitResolvedWithScan(const ResolvedWithScan* node) override {
    for (const std::unique_ptr<const ResolvedWithEntry>& with_entry :
         node->with_entry_list()) {
      absl::flat_hash_set<ResolvedColumn> projected_cols;
      if (absl::c_any_of(with_entry->with_subquery()->column_list(),
                         [&projected_cols](const ResolvedColumn& column) {
                           return !projected_cols.insert(column).second;
                         })) {
        return absl::UnimplementedError(
            "Measure type rewriter does not support WITH scans emitting "
            "duplicate measure columns");
      }
    }
    return DefaultVisit(node);
  }

  // TODO: b/350555383  - Support this shape in the future
  absl::Status VisitResolvedJoinScan(const ResolvedJoinScan* node) override {
    if (node->is_lateral() &&
        absl::c_any_of(node->column_list(), [](const ResolvedColumn& column) {
          return IsOrContainsMeasure(column.type());
        })) {
      return absl::UnimplementedError(
          "Measure type rewriter does not support LATERAL joins that emit "
          "measure columns");
    }
    return DefaultVisit(node);
  }

  // TODO: b/350555383  - Support this shape in the future
  absl::Status VisitResolvedAggregateFunctionCall(
      const ResolvedAggregateFunctionCall* node) override {
    if (IsMeasureAggFunction(node)) {
      GOOGLESQL_RET_CHECK(node->argument_list().size() == 1);
      const ResolvedExpr* arg = node->argument_list()[0].get();
      const bool allow_get_struct_field =
          language_options_.LanguageFeatureEnabled(FEATURE_MEASURES_IN_STRUCT);
      const bool allow_get_row_field =
          language_options_.LanguageFeatureEnabled(FEATURE_ROW_TYPE);
      const bool is_valid_arg =
          arg->Is<ResolvedColumnRef>() ||
          (allow_get_struct_field && arg->Is<ResolvedGetStructField>()) ||
          (allow_get_row_field && arg->Is<ResolvedGetRowField>());
      if (!is_valid_arg) {
        // The measure rewriter currently assumes that the argument to the AGG
        // function invocation is:
        //
        // - A column reference, or
        // - A struct field access, or
        // - A row field access
        //
        // resolving to a measure.
        //
        // The MeasureColumnRewriter makes this assumption as well, since it
        // skips mapping measure typed columns to closure columns if the measure
        // typed column is rooted within an AGG function call sub-tree. Removing
        // this check will require relaxing that assumption.
        std::string error_message =
            "Measure type rewriter expects argument to AGG function to be a "
            "direct column reference";
        if (allow_get_struct_field) {
          absl::StrAppend(&error_message,
                          " or a struct field access resolving to measure");
        }
        if (allow_get_row_field) {
          absl::StrAppend(&error_message,
                          ", or a row field access resolving to measure");
        }
        return absl::UnimplementedError(error_message);
      }
    }
    return DefaultVisit(node);
  }

  // TODO: b/350555383  - Support this shape in the future
  absl::Status VisitResolvedWithExpr(const ResolvedWithExpr* node) override {
    if (IsOrContainsMeasure(node->expr()->type())) {
      return absl::UnimplementedError(
          "Measure type rewriter does not support WITH expressions emitting a "
          "measure type");
    }
    return DefaultVisit(node);
  }

  // TODO: b/350555383  - Support this shape in the future
  absl::Status VisitResolvedMeasureGroup(
      const ResolvedMeasureGroup* node) override {
    for (const auto& computed_column : node->aggregate_list()) {
      if (IsMeasureAggFunction(computed_column->expr())) {
        return absl::UnimplementedError(
            "Measure type rewriter does not support aggregating measures in a "
            "MATCH_RECOGNIZE scan");
      }
    }
    return DefaultVisit(node);
  }

 private:
  explicit UnsupportedQueryShapeFinder(const LanguageOptions& language_options)
      : language_options_(language_options) {}
  UnsupportedQueryShapeFinder(const UnsupportedQueryShapeFinder&) = delete;
  UnsupportedQueryShapeFinder& operator=(const UnsupportedQueryShapeFinder&) =
      delete;

  const LanguageOptions& language_options_;
};

absl::Status HasUnsupportedQueryShape(const ResolvedNode* input,
                                      const LanguageOptions& language_options) {
  return UnsupportedQueryShapeFinder::HasUnsupportedQueryShape(
      input, language_options);
}

namespace {

// `MultiLevelAggregateRewriter` is a visitor that rewrites aggregate function
// calls to use multi-level aggregation to grain-lock and avoid overcounting.
//
// For example, it rewrites SUM(x) to SUM(ANY_VALUE(x) GROUP BY grain_lock_key),
// where `grain_lock_key` is a constructed from the input `closure_struct_ref`.
class MultiLevelAggregateRewriter : public ResolvedASTRewriteVisitor {
 public:
  MultiLevelAggregateRewriter(
      const Function* any_value_fn, FunctionCallBuilder& function_call_builder,
      const LanguageOptions& language_options, ColumnFactory& column_factory,
      const ResolvedColumnRef* closure_struct_ref,
      const absl::btree_set<std::string, googlesql_base::CaseLess>&
          row_identity_column_names)
      : any_value_fn_(any_value_fn),
        function_call_builder_(function_call_builder),
        language_options_(language_options),
        column_factory_(column_factory),
        closure_struct_ref_(closure_struct_ref),
        row_identity_column_names_(row_identity_column_names) {};
  MultiLevelAggregateRewriter(const MultiLevelAggregateRewriter&) = delete;
  MultiLevelAggregateRewriter& operator=(const MultiLevelAggregateRewriter&) =
      delete;

 protected:
  absl::Status PreVisitResolvedAggregateFunctionCall(
      const googlesql::ResolvedAggregateFunctionCall&) override {
    aggregate_function_depth_++;
    return absl::OkStatus();
  }

  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedAggregateFunctionCall(
      std::unique_ptr<const ResolvedAggregateFunctionCall> node) override {
    auto cleanup = absl::MakeCleanup([this] { aggregate_function_depth_--; });
    // If we are within a subquery, then we don't need to grain-lock the
    // aggregate function.
    if (subquery_depth_ > 0) {
      return node;
    }
    // Inject the WHERE modifier to discard NULL STRUCT values.
    GOOGLESQL_ASSIGN_OR_RETURN(node, MaybeInjectWhereModifier(std::move(node)));
    // Only perform the ANY_VALUE multi-level aggregation rewrite for aggregate
    // functions that have an empty `group_by_aggregate_list`.
    if (!node->group_by_aggregate_list().empty()) {
      return node;
    }
    // TODO: b/350555383 - How do we handle `generic_argument_list` ?
    if (!node->generic_argument_list().empty()) {
      return absl::UnimplementedError(
          "Measure type rewrite does not currently support generic arguments");
    }
    if (!node->group_by_list().empty()) {
      // `group_by_list` is not empty, but `group_by_aggregate_list` is empty.
      // This means that the aggregate function is a leaf node aggregate
      // function that only references grouping consts or correlated columns
      // (e.g. SUM(1 + e GROUP BY e)). We don't need to perform the ANY_VALUE
      // multi-level aggregation rewrite since the aggregate function is
      // guaranteed to see exactly 1 row per group.
      return node;
    }

    // If here, both `group_by_list` and `group_by_aggregate_list` are empty.
    // This is a plain aggregate function that needs to be rewritten to
    // grain-lock.
    ResolvedAggregateFunctionCallBuilder aggregate_function_call_builder =
        ToBuilder(std::move(node));

    // Step 1: Release the argument list, and wrap applicable arguments with an
    // ANY_VALUE aggregate function call. These aggregate function calls will be
    // placed in the `group_by_aggregate_list` of the rewritten aggregate
    // function call.
    std::vector<std::unique_ptr<const ResolvedExpr>> original_argument_list =
        aggregate_function_call_builder.release_argument_list();
    std::vector<std::unique_ptr<const ResolvedExpr>> rewritten_argument_list;
    std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
        group_by_aggregate_list;
    for (int i = 0; i < original_argument_list.size(); ++i) {
      std::unique_ptr<const ResolvedExpr> argument =
          std::move(original_argument_list[i]);
      // If the `argument` subtree does not contain a ResolvedColumn, then we
      // don't need to wrap it with an ANY_VALUE aggregate function call, and we
      // can simply add it to the rewritten argument list. This behavior is
      // necessary to correctly transform aggregate functions that expect some
      // of their arguments to be literals or parameters (e.g. STRING_AGG), or
      // aggregate functions with special arguments (e.g. BIT_XOR with
      // BitwiseAggMode ENUM).
      GOOGLESQL_ASSIGN_OR_RETURN(const bool arg_contains_resolved_column,
                       ContainsResolvedColumn(argument.get()));
      if (!arg_contains_resolved_column) {
        rewritten_argument_list.push_back(std::move(argument));
        continue;
      }
      const Type* argument_type = argument->type();
      std::vector<std::unique_ptr<const ResolvedExpr>> any_value_argument_list;
      any_value_argument_list.push_back(std::move(argument));
      FunctionSignature any_value_signature({argument_type, 1},
                                            {{argument_type, 1}}, FN_ANY_VALUE);
      auto resolved_any_value_aggregate_function_call =
          MakeResolvedAggregateFunctionCall(
              argument_type, any_value_fn_, any_value_signature,
              std::move(any_value_argument_list), /*generic_argument_list=*/{},
              aggregate_function_call_builder.error_mode(), /*distinct=*/false,
              ResolvedNonScalarFunctionCallBase::DEFAULT_NULL_HANDLING,
              /*where_expr=*/nullptr, /*having_modifier=*/nullptr,
              /*order_by_item_list=*/{}, /*limit=*/nullptr,
              /*function_call_info=*/nullptr, /*group_by_list=*/{},
              /*group_by_aggregate_list=*/{}, /*having_expr=*/nullptr);
      ResolvedColumn any_value_column = column_factory_.MakeCol(
          "$aggregate", absl::StrCat("$any_value_grain_lock_", i),
          argument_type);
      group_by_aggregate_list.push_back(MakeResolvedComputedColumn(
          any_value_column,
          std::move(resolved_any_value_aggregate_function_call)));
      rewritten_argument_list.push_back(MakeResolvedColumnRef(
          any_value_column.type(), any_value_column, /*is_correlated=*/false));
    }

    // Step 2: Compute the `group_by_list`, one grouping key per row identity
    // column the measure needs.
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::vector<std::unique_ptr<const ResolvedExpr>> grain_lock_key_exprs,
        CreateGrainLockingKeys());
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> group_by_list;
    group_by_list.reserve(grain_lock_key_exprs.size());
    for (int i = 0; i < grain_lock_key_exprs.size(); ++i) {
      const ResolvedColumn grain_lock_key_column = column_factory_.MakeCol(
          "$groupbymod", absl::StrCat("grain_lock_key_", i),
          grain_lock_key_exprs[i]->type());
      group_by_list.push_back(MakeResolvedComputedColumn(
          grain_lock_key_column, std::move(grain_lock_key_exprs[i])));
    }

    // Step 3: Set the `group_by_aggregate_list`, `group_by_list` and
    // `argument_list` on the rewritten aggregate function call.
    aggregate_function_call_builder.set_argument_list(
        std::move(rewritten_argument_list));
    aggregate_function_call_builder.set_group_by_aggregate_list(
        std::move(group_by_aggregate_list));
    aggregate_function_call_builder.set_group_by_list(std::move(group_by_list));

    // Step 4: Push the rewritten aggregate function call into
    // `computed_aggregate_list_`, and return a column reference to it.
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedAggregateFunctionCall>
                         rewritten_aggregate_function,
                     std::move(aggregate_function_call_builder).Build());
    return rewritten_aggregate_function;
  }

  absl::Status PreVisitResolvedSubqueryExpr(
      const googlesql::ResolvedSubqueryExpr&) override {
    subquery_depth_++;
    return absl::OkStatus();
  }

  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedSubqueryExpr(
      std::unique_ptr<const ResolvedSubqueryExpr> node) override {
    subquery_depth_--;
    return node;
  }

 private:
  // Returns one grain locking key per row identity column the measure needs,
  // each a `GetStructField` reaching a field of the "key_columns" sub-struct.
  //
  // Grouping by the columns rather than by a single STRUCT packing them is a
  // performance choice: not every engine groups on a synthesized STRUCT key as
  // cheaply as on the columns inside it.
  //
  // The two forms group the input identically except on a NULL closure --
  // grouping by `STRUCT(a, b)` keeps it in a group of its own, grouping by
  // `a, b` merges it into `(NULL, NULL)`. A NULL closure never reaches here:
  // the closure is a `MakeStruct`, so it is only NULL if the measure
  // propagated past an OUTER JOIN, and that is either discarded by the WHERE
  // modifier `MaybeInjectWhereModifier` injects under
  // `FEATURE_AGGREGATE_FILTERING` or, without that feature, rejected by the
  // resolver before the rewriter runs.
  absl::StatusOr<std::vector<std::unique_ptr<const ResolvedExpr>>>
  CreateGrainLockingKeys() const {
    GOOGLESQL_RET_CHECK(closure_struct_ref_->type()->IsStruct());
    const StructType* closure_struct = closure_struct_ref_->type()->AsStruct();
    GOOGLESQL_RET_CHECK(closure_struct->num_fields() == 2);
    GOOGLESQL_RET_CHECK(closure_struct->field(kKeyColumnsFieldIndex).type->IsStruct());
    const StructType* key_columns_struct =
        closure_struct->field(kKeyColumnsFieldIndex).type->AsStruct();
    GOOGLESQL_RET_CHECK_GE(key_columns_struct->num_fields(),
                 row_identity_column_names_.size());

    // The keys are emitted in `row_identity_column_names_` order, i.e. sorted
    // by name, whether the measure needs every key column or only some.
    std::vector<std::unique_ptr<const ResolvedExpr>> grain_lock_key_exprs;
    grain_lock_key_exprs.reserve(row_identity_column_names_.size());
    for (absl::string_view field_name : row_identity_column_names_) {
      bool is_ambiguous = false;
      int field_idx = -1;
      const StructField* field =
          key_columns_struct->FindField(field_name, &is_ambiguous, &field_idx);
      GOOGLESQL_RET_CHECK(field != nullptr) << "Cannot find field " << field_name
                                  << " from the key_columns struct: "
                                  << key_columns_struct->DebugString();
      GOOGLESQL_RET_CHECK(!is_ambiguous)
          << field_name << " is ambiguous, key_columns struct: "
          << key_columns_struct->DebugString();

      GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedColumnRef> struct_ref_copy,
                       ResolvedASTDeepCopyVisitor::Copy(closure_struct_ref_));
      grain_lock_key_exprs.push_back(MakeResolvedGetStructField(
          field->type,
          MakeResolvedGetStructField(key_columns_struct,
                                     std::move(struct_ref_copy),
                                     kKeyColumnsFieldIndex),
          field_idx));
    }
    return grain_lock_key_exprs;
  }

  // Modify the aggregate function call to inject a WHERE modifier to discard
  // NULL STRUCT values. NULL STRUCT values may be introduced if the measure
  // propagates past OUTER JOINs. NULL STRUCT values represent invalid captured
  // measure context / state and hence must be discarded.
  absl::StatusOr<std::unique_ptr<const ResolvedAggregateFunctionCall>>
  MaybeInjectWhereModifier(
      std::unique_ptr<const ResolvedAggregateFunctionCall> node) {
    // If `aggregate_function_depth_` == 1 && subquery_depth_ == 0, then we are
    // currently within a top-level aggregate function call, and a WHERE
    // modifier should be injected to discard NULL struct column values. Only
    // inject the WHERE modifier if the aggregate filtering is enabled.
    if (aggregate_function_depth_ == 1 && subquery_depth_ == 0 &&
        language_options_.LanguageFeatureEnabled(FEATURE_AGGREGATE_FILTERING)) {
      // Measure validator should have already verified that there is no
      // WHERE clause on the aggregate function call.
      GOOGLESQL_RET_CHECK(node->where_expr() == nullptr);
      ResolvedAggregateFunctionCallBuilder aggregate_function_call_builder =
          ToBuilder(std::move(node));
      GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedColumnRef> struct_ref_copy,
                       ResolvedASTDeepCopyVisitor::Copy(closure_struct_ref_));
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto struct_is_not_null,
          function_call_builder_.IsNotNull(std::move(struct_ref_copy)));
      aggregate_function_call_builder.set_where_expr(
          std::move(struct_is_not_null));
      return std::move(aggregate_function_call_builder).Build();
    }
    return node;
  }

  absl::StatusOr<const bool> ContainsResolvedColumn(const ResolvedExpr* expr) {
    ContainsResolvedColumnVisitor contains_resolved_column_visitor;
    GOOGLESQL_RETURN_IF_ERROR(expr->Accept(&contains_resolved_column_visitor));
    return contains_resolved_column_visitor.ContainsResolvedColumn();
  }

  // A pointer to the `ANY_VALUE` function in the catalog used for the rewrite.
  const Function* any_value_fn_ = nullptr;
  // Used to create new function calls for the rewrite.
  FunctionCallBuilder& function_call_builder_;
  // Used to determine if `FEATURE_AGGREGATE_FILTERING` is enabled.
  // If enabled, then the rewrite will inject a WHERE modifier to discard
  // NULL values for the special STRUCT-typed column.
  const LanguageOptions& language_options_;
  // Used to create new columns.
  ColumnFactory& column_factory_;

  // The ColumnRef to the special STRUCT-typed column that contains the grouping
  // keys needed for grain-locking.
  const ResolvedColumnRef* closure_struct_ref_;

  // Names of row identity columns for the measure being rewritten.
  //
  // These names match `Column::Name()` for consistency in the printed
  // resolved AST.
  const absl::btree_set<std::string, googlesql_base::CaseLess>&
      row_identity_column_names_;

  // If `subquery_depth_` > 0, then we are currently within a subquery and
  // any aggregate functions should not be rewritten to grain-lock.
  uint64_t subquery_depth_ = 0;
  // If `aggregate_function_depth_` == 1 && subquery_depth_ == 0, then we are
  // currently within a top-level aggregate function call, and a WHERE modifier
  // should be injected to discard NULL struct column values.
  uint64_t aggregate_function_depth_ = 0;
};

// `StructColumnReferenceRewriter` rewrites a measure expression to reference
// columns from the STRUCT-typed column used to replace the measure column.
class StructColumnReferenceRewriter : public ResolvedASTDeepCopyVisitor {
 public:
  StructColumnReferenceRewriter(ResolvedColumn struct_column,
                                bool struct_column_refs_are_correlated,
                                AnnotationPropagator& annotation_propagator)
      : struct_column_(struct_column),
        struct_column_refs_are_correlated_(struct_column_refs_are_correlated),
        annotation_propagator_(annotation_propagator) {}
  StructColumnReferenceRewriter(const StructColumnReferenceRewriter&) = delete;
  StructColumnReferenceRewriter& operator=(
      const StructColumnReferenceRewriter&) = delete;

 protected:
  absl::Status VisitResolvedSubqueryExpr(
      const ResolvedSubqueryExpr* node) override {
    // First, process the `in_expr` field. The `in_expr` does not see the
    // parameter list, so we must process it first, before we push a new
    // CorrelatedParameterInfo onto the stack.
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> rewritten_in_expr,
                     ProcessNode(node->in_expr()));

    // Now, push a new CorrelatedParameterInfo onto the stack and process the
    // `subquery` field.
    correlated_parameter_info_list_.push_back(CorrelatedParameterInfo());
    auto cleanup_correlated_parameter_info = absl::MakeCleanup(
        [this] { correlated_parameter_info_list_.pop_back(); });
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<const ResolvedScan> rewritten_subquery_scan,
        ProcessNode(node->subquery()));

    // Make a copy of the subquery expr, and set the `subquery` and `in_expr`
    // fields.
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<const ResolvedSubqueryExpr> copied_subquery_expr,
        ResolvedASTDeepCopyVisitor::Copy(node));
    ResolvedSubqueryExprBuilder subquery_expr_builder =
        ToBuilder(std::move(copied_subquery_expr));
    subquery_expr_builder.set_subquery(std::move(rewritten_subquery_scan));
    subquery_expr_builder.set_in_expr(std::move(rewritten_in_expr));
    GOOGLESQL_RET_CHECK(!correlated_parameter_info_list_.empty());
    if (correlated_parameter_info_list_.back()
            .add_struct_column_to_parameter_list) {
      std::unique_ptr<ResolvedColumnRef> struct_column_ref =
          MakeResolvedColumnRef(
              struct_column_,
              /*is_correlated=*/
              correlated_parameter_info_list_.back().is_correlated);
      subquery_expr_builder.add_parameter_list(std::move(struct_column_ref));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<ResolvedSubqueryExpr> rewritten_subquery_expr,
        std::move(subquery_expr_builder).BuildMutable());
    PushNodeToStack(std::move(rewritten_subquery_expr));
    return absl::OkStatus();
  }

  absl::Status VisitResolvedInlineLambda(
      const ResolvedInlineLambda* node) override {
    // Push a new CorrelatedParameterInfo onto the stack and process the
    // `body` of the lambda.
    correlated_parameter_info_list_.push_back(CorrelatedParameterInfo());
    auto cleanup_correlated_parameter_info = absl::MakeCleanup(
        [this] { correlated_parameter_info_list_.pop_back(); });
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> rewritten_lambda_body,
                     ProcessNode(node->body()));

    // Make a copy of the lambda, set the `body` field and augment the parameter
    // list with the struct column if needed.
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedInlineLambda> copied_lambda,
                     ResolvedASTDeepCopyVisitor::Copy(node));
    ResolvedInlineLambdaBuilder lambda_builder =
        ToBuilder(std::move(copied_lambda));
    lambda_builder.set_body(std::move(rewritten_lambda_body));
    GOOGLESQL_RET_CHECK(!correlated_parameter_info_list_.empty());
    if (correlated_parameter_info_list_.back()
            .add_struct_column_to_parameter_list) {
      std::unique_ptr<ResolvedColumnRef> struct_column_ref =
          MakeResolvedColumnRef(
              struct_column_,
              /*is_correlated=*/
              correlated_parameter_info_list_.back().is_correlated);
      lambda_builder.add_parameter_list(std::move(struct_column_ref));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedInlineLambda> rewritten_lambda,
                     std::move(lambda_builder).BuildMutable());
    PushNodeToStack(std::move(rewritten_lambda));
    return absl::OkStatus();
  }

  absl::Status VisitResolvedExpressionColumn(
      const ResolvedExpressionColumn* node) override {
    // If we visit an ExpressionColumn, then we need to augment the parameter
    // list of any enclosing subqueries or lambdas to include the struct column.
    for (int i = 0; i < correlated_parameter_info_list_.size(); ++i) {
      correlated_parameter_info_list_[i].add_struct_column_to_parameter_list =
          true;
      if (i > 0 || struct_column_refs_are_correlated_) {
        correlated_parameter_info_list_[i].is_correlated = true;
      }
    }
    // Make a column ref to the struct column. If
    // `struct_column_refs_are_correlated` is true, OR we are within a subquery,
    // then the column ref is correlated.
    //
    // struct_column_ref = ColumnRef(
    //   type=STRUCT<STRUCT<referenced_columns>, STRUCT<key_columns>>
    // )
    std::unique_ptr<ResolvedColumnRef> struct_column_ref =
        MakeResolvedColumnRef(
            struct_column_,
            /*is_correlated=*/struct_column_refs_are_correlated_ ||
                !correlated_parameter_info_list_.empty());
    // +-GetStructField
    //  +-type=STRUCT<referenced_columns>
    //  +-expr=
    //  | +-<struct_column_ref>
    //  +-field_idx=0
    GOOGLESQL_RET_CHECK(struct_column_ref->type() != nullptr);
    GOOGLESQL_RET_CHECK(struct_column_ref->type()->IsStruct())
        << "Expected struct type for closure column, but got: "
        << struct_column_ref->type()->DebugString();
    GOOGLESQL_RET_CHECK_EQ(struct_column_ref->type()->AsStruct()->num_fields(), 2);
    const StructField& referenced_columns_field =
        struct_column_ref->type()->AsStruct()->field(
            kReferencedColumnsFieldIndex);
    std::unique_ptr<ResolvedGetStructField> get_struct_field_expr =
        MakeResolvedGetStructField(referenced_columns_field.type,
                                   std::move(struct_column_ref),
                                   kReferencedColumnsFieldIndex);
    GOOGLESQL_RETURN_IF_ERROR(annotation_propagator_.CheckAndPropagateAnnotations(
        nullptr, get_struct_field_expr.get()));

    // +-GetStructField
    //  +-type=<output_type>
    //  +-expr=
    //  | +-GetStructField
    //  |  +-type=STRUCT<referenced_columns>
    //  |  +-expr=
    //  |  | +-<struct_column_ref>
    //  |  +-field_idx=0
    //  +-field_idx=<field_index>
    bool is_ambiguous = false;
    int field_index = -1;
    const StructField* field =
        get_struct_field_expr->type()->AsStruct()->FindField(
            node->name(), &is_ambiguous, &field_index);
    GOOGLESQL_RET_CHECK(field != nullptr);
    GOOGLESQL_RET_CHECK(!is_ambiguous);
    GOOGLESQL_RET_CHECK(field_index >= 0);
    std::unique_ptr<ResolvedGetStructField> get_field_expr =
        MakeResolvedGetStructField(
            field->type, std::move(get_struct_field_expr), field_index);
    GOOGLESQL_RETURN_IF_ERROR(annotation_propagator_.CheckAndPropagateAnnotations(
        nullptr, get_field_expr.get()));
    PushNodeToStack(std::move(get_field_expr));
    return absl::OkStatus();
  }

 private:
  // `CorrelatedParameterInfo` is used to track information about correlated
  // parameters for both subqueries and lambdas.
  struct CorrelatedParameterInfo {
    bool add_struct_column_to_parameter_list = false;
    bool is_correlated = false;
  };

  ResolvedColumn struct_column_;
  // If `struct_column_refs_are_correlated_` is true, then any references to
  // `struct_column_` are treated as correlated. Should only be true when the
  // measure column is being invoked in a correlated context;
  // e.g. AGG(correlated_reference_to_measure_column).
  bool struct_column_refs_are_correlated_;
  AnnotationPropagator& annotation_propagator_;
  std::vector<CorrelatedParameterInfo> correlated_parameter_info_list_;
};

// Rewrites column references in the expression `expr` to
// reference field accesses on the closure struct column `closure_struct_ref`.
//
// Returns the rewritten expression.
static absl::StatusOr<std::unique_ptr<const ResolvedExpr>> RewriteReferences(
    const ResolvedExpr* expr, const ResolvedColumnRef* closure_struct_ref,
    AnnotationPropagator& annotation_propagator) {
  StructColumnReferenceRewriter rewriter(closure_struct_ref->column(),
                                         closure_struct_ref->is_correlated(),
                                         annotation_propagator);
  GOOGLESQL_RETURN_IF_ERROR(expr->Accept(&rewriter));

  return rewriter.ConsumeRootNode<ResolvedExpr>();
}

// Rewrites the expression `expr` to use multi-level aggregation to grain-lock
// and avoid overcounting.
//
// Returns the grain-locked expression.
static absl::StatusOr<std::unique_ptr<const ResolvedExpr>> GrainLock(
    std::unique_ptr<const ResolvedExpr> expr,
    const ResolvedColumnRef* closure_struct_ref,
    const absl::btree_set<std::string, googlesql_base::CaseLess>&
        row_identity_column_names,
    const Function* any_value_fn, FunctionCallBuilder& function_call_builder,
    const LanguageOptions& language_options, ColumnFactory& column_factory) {
  MultiLevelAggregateRewriter rewriter(
      any_value_fn, function_call_builder, language_options, column_factory,
      closure_struct_ref, row_identity_column_names);

  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedNode> rewritten,
                   rewriter.VisitAll(std::move(expr)));
  GOOGLESQL_RET_CHECK(rewritten->Is<ResolvedExpr>());
  return absl::WrapUnique(rewritten.release()->GetAs<ResolvedExpr>());
}

// Visitor to replace ResolvedColumnRef with expressions.
class ColumnRefReplacer : public ResolvedASTRewriteVisitor {
 public:
  explicit ColumnRefReplacer(
      absl::flat_hash_map<ResolvedColumn, std::unique_ptr<const ResolvedExpr>>
          substitution)
      : substitution_(std::move(substitution)) {}

 protected:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedColumnRef(
      std::unique_ptr<const ResolvedColumnRef> node) override {
    auto it = substitution_.find(node->column());
    if (it != substitution_.end()) {
      return ResolvedASTDeepCopyVisitor::Copy(it->second.get());
    }
    return node;
  }

 private:
  absl::flat_hash_map<ResolvedColumn, std::unique_ptr<const ResolvedExpr>>
      substitution_;
};

// Substitutes column references in `expr` with expressions from `substitution`.
//
// For each `ResolvedColumnRef` in `expr` pointing to a column `C` that is
// present in `substitution`, it replaces the reference with a deep copy of the
// corresponding expression `substitution[C]`.
//
// Arguments:
// - `expr`: The expression to perform substitution on.
// - `substitution`: A map from columns to the expressions that should replace
//     references to them.
static absl::StatusOr<std::unique_ptr<const ResolvedExpr>> SubstituteColumnRefs(
    std::unique_ptr<const ResolvedExpr> expr,
    absl::flat_hash_map<ResolvedColumn, std::unique_ptr<const ResolvedExpr>>
        substitution) {
  ColumnRefReplacer substitution_visitor(std::move(substitution));
  return substitution_visitor.VisitAll<ResolvedExpr>(std::move(expr));
}

}  // namespace

// TODO: Add caching to avoid exponential AST size bloat.
// Currently, the function does not do any caching, so the same AGG(dep_m) calls
// can be rewritten multiple times. This includes:
//
// - The `AGG(dep_m)` calls in the definition expression of a measure, e.g.,
//   `m := MEASURE(AGG(dep_m) + AGG(dep_m))`.
// - The `AGG(dep_m)` calls in multiple derived measure definitions, e.g.,
//   `m1 := MEASURE(AGG(dep_m) + 1)`, `m2 := MEASURE(AGG(dep_m) + 2)`.
//
// As a result, there can be exponential AST size bloat. For example,
// consider n measures that depend on each other, specifically,
//
// m_i := MEASURE(AGG(m_1) + ... + AGG(m_{i-1})) for i = 2..n.
//
// Rewriting this `AGG(m_n)` has the time complexity O(2^n).
absl::StatusOr<RewriteMeasureExprResult> RewriteMeasureExpr(
    const MeasureType* measure_type,
    const ResolvedColumnRef* closure_struct_ref,
    const MeasureCollector& measure_collector, const Function* any_value_fn,
    FunctionCallBuilder& function_call_builder,
    const LanguageOptions& language_options, ColumnFactory& column_factory,
    TypeFactory& type_factory, AnnotationPropagator& annotation_propagator) {
  GOOGLESQL_ASSIGN_OR_RETURN(MeasureInfo measure_info,
                   measure_collector.GetMeasureInfo(measure_type));

  const ResolvedExpr* measure_expr = measure_info.measure_expr;

  // Remap column ids in the measure expression to use new column ids
  // allocated by `column_factory`. Since the measure expression was
  // analyzed in a different context, it's column ids will be invalid in
  // the current query.
  ColumnReplacementMap column_replacement_map;
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> rewritten_measure_expr,
                   CopyResolvedASTAndRemapColumns(*measure_expr, column_factory,
                                                  column_replacement_map));

  // Extract constituent aggregates from the measure expression.
  std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
      temp_constituent_aggregates;
  GOOGLESQL_ASSIGN_OR_RETURN(
      rewritten_measure_expr,
      ExtractTopLevelAggregates(std::move(rewritten_measure_expr),
                                temp_constituent_aggregates, column_factory));

  // The list of standard aggregates that need to be computed by the
  // AggregateScan to evaluate this AGG(m) call.
  std::vector<std::unique_ptr<const ResolvedComputedColumnBase>> aggregates;

  // Computed columns representing the closure struct expressions of
  // dependent measures.
  //
  // For example, suppose the input measure is m := MEASURE(AGG(b) + 1). The
  // closure struct expression of b is `GetStructField(closure_struct_ref, b)`,
  // and we construct a ResolvedComputedColumn to encapsulate its value to
  // simplify the recursive call.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      closure_computed_columns;

  // A replacement map to rewrite the scalar expression of AGG(m).
  //
  // The keys are the columns representing the output of each AGG(dep_m),
  // and the values are the rewritten scalar expressions of AGG(dep_m).
  absl::flat_hash_map<ResolvedColumn, std::unique_ptr<const ResolvedExpr>>
      substitution;

  for (std::unique_ptr<const ResolvedComputedColumnBase>& aggregate_col :
       temp_constituent_aggregates) {
    GOOGLESQL_RET_CHECK(aggregate_col->Is<ResolvedComputedColumn>());
    ResolvedComputedColumnBuilder builder = ToBuilder(absl::WrapUnique(
        aggregate_col.release()->GetAs<ResolvedComputedColumn>()));

    if (!IsMeasureAggFunction(builder.expr())) {
      // Standard aggregate. Apply grain locking to the aggregate expression.
      //
      // We do not allow defining measures with AGG calls nested under other
      // aggregate functions, so the expression here is guaranteed to not
      // contain any nested AGG calls.
      GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> expr,
                       RewriteReferences(builder.expr(), closure_struct_ref,
                                         annotation_propagator));
      GOOGLESQL_ASSIGN_OR_RETURN(expr, GrainLock(std::move(expr), closure_struct_ref,
                                       measure_info.row_identity_column_names,
                                       any_value_fn, function_call_builder,
                                       language_options, column_factory));
      GOOGLESQL_ASSIGN_OR_RETURN(auto grain_locked_aggregate,
                       std::move(builder).set_expr(std::move(expr)).Build());
      aggregates.push_back(std::move(grain_locked_aggregate));
    } else {
      // AGG(<dep_m_expr>) call. Rewrite it recursively.
      //
      // <dep_m_expr> is guaranteed to be a one of the following:
      //
      // - ExpressionColumn, e.g., `AGG(b)`
      // - GetStructField, e.g., `AGG(STRUCT(b).b)`
      const ResolvedAggregateFunctionCall* agg_call =
          builder.expr()->GetAs<ResolvedAggregateFunctionCall>();
      GOOGLESQL_RET_CHECK(agg_call != nullptr);
      GOOGLESQL_RET_CHECK_EQ(agg_call->argument_list().size(), 1);

      // The `agg_arg` is can be a complex expression that evaluates to
      // a measure type, e.g., `STRUCT(b).b`, not necessarily a
      // ResolvedExpressionColumn.
      const ResolvedExpr* agg_arg = agg_call->argument_list()[0].get();
      const MeasureType* dep_measure_type = agg_arg->type()->AsMeasure();
      GOOGLESQL_RET_CHECK(dep_measure_type != nullptr);
      GOOGLESQL_RET_CHECK(agg_arg->Is<ResolvedExpressionColumn>() ||
                agg_arg->Is<ResolvedGetStructField>());

      // Replacing the references to `dep_m` in `agg_arg` with the corresponding
      // struct field access on `closure_struct_ref`, which evaluates to the
      // closure expression of `dep_m`, gives the closure expression of `dep_m`.
      //
      // Example:
      //
      // Suppose we have:
      //   b := MEASURE( SUM(x) )
      //   m := MEASURE( AGG(b) + 1 )
      //
      // The closure struct columns for b and m are constructed by
      // ComputeClosureColumnsForMeasuresFromScan (if they are from a scan) or
      // BuildStructFromRowFields (if they are from a RowType). Specifically,
      //
      // The closure struct of the base measure b is:
      //
      //   closure_of_b := STRUCT<
      //     referenced_columns: STRUCT<x TYPEOF(x)>,
      //     key_columns: ...
      //   >
      //
      // The closure struct of the derived measure m is:
      //
      //   closure_of_m := STRUCT<
      //     referenced_columns: STRUCT<b closure_of_b>,
      //     key_columns: ...
      //   >
      //
      // When calling RewriteMeasureExpr on m, `closure_struct_ref` is
      // `closure_of_m`. To recursively rewrite `AGG(b)` in m's definition, we
      // call RewriteMeasureExpr on b, which requires the closure expression
      // of b.
      //
      // We extract b's closure from `closure_of_m` via struct field access:
      //
      //   `closure_struct_ref.referenced_columns.b`
      //
      // which evaluates to `closure_of_b`. (See the definition of
      // `closure_of_m` above.)
      //
      // In the implementation, b in `AGG(b)` is a ResolvedExpressionColumn.
      // Calling `RewriteReferences(b, closure_struct_ref)` produces the
      // resolved expression for `closure_struct_ref.referenced_columns.b`.
      //
      // Also supports more complex `agg_arg` shapes. For example, if `agg_arg`
      // is `STRUCT(dep_m AS f).f`, the result expression is
      // `STRUCT(closure_struct_ref.referenced_columns.dep_m AS f).f`, which
      // evaluates to the closure expression of `dep_m`.
      GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> dep_closure_expr,
                       RewriteReferences(agg_arg, closure_struct_ref,
                                         annotation_propagator));

      // Create a local ResolvedComputedColumn to hold the closure struct
      // expression of `dep_m`.
      ResolvedColumn dep_closure_column = column_factory.MakeCol(
          "$aggregate", "dep_closure", dep_closure_expr->annotated_type());
      auto dep_closure_computed_column = MakeResolvedComputedColumn(
          dep_closure_column, std::move(dep_closure_expr));

      auto sub_closure_ref = MakeResolvedColumnRef(
          dep_closure_column,
          // The created ResolvedComputedColumn will be added to a ProjectScan
          // that wraps the `input_scan` of the AggregateScan, so for the AGG
          // call this column reference is always local.
          /*is_correlated=*/false);

      closure_computed_columns.push_back(
          std::move(dep_closure_computed_column));

      GOOGLESQL_ASSIGN_OR_RETURN(
          RewriteMeasureExprResult sub_result,
          RewriteMeasureExpr(
              dep_measure_type, sub_closure_ref.get(), measure_collector,
              any_value_fn, function_call_builder, language_options,
              column_factory, type_factory, annotation_propagator));

      for (auto& cc : sub_result.closure_computed_columns) {
        closure_computed_columns.push_back(std::move(cc));
      }
      for (auto& agg : sub_result.constituent_aggregate_list) {
        aggregates.push_back(std::move(agg));
      }
      // The ColumnRefs to the ResolvedComputedColumn corresponding to the
      // AGG(dep_m) call are replaced with the rewritten expression of
      // AGG(dep_m).
      substitution[builder.column()] =
          std::move(sub_result.rewritten_measure_expr);
    }
  }

  // Apply substitutions to rewritten_measure_expr.
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> substituted_expr,
                   SubstituteColumnRefs(std::move(rewritten_measure_expr),
                                        std::move(substitution)));

  return RewriteMeasureExprResult{
      .rewritten_measure_expr = std::move(substituted_expr),
      .constituent_aggregate_list = std::move(aggregates),
      .closure_computed_columns = std::move(closure_computed_columns),
  };
}

}  // namespace googlesql
