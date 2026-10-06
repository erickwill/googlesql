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

#include "googlesql/reference_impl/vector_tvfs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/proto/kmeans_options.pb.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/kmeans_options.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "googlesql/reference_impl/evaluation.h"
#include "googlesql/reference_impl/function.h"
#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "googlesql/base/check.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {

namespace {

// Evaluator iterator that implements the batch vector search logic. It fetches
// and caches all target base rows, then iterates over query rows to compute
// distances and find the nearest base rows.
class BatchVectorSearchResultIterator : public EvaluatorTableIterator {
 public:
  explicit BatchVectorSearchResultIterator(
      std::unique_ptr<EvaluatorTableIterator> base_iterator,
      std::unique_ptr<EvaluatorTableIterator> query_iterator,
      std::vector<TVFSchemaColumn> output_columns, std::string column_to_search,
      std::string query_column_to_search, bool query_column_to_search_provided,
      bool is_batch, int64_t top_k, std::string distance_type,
      Value max_distance, std::unique_ptr<TypeFactory> type_factory)
      : base_iterator_(std::move(base_iterator)),
        query_iterator_(std::move(query_iterator)),
        column_to_search_(std::move(column_to_search)),
        query_column_to_search_(std::move(query_column_to_search)),
        query_column_to_search_provided_(query_column_to_search_provided),
        is_batch_(is_batch),
        top_k_(top_k),
        distance_type_(std::move(distance_type)),
        max_distance_(max_distance),
        output_columns_(std::move(output_columns)),
        type_factory_(std::move(type_factory)) {}

  ~BatchVectorSearchResultIterator() override = default;

  int NumColumns() const override {
    return static_cast<int>(output_columns_.size());
  }

  std::string GetColumnName(int i) const override {
    return output_columns_[i].name;
  }

  const Type* GetColumnType(int i) const override {
    return output_columns_[i].type;
  }

  const Value& GetValue(int i) const override {
    return current_output_values_[i];
  }

  bool NextRow() override;

  absl::Status Status() const override { return status_; }
  absl::Status Cancel() override {
    absl::Status s = base_iterator_->Cancel();
    if (!s.ok()) return s;
    return query_iterator_->Cancel();
  }
  void SetDeadline(absl::Time deadline) override {
    base_iterator_->SetDeadline(deadline);
    query_iterator_->SetDeadline(deadline);
  }

 private:
  struct ResultEntry {
    Value query_row;
    Value base_row;
    Value distance;

    bool operator<(const ResultEntry& other) const {
      // We want min-heap for top-k (keeping smallest distances).
      if (distance.LessThan(other.distance)) {
        return true;
      }
      // If distances are equal, we compare rows lexicographically by
      // (query_row, base_row). This can happen in the case of NULL distance.
      if (distance.Equals(other.distance)) {
        return query_row.Equals(other.query_row)
                   ? base_row.LessThan(other.base_row)
                   : query_row.LessThan(other.query_row);
      }
      return false;
    }
  };

  const StructType* GetBaseRowStructType() const {
    return output_columns_[1].type->AsStruct();
  }

  absl::Status InitializeBaseData();
  bool ProcessNextQueryRow();
  absl::StatusOr<Value> ComputeDistance(const Value& v1, const Value& v2);

  // Input args.
  std::unique_ptr<EvaluatorTableIterator> base_iterator_;
  std::unique_ptr<EvaluatorTableIterator> query_iterator_;
  std::string column_to_search_;
  std::string query_column_to_search_;
  bool query_column_to_search_provided_;
  bool is_batch_;
  int64_t top_k_;
  std::string distance_type_;
  Value max_distance_;

  bool base_loaded_ = false;
  // Base table data loaded in memory.
  // Each element is a Struct Value representing the row.
  std::vector<Value> base_rows_;
  // Pre-computed index of embedding column in base/query iterators.
  int base_embedding_col_idx_ = -1;
  int query_embedding_col_idx_ = -1;

  // Buffer for results of current query row.
  std::vector<ResultEntry> results_buffer_;
  int buffer_index_ = 0;

  std::vector<TVFSchemaColumn> output_columns_;
  // Current output row values.
  std::array<Value, 3> current_output_values_;
  absl::Status status_;
  // Type factory to maintain lifetime of structs in the output.
  std::unique_ptr<TypeFactory> type_factory_;
};

// An EvaluatorTableIterator that wraps a single Value. It iterates over exactly
// this single value, yielding it as the single column of the row. This is used
// to wrap a query value argument into an iterator interface to reuse the batch
// logic.
class SingleValueEvaluatorTableIterator : public EvaluatorTableIterator {
 public:
  explicit SingleValueEvaluatorTableIterator(Value value)
      : value_(std::move(value)) {}

  int NumColumns() const override { return 1; }

  std::string GetColumnName(int i) const override { return "query_value"; }

  const Type* GetColumnType(int i) const override { return value_.type(); }

  const Value& GetValue(int i) const override { return value_; }

  // An iterator that yields exactly one row containing a single value.
  // `done_` tracks whether this single row has already been consumed.
  bool NextRow() override {
    if (done_) return false;
    done_ = true;
    return true;
  }

  absl::Status Status() const override { return absl::OkStatus(); }
  absl::Status Cancel() override { return absl::OkStatus(); }
  void SetDeadline(absl::Time deadline) override {}

 private:
  Value value_;
  bool done_ = false;
};

class SingleVectorSearchResultIterator : public EvaluatorTableIterator {
 public:
  explicit SingleVectorSearchResultIterator(
      std::unique_ptr<EvaluatorTableIterator> batch_iterator)
      : batch_iterator_(std::move(batch_iterator)) {
    ABSL_DCHECK_EQ(batch_iterator_->NumColumns(), 3);
  }

  int NumColumns() const override { return 2; }

  std::string GetColumnName(int i) const override {
    IsValidIndex(i);
    return batch_iterator_->GetColumnName(i + 1);
  }

  const Type* GetColumnType(int i) const override {
    IsValidIndex(i);
    return batch_iterator_->GetColumnType(i + 1);
  }

  const Value& GetValue(int i) const override {
    IsValidIndex(i);
    return batch_iterator_->GetValue(i + 1);
  }

  bool NextRow() override { return batch_iterator_->NextRow(); }

  absl::Status Status() const override { return batch_iterator_->Status(); }
  absl::Status Cancel() override { return batch_iterator_->Cancel(); }
  void SetDeadline(absl::Time deadline) override {
    batch_iterator_->SetDeadline(deadline);
  }

 private:
  void IsValidIndex(int i) const {
    ABSL_DCHECK_GE(i, 0);
    ABSL_DCHECK_LT(i, 2);
  }

  std::unique_ptr<EvaluatorTableIterator> batch_iterator_;
};

}  // namespace

absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>>
VectorSearchTVF::CreateEvaluator(
    std::vector<TableValuedFunction::TvfEvaluatorArg> args,
    std::shared_ptr<FunctionSignature> function_call_signature,
    std::shared_ptr<const TVFSignature> tvf_signature,
    EvaluationContext* context) {
  // Indices for arguments in the batch version of VECTOR_SEARCH.
  // The single version of VECTOR_SEARCH has the same arguments starting from
  // the `options` argument, but shifted left by 1 (i.e. index - 1) because
  // it lacks the `query_column_to_search` argument.
  constexpr int kBatchBaseTableIdx = 0;
  constexpr int kBatchBaseColumnNameIdx = 1;
  constexpr int kBatchQueryTableIdx = 2;
  constexpr int kBatchQueryColumnNameIdx = 3;
  [[maybe_unused]] constexpr int kBatchOptionsIdx = 4;
  constexpr int kBatchTopKIdx = 5;
  constexpr int kBatchDistanceTypeIdx = 6;
  constexpr int kBatchMaxDistanceIdx = 7;

  GOOGLESQL_RET_CHECK(args.size() >= 3);

  const bool is_batch = (args[kBatchQueryTableIdx].relation != nullptr);

  if (is_batch) {
    GOOGLESQL_RET_CHECK(args.size() == kBatchMaxDistanceIdx + 1);
  } else {
    GOOGLESQL_RET_CHECK(args.size() == kBatchMaxDistanceIdx);
  }

  const int top_k_idx = is_batch ? kBatchTopKIdx : kBatchTopKIdx - 1;
  const int distance_type_idx =
      is_batch ? kBatchDistanceTypeIdx : kBatchDistanceTypeIdx - 1;
  const int max_distance_idx =
      is_batch ? kBatchMaxDistanceIdx : kBatchMaxDistanceIdx - 1;

  std::unique_ptr<EvaluatorTableIterator> base_table_iter =
      std::move(args[kBatchBaseTableIdx].relation);
  GOOGLESQL_RET_CHECK(base_table_iter != nullptr);

  GOOGLESQL_RET_CHECK(args[kBatchBaseColumnNameIdx].value.has_value() &&
            args[kBatchBaseColumnNameIdx].value->type()->IsString());
  std::string base_column_name =
      args[kBatchBaseColumnNameIdx].value->string_value();

  std::unique_ptr<EvaluatorTableIterator> query_table_iter;
  std::string query_column_name;
  bool query_column_to_search_provided = true;

  if (is_batch) {
    query_table_iter = std::move(args[kBatchQueryTableIdx].relation);
    GOOGLESQL_RET_CHECK(query_table_iter != nullptr);

    if (!args[kBatchQueryColumnNameIdx].value.has_value() ||
        args[kBatchQueryColumnNameIdx].value->is_null()) {
      query_column_to_search_provided = false;
      // By default, set the query column name to the first column in the query
      // table. We will validate later that
      // 1) Either the column exists in the query table and its type matches the
      // base column type.
      // 2) Or if not, that the query table has only one column and its type
      // matches the base column type.
      query_column_name = base_column_name;
    } else {
      GOOGLESQL_RET_CHECK(args[kBatchQueryColumnNameIdx].value.has_value() &&
                args[kBatchQueryColumnNameIdx].value->type()->IsString());
      query_column_name = args[kBatchQueryColumnNameIdx].value->string_value();
    }
  } else {
    GOOGLESQL_RET_CHECK(args[kBatchQueryTableIdx].value.has_value());
    Value query_value = args[kBatchQueryTableIdx].value.value();
    query_table_iter =
        std::make_unique<SingleValueEvaluatorTableIterator>(query_value);
    query_column_name = "query_value";
  }

  // This argument is an engine supplied option. It must be already provided
  // during function registration.
  GOOGLESQL_RET_CHECK(args[top_k_idx].value.has_value() &&
            args[top_k_idx].value->type()->IsInt64());
  int top_k = static_cast<int>(args[top_k_idx].value->int64_value());
  // TODO: Cleanup error code here and at a few other places.
  if (top_k <= 0) {
    return absl::OutOfRangeError(
        "Argument 'top_k' to table-valued function VECTOR_SEARCH must be at "
        "least 1");
  }

  std::string distance_type =
      !args[distance_type_idx].value.has_value() ||
              args[distance_type_idx].value->is_null()
          ? "EUCLIDEAN"
          : absl::AsciiStrToUpper(
                args[distance_type_idx].value->string_value());
  if (distance_type != "COSINE" && distance_type != "EUCLIDEAN" &&
      distance_type != "DOT_PRODUCT") {
    return absl::OutOfRangeError(
        "`distance_type` argument of VECTOR_SEARCH TVF must be set to one of "
        "'COSINE', 'DOT_PRODUCT', or 'EUCLIDEAN'");
  }

  double max_distance = std::numeric_limits<double>::infinity();
  if (args[max_distance_idx].value.has_value() &&
      !args[max_distance_idx].value->is_null()) {
    GOOGLESQL_RET_CHECK(args[max_distance_idx].value->type()->IsDouble());
    max_distance = args[max_distance_idx].value->double_value();
  }

  std::vector<TVFSchemaColumn> output_columns;
  auto type_factory = std::make_unique<googlesql::TypeFactory>();

  std::vector<StructType::StructField> query_fields;
  query_fields.reserve(query_table_iter->NumColumns());
  for (int i = 0; i < query_table_iter->NumColumns(); ++i) {
    query_fields.push_back({query_table_iter->GetColumnName(i),
                            query_table_iter->GetColumnType(i)});
  }
  const StructType* query_struct_type;
  GOOGLESQL_RET_CHECK_OK(type_factory->MakeStructType(query_fields, &query_struct_type));
  output_columns.push_back({"query", query_struct_type});

  std::vector<StructType::StructField> base_fields;
  base_fields.reserve(base_table_iter->NumColumns());
  for (int i = 0; i < base_table_iter->NumColumns(); ++i) {
    base_fields.push_back(
        {base_table_iter->GetColumnName(i), base_table_iter->GetColumnType(i)});
  }
  const StructType* base_struct_type;
  GOOGLESQL_RET_CHECK_OK(type_factory->MakeStructType(base_fields, &base_struct_type));
  output_columns.push_back({"base", base_struct_type});
  output_columns.push_back({"distance", types::DoubleType()});

  auto batch_iter = std::make_unique<BatchVectorSearchResultIterator>(
      std::move(base_table_iter), std::move(query_table_iter),
      std::move(output_columns), std::move(base_column_name),
      std::move(query_column_name), query_column_to_search_provided, is_batch,
      top_k, std::move(distance_type), Value::Double(max_distance),
      std::move(type_factory));

  if (is_batch) {
    return batch_iter;
  } else {
    return std::make_unique<SingleVectorSearchResultIterator>(
        std::move(batch_iter));
  }
}

bool BatchVectorSearchResultIterator::NextRow() {
  if (!status_.ok()) return false;
  if (!base_loaded_) {
    status_ = InitializeBaseData();
    if (!status_.ok()) return false;
    base_loaded_ = true;
  }

  while (buffer_index_ >= results_buffer_.size()) {
    // We consumed all results from the buffer. See if there are more records
    // to be processed from the query table.
    if (!ProcessNextQueryRow()) {
      // Status is already set in ProcessNextQueryRow() if there is an error.
      return false;
    }
  }

  const ResultEntry& entry = results_buffer_[buffer_index_++];
  current_output_values_[0] = entry.query_row;
  current_output_values_[1] = entry.base_row;
  current_output_values_[2] = entry.distance;
  return true;
}

absl::Status BatchVectorSearchResultIterator::InitializeBaseData() {
  int found_count = 0;
  for (int i = 0; i < base_iterator_->NumColumns(); ++i) {
    if (googlesql_base::CaseEqual(base_iterator_->GetColumnName(i),
                               column_to_search_)) {
      base_embedding_col_idx_ = i;
      found_count++;
    }
  }
  if (base_embedding_col_idx_ == -1) {
    return absl::OutOfRangeError(absl::Substitute(
        "Unrecognized name $0 in base table argument", column_to_search_));
  }
  if (found_count > 1) {
    return absl::OutOfRangeError(absl::Substitute(
        "Column $0 is ambiguous in the base table", column_to_search_));
  }
  const Type* base_embedding_type =
      base_iterator_->GetColumnType(base_embedding_col_idx_);
  if (!base_embedding_type->IsString() &&
      !(base_embedding_type->IsArray() &&
        (base_embedding_type->AsArray()->element_type()->IsFloat() ||
         base_embedding_type->AsArray()->element_type()->IsDouble()))) {
    return absl::OutOfRangeError(
        "The column specified by the `column_to_search` argument of "
        "VECTOR_SEARCH TVF must be of type ARRAY<DOUBLE> or ARRAY<FLOAT> or "
        "STRING");
  }
  while (base_iterator_->NextRow()) {
    std::vector<Value> fields;
    fields.reserve(base_iterator_->NumColumns());
    for (int i = 0; i < base_iterator_->NumColumns(); ++i) {
      fields.push_back(base_iterator_->GetValue(i));
    }
    const StructType* struct_type = GetBaseRowStructType();  // base
    base_rows_.push_back(Value::Struct(struct_type, fields));
  }
  if (!base_iterator_->Status().ok()) {
    return base_iterator_->Status();
  }
  if (is_batch_) {
    // Reset found_count. We will use the same variable to check for ambiguity
    // in the query table.
    found_count = 0;
    // By default, query_column_to_search, if not provided, is set to
    // column_to_search. We check if it exists in the query table. If not, we
    // check further based on whether it's provided or not.
    for (int i = 0; i < query_iterator_->NumColumns(); ++i) {
      if (googlesql_base::CaseEqual(query_iterator_->GetColumnName(i),
                                 query_column_to_search_)) {
        query_embedding_col_idx_ = i;
        found_count++;
      }
    }
    if (found_count > 1) {
      return absl::OutOfRangeError(
          absl::Substitute("Column $0 is ambiguous in the query table",
                           query_column_to_search_));
    }
    if (query_embedding_col_idx_ == -1) {
      // If the column is not found, we check if it was provided or not. It is
      // an error if it was provided and it doesn't exist in the query table.
      if (query_column_to_search_provided_) {
        return absl::OutOfRangeError(
            absl::Substitute("Unrecognized name $0 in query table argument",
                             query_column_to_search_));
      } else {
        // If not provided, we assume that the query table has only one column.
        // Throw an error otherwise.
        if (query_iterator_->NumColumns() == 1) {
          query_embedding_col_idx_ = 0;
        } else {
          return absl::OutOfRangeError(
              "Cannot infer query column. `query_column_to_search` was not "
              "provided, and the query table has multiple columns but none "
              "match "
              "the name of `column_to_search`");
        }
      }
    }
  } else {
    query_embedding_col_idx_ = 0;
  }
  const Type* query_embedding_type =
      query_iterator_->GetColumnType(query_embedding_col_idx_);
  if (!query_embedding_type->IsString() &&
      !(query_embedding_type->IsArray() &&
        (query_embedding_type->AsArray()->element_type()->IsFloat() ||
         query_embedding_type->AsArray()->element_type()->IsDouble()))) {
    if (is_batch_) {
      return absl::OutOfRangeError(
          "The column specified by the `query_column_to_search` argument of "
          "VECTOR_SEARCH TVF must be of type ARRAY<DOUBLE> or ARRAY<FLOAT> or "
          "STRING");
    } else {
      return absl::OutOfRangeError(
          "The `query_value` argument of VECTOR_SEARCH TVF must be of type "
          "ARRAY<DOUBLE> or ARRAY<FLOAT> or STRING");
    }
  }

  if (!base_embedding_type->Equals(query_embedding_type)) {
    if (is_batch_) {
      return absl::OutOfRangeError(
          "The column types of argument `column_to_search` in the base table "
          "and argument `query_column_to_search` in the query table must be "
          "the "
          "same");
    } else {
      return absl::OutOfRangeError(
          "The column type of argument `column_to_search` in the base table "
          "and the type of argument `query_value` must be the same");
    }
  }

  if (base_embedding_type->IsString() || query_embedding_type->IsString()) {
    if (is_batch_) {
      return absl::OutOfRangeError(
          "STRING column_type for arguments `column_to_search` or "
          "`query_column_to_search` is not supported");
    } else {
      return absl::OutOfRangeError(
          "STRING type for arguments `column_to_search` or "
          "`query_value` is not supported");
    }
  }
  return absl::OkStatus();
}

bool BatchVectorSearchResultIterator::ProcessNextQueryRow() {
  if (!query_iterator_->NextRow()) {
    if (!query_iterator_->Status().ok()) {
      status_ = query_iterator_->Status();
    }
    return false;
  }

  std::vector<Value> query_fields;
  query_fields.reserve(query_iterator_->NumColumns());
  for (int i = 0; i < query_iterator_->NumColumns(); ++i) {
    query_fields.push_back(query_iterator_->GetValue(i));
  }
  const StructType* query_struct_type = output_columns_[0].type->AsStruct();
  Value query_row = Value::Struct(query_struct_type, query_fields);
  Value query_embedding = query_iterator_->GetValue(query_embedding_col_idx_);

  std::vector<ResultEntry> all_matches;
  for (const Value& base_row : base_rows_) {
    Value base_embedding = base_row.field(base_embedding_col_idx_);
    if (base_embedding.is_null() || query_embedding.is_null()) {
      all_matches.push_back({query_row, base_row, Value::NullDouble()});
      continue;
    }
    if (BuiltinScalarFunction::HasNulls(query_embedding.elements()) ||
        BuiltinScalarFunction::HasNulls(base_embedding.elements())) {
      status_ = absl::OutOfRangeError("Unexpected NULL element in input array");
      return false;
    }
    // Compute distance
    absl::StatusOr<Value> dist =
        ComputeDistance(base_embedding, query_embedding);
    if (!dist.ok()) {
      status_ = dist.status();
      return false;
    }

    if (dist.value().is_null()) {
      all_matches.push_back({query_row, base_row, Value::NullDouble()});
      // We explicitly skip NaN values because in the Value::LessThan()
      // comparison, NaN is less than any value. This makes it pass the
      // max_distance check and get included in the results, which is not the
      // behaviour as per how SQL LESS THAN works.
    } else if (std::isnan(dist.value().double_value())) {
      continue;
    } else {
      if (dist.value().LessThan(max_distance_) ||
          dist.value().Equals(max_distance_)) {
        all_matches.push_back({query_row, base_row, dist.value()});
      }
    }
  }

  // Sort matches. For Top-K, we want smallest distance first.
  size_t count = std::min(static_cast<size_t>(top_k_), all_matches.size());
  std::partial_sort(all_matches.begin(), all_matches.begin() + count,
                    all_matches.end());

  results_buffer_.reserve(results_buffer_.size() + count);
  // Take top K
  for (size_t i = 0; i < count; ++i) {
    results_buffer_.push_back(std::move(all_matches[i]));
  }
  return true;
}

absl::StatusOr<Value> BatchVectorSearchResultIterator::ComputeDistance(
    const Value& v1, const Value& v2) {
  if (distance_type_ == "COSINE") {
    return functions::CosineDistanceDense(v1, v2);
  } else if (distance_type_ == "EUCLIDEAN") {
    return functions::EuclideanDistanceDense(v1, v2);
  } else if (distance_type_ == "DOT_PRODUCT") {
    GOOGLESQL_ASSIGN_OR_RETURN(Value dot_product, functions::DotProduct(v1, v2));
    // A larger dot product indicates greater similarity, whereas vector search
    // orders results by distance in ascending order (where a smaller distance
    // represents closer neighbors). Therefore, the dot product is negated so
    // that higher similarity corresponds to a smaller distance.
    return Value::Double(-dot_product.ToDouble());
  }
  return absl::OutOfRangeError(
      "`distance_type` argument of VECTOR_SEARCH TVF must be set to one of "
      "'COSINE', 'DOT_PRODUCT', or 'EUCLIDEAN'");
}

namespace {

class KMeansResultIterator : public EvaluatorTableIterator {
 public:
  KMeansResultIterator(std::unique_ptr<EvaluatorTableIterator> base_iterator,
                       std::string vector_column_name, int64_t k,
                       KMeansOptions options,
                       std::vector<TVFSchemaColumn> output_columns,
                       std::unique_ptr<TypeFactory> type_factory)
      : base_iterator_(std::move(base_iterator)),
        vector_column_name_(std::move(vector_column_name)),
        k_(k),
        options_(std::move(options)),
        output_columns_(std::move(output_columns)),
        type_factory_(std::move(type_factory)) {
    ABSL_DCHECK_EQ(output_columns_.size(), 2);
  }

  int NumColumns() const override { return 2; }
  std::string GetColumnName(int i) const override {
    return output_columns_[i].name;
  }
  const Type* GetColumnType(int i) const override {
    return output_columns_[i].type;
  }
  const Value& GetValue(int i) const override {
    return current_output_values_[i];
  }

  bool NextRow() override {
    if (!status_.ok()) {
      return false;
    }
    if (!clustered_) {
      status_ = PerformClustering();
      if (!status_.ok()) {
        return false;
      }
      clustered_ = true;
    }
    if (current_centroid_idx_ >= centroids_.size()) {
      return false;
    }
    current_output_values_[0] = Value::Int64(current_centroid_idx_ + 1);
    current_output_values_[1] = centroids_[current_centroid_idx_];
    current_centroid_idx_++;
    return true;
  }

  absl::Status Status() const override { return status_; }
  absl::Status Cancel() override { return base_iterator_->Cancel(); }
  void SetDeadline(absl::Time deadline) override {
    base_iterator_->SetDeadline(deadline);
  }

 private:
  struct ValidatedInput {
    std::vector<Value> valid_vectors;
    std::vector<Value> distinct_vectors;
  };

  absl::Status InitCentroids(absl::Span<const Value> distinct_vectors,
                             int64_t restart_idx,
                             std::vector<Value>& out_centroids) {
    out_centroids.clear();
    GOOGLESQL_RET_CHECK(!distinct_vectors.empty());

    if (options_.init_method() == KMeansOptions::KMEANSPP) {
      std::vector<bool> is_centroid(distinct_vectors.size(), false);
      std::mt19937 gen(restart_idx);
      // Pick first centroid uniformly at random from distinct vectors.
      size_t first_idx = absl::Uniform<size_t>(gen, 0, distinct_vectors.size());
      out_centroids.push_back(distinct_vectors[first_idx]);
      is_centroid[first_idx] = true;

      std::vector<double> min_dists(distinct_vectors.size(),
                                    std::numeric_limits<double>::infinity());
      // Initialize min_dists with distances to the first centroid.
      for (size_t i = 0; i < distinct_vectors.size(); ++i) {
        GOOGLESQL_ASSIGN_OR_RETURN(Value dist_val, ComputeDistance(out_centroids[0],
                                                         distinct_vectors[i]));
        min_dists[i] = dist_val.double_value();
      }

      while (out_centroids.size() < k_) {
        std::vector<double> weights(distinct_vectors.size(), 0.0);
        double total_weight = 0.0;
        for (size_t i = 0; i < distinct_vectors.size(); ++i) {
          if (!is_centroid[i]) {
            double dist = min_dists[i];
            weights[i] = dist * dist;
            total_weight += weights[i];
          }
        }

        size_t best_idx = distinct_vectors.size();
        if (total_weight <= 0) {
          // Fallback to deterministic selection of the next available
          // non-centroid.
          for (size_t i = 0; i < distinct_vectors.size(); ++i) {
            if (!is_centroid[i]) {
              best_idx = i;
              break;
            }
          }
          GOOGLESQL_RET_CHECK_NE(best_idx, distinct_vectors.size())
              << "Failed to find non-centroid vector";
        } else {
          std::discrete_distribution<size_t> distribution(weights.begin(),
                                                          weights.end());
          best_idx = distribution(gen);
        }

        const Value& new_centroid = distinct_vectors[best_idx];
        out_centroids.push_back(new_centroid);
        is_centroid[best_idx] = true;

        // Update min_dists with distances to the new centroid.
        if (out_centroids.size() < k_) {
          for (size_t i = 0; i < distinct_vectors.size(); ++i) {
            GOOGLESQL_ASSIGN_OR_RETURN(
                Value dist_val,
                ComputeDistance(new_centroid, distinct_vectors[i]));
            double d = dist_val.double_value();
            if (d < min_dists[i]) {
              min_dists[i] = d;
            }
          }
        }
      }
    } else {
      std::mt19937 gen(restart_idx);
      std::sample(distinct_vectors.begin(), distinct_vectors.end(),
                  std::back_inserter(out_centroids), k_, gen);
    }
    return absl::OkStatus();
  }

  absl::StatusOr<ValidatedInput> GetAndValidateInputVectors(
      int vector_col_idx) {
    std::vector<Value> valid_vectors;
    size_t expected_len = 0;
    bool first_vector = true;

    while (base_iterator_->NextRow()) {
      Value vec = base_iterator_->GetValue(vector_col_idx);
      // If the vector is NULL, we skip it irrespective of the value of
      // fail_on_invalid_vector config.
      if (vec.is_null()) {
        continue;
      }
      if (vec.elements().empty()) {
        if (options_.fail_on_invalid_vector()) {
          return absl::OutOfRangeError("Invalid vector: empty vector");
        }
        continue;
      }
      if (BuiltinScalarFunction::HasNulls(vec.elements())) {
        if (options_.fail_on_invalid_vector()) {
          return absl::OutOfRangeError(
              "Unexpected NULL element in input array");
        }
        continue;
      }

      bool has_invalid_float = false;
      bool is_all_zeros = true;
      for (const Value& elem : vec.elements()) {
        double val =
            elem.type()->IsFloat() ? elem.float_value() : elem.double_value();
        if (!std::isfinite(val)) {
          has_invalid_float = true;
          break;
        }
        if (std::fpclassify(val) != FP_ZERO) {
          is_all_zeros = false;
        }
      }
      if (has_invalid_float) {
        if (options_.fail_on_invalid_vector()) {
          return absl::OutOfRangeError(
              "Invalid vector element: NaN or Infinity");
        }
        continue;
      }
      // If fail_on_invalid_vector is true, all-zero vectors are rejected.
      // Otherwise, they are ignored and skipped.
      if (is_all_zeros) {
        if (options_.fail_on_invalid_vector()) {
          return absl::OutOfRangeError("Invalid vector element: all zeros");
        }
        continue;
      }

      size_t len = vec.elements().size();
      if (first_vector) {
        expected_len = len;
        first_vector = false;
      } else if (len != expected_len) {
        return absl::OutOfRangeError(absl::Substitute(
            "Array length mismatch: $0 and $1", expected_len, len));
      }
      valid_vectors.push_back(vec);
    }
    GOOGLESQL_RETURN_IF_ERROR(base_iterator_->Status());

    if (valid_vectors.size() < k_) {
      return absl::OutOfRangeError(absl::Substitute(
          "Number of valid input vectors ($0) is less than requested number of "
          "clusters ($1)",
          valid_vectors.size(), k_));
    }

    std::vector<Value> distinct_vectors = valid_vectors;
    std::sort(distinct_vectors.begin(), distinct_vectors.end(),
              [](const Value& a, const Value& b) { return a.LessThan(b); });
    distinct_vectors.erase(
        std::unique(distinct_vectors.begin(), distinct_vectors.end()),
        distinct_vectors.end());

    if (distinct_vectors.size() < k_) {
      return absl::OutOfRangeError(absl::Substitute(
          "Number of distinct input vectors ($0) is less than requested number "
          "of clusters ($1)",
          distinct_vectors.size(), k_));
    }
    return ValidatedInput{std::move(valid_vectors),
                          std::move(distinct_vectors)};
  }

  absl::Status RunKMeansIterations(absl::Span<const Value> valid_vectors,
                                   const Type* element_type,
                                   std::vector<Value>& centroids) {
    int64_t actual_k = centroids.size();
    std::vector<int64_t> assignments(valid_vectors.size(), -1);

    for (int64_t iter = 0; iter < options_.num_iterations(); ++iter) {
      std::vector<std::vector<Value>> assigned_vectors(actual_k);
      bool changed = false;

      for (size_t v_idx = 0; v_idx < valid_vectors.size(); ++v_idx) {
        const Value& vec = valid_vectors[v_idx];
        int64_t best_c = 0;
        double min_dist = std::numeric_limits<double>::infinity();
        for (int64_t c = 0; c < actual_k; ++c) {
          GOOGLESQL_ASSIGN_OR_RETURN(Value dist_val, ComputeDistance(centroids[c], vec));
          double d = dist_val.double_value();
          if (d < min_dist) {
            min_dist = d;
            best_c = c;
          }
        }
        assigned_vectors[best_c].push_back(vec);
        if (assignments[v_idx] != best_c) {
          assignments[v_idx] = best_c;
          changed = true;
        }
      }

      if (!changed) {
        break;
      }

      for (int64_t c = 0; c < actual_k; ++c) {
        if (assigned_vectors[c].empty()) {
          continue;
        }
        GOOGLESQL_ASSIGN_OR_RETURN(Value mean_vec,
                         ComputeMean(assigned_vectors[c], element_type));
        centroids[c] = mean_vec;
      }
    }
    return absl::OkStatus();
  }

  absl::Status PerformClustering() {
    int vector_col_idx = -1;
    for (int i = 0; i < base_iterator_->NumColumns(); ++i) {
      if (googlesql_base::CaseEqual(base_iterator_->GetColumnName(i),
                                 vector_column_name_)) {
        vector_col_idx = i;
        break;
      }
    }
    GOOGLESQL_RET_CHECK_NE(vector_col_idx, -1) << "Vector column not found";

    const Type* vector_type = base_iterator_->GetColumnType(vector_col_idx);
    if (!vector_type->IsArray() ||
        (!vector_type->AsArray()->element_type()->IsFloat() &&
         !vector_type->AsArray()->element_type()->IsDouble())) {
      return absl::OutOfRangeError(
          "The column specified by the `vectors_column` argument of KMeans TVF "
          "must be of type ARRAY<DOUBLE> or ARRAY<FLOAT>");
    }

    // 1. Extract the collection and validation of input vectors:
    GOOGLESQL_ASSIGN_OR_RETURN(ValidatedInput validated_input,
                     GetAndValidateInputVectors(vector_col_idx));

    // 2. Initialize centroids:
    std::vector<Value> current_centroids;
    GOOGLESQL_RETURN_IF_ERROR(InitCentroids(validated_input.distinct_vectors,
                                  /*restart_idx=*/0, current_centroids));

    // 3. Run the KMeans iterations:
    const Type* element_type = vector_type->AsArray()->element_type();
    GOOGLESQL_RETURN_IF_ERROR(RunKMeansIterations(validated_input.valid_vectors,
                                        element_type, current_centroids));

    centroids_ = std::move(current_centroids);
    return absl::OkStatus();
  }

  absl::StatusOr<Value> ComputeDistance(const Value& v1, const Value& v2) {
    switch (options_.distance_type()) {
      case KMeansOptions::EUCLIDEAN:
      case KMeansOptions::DISTANCE_TYPE_UNSPECIFIED:
        return functions::EuclideanDistanceDense(v1, v2);
      default:
        return absl::InternalError("Unknown distance type");
    }
  }

  absl::StatusOr<Value> ComputeMean(absl::Span<const Value> vectors,
                                    const Type* element_type) {
    GOOGLESQL_RET_CHECK(!vectors.empty()) << "Empty vectors for mean";
    size_t len = vectors[0].elements().size();
    std::vector<double> sums(len, 0.0);
    for (const Value& vec : vectors) {
      for (size_t i = 0; i < len; ++i) {
        double val = element_type->IsFloat() ? vec.elements()[i].float_value()
                                             : vec.elements()[i].double_value();
        sums[i] += val;
      }
    }
    std::vector<Value> mean_elems;
    mean_elems.reserve(len);
    double count = vectors.size();
    for (size_t i = 0; i < len; ++i) {
      double m = sums[i] / count;
      if (element_type->IsFloat()) {
        mean_elems.push_back(Value::Float(static_cast<float>(m)));
      } else {
        mean_elems.push_back(Value::Double(m));
      }
    }
    return Value::Array(output_columns_[1].type->AsArray(), mean_elems);
  }

  std::unique_ptr<EvaluatorTableIterator> base_iterator_;
  std::string vector_column_name_;
  int64_t k_;
  KMeansOptions options_;
  std::vector<TVFSchemaColumn> output_columns_;
  std::unique_ptr<TypeFactory> type_factory_;

  bool clustered_ = false;
  std::vector<Value> centroids_;
  int current_centroid_idx_ = 0;
  std::array<Value, 2> current_output_values_;
  absl::Status status_;
};

}  // namespace

absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>>
KMeansTVF::CreateEvaluator(
    std::vector<TableValuedFunction::TvfEvaluatorArg> args,
    std::shared_ptr<FunctionSignature> function_call_signature,
    std::shared_ptr<const TVFSignature> tvf_signature,
    EvaluationContext* context) {
  if (context) {
    context->SetNonDeterministicOutput();
  }
  GOOGLESQL_RET_CHECK(args.size() == 4);
  std::unique_ptr<EvaluatorTableIterator> input_table_iter =
      std::move(args[0].relation);
  GOOGLESQL_RET_CHECK(input_table_iter != nullptr);

  GOOGLESQL_RET_CHECK(args[1].value.has_value() && args[1].value->type()->IsString());
  std::string vectors_column_name = args[1].value->string_value();

  GOOGLESQL_RET_CHECK(args[2].value.has_value() && args[2].value->type()->IsInt64());
  int64_t k = args[2].value->int64_value();
  if (k <= 0) {
    return absl::OutOfRangeError(
        "Argument 'k' to table-valued function KMEANS must be at least 1");
  }

  KMeansOptions options = DefaultKMeansOptions();

  if (args[3].value.has_value() && !args[3].value->is_null()) {
    GOOGLESQL_RET_CHECK(args[3].value->type()->IsProto());
    if (!options.MergeFromString(args[3].value->proto_value())) {
      return absl::OutOfRangeError("Invalid options proto in KMEANS");
    }
  }

  if (options.num_iterations() < 1) {
    return absl::OutOfRangeError(
        "num_iterations in KMeansOptions must be at least 1");
  }
  if (options.num_restarts() < 1) {
    return absl::OutOfRangeError(
        "num_restarts in KMeansOptions must be at least 1");
  }
  if (options.min_relative_progress() < 0.0) {
    return absl::OutOfRangeError(
        "min_relative_progress in KMeansOptions must be non-negative");
  }
  if (options.distance_type() == KMeansOptions::DISTANCE_TYPE_UNSPECIFIED) {
    return absl::OutOfRangeError(
        "distance_type in KMeansOptions must not be set to unspecified");
  }
  if (options.init_method() == KMeansOptions::INIT_METHOD_UNSPECIFIED) {
    return absl::OutOfRangeError(
        "init_method in KMeansOptions must not be set to unspecified");
  }

  std::vector<TVFSchemaColumn> output_columns;
  auto type_factory = std::make_unique<googlesql::TypeFactory>();
  output_columns.push_back({"cluster_id", type_factory->get_int64()});

  int vector_col_idx = -1;
  int found_count = 0;
  for (int i = 0; i < input_table_iter->NumColumns(); ++i) {
    if (googlesql_base::CaseEqual(input_table_iter->GetColumnName(i),
                               vectors_column_name)) {
      vector_col_idx = i;
      found_count++;
    }
  }
  if (vector_col_idx == -1) {
    return absl::OutOfRangeError(absl::Substitute(
        "Unrecognized name $0 in input table argument", vectors_column_name));
  }
  if (found_count > 1) {
    return absl::OutOfRangeError(absl::Substitute(
        "Column $0 is ambiguous in the base table", vectors_column_name));
  }
  const Type* vector_type = input_table_iter->GetColumnType(vector_col_idx);
  output_columns.push_back({"cluster_vector", vector_type});

  return std::make_unique<KMeansResultIterator>(
      std::move(input_table_iter), vectors_column_name, k, options,
      std::move(output_columns), std::move(type_factory));
}

}  // namespace googlesql
