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

#include "googlesql/public/sql_tvf.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/analyzer/name_scope.h"
#include "googlesql/analyzer/resolver.h"
#include "googlesql/common/errors.h"
#include "googlesql/common/status_payload_utils.h"
#include "googlesql/parser/ast_node_kind.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/proto/function.pb.h"
#include "googlesql/proto/internal_error_location.pb.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output_properties.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/cycle_detector.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/id_string.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/types/annotation.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/base/casts.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/base/map_util.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {

// static
absl::Status SQLTableValuedFunction::Create(
    const ::googlesql::ResolvedCreateTableFunctionStmt* create_tvf_statement,
    std::unique_ptr<SQLTableValuedFunction>* simple_sql_tvf) {
  return Create(
      create_tvf_statement, /*tvf_options=*/ {}, simple_sql_tvf);
}

// static
absl::Status SQLTableValuedFunction::Create(
    const ::googlesql::ResolvedCreateTableFunctionStmt* create_tvf_statement,
    TableValuedFunctionOptions tvf_options,
    std::unique_ptr<SQLTableValuedFunction>* simple_sql_tvf) {
  GOOGLESQL_RET_CHECK_NE(create_tvf_statement, nullptr);
  // Only SQL TVFs are supported.
  GOOGLESQL_RET_CHECK_NE(create_tvf_statement->query(), nullptr);
  // Only non-templated SQL TVFs are supported.
  GOOGLESQL_RET_CHECK(!create_tvf_statement->signature().IsTemplated());

  GOOGLESQL_RETURN_IF_ERROR(
      create_tvf_statement->signature().IsValidForTableValuedFunction());

  simple_sql_tvf->reset(
      new SQLTableValuedFunction(create_tvf_statement, tvf_options));
  (*simple_sql_tvf)->set_sql_security(create_tvf_statement->sql_security());
  return absl::OkStatus();
}

static absl::StatusOr<bool> TableArgHasEqualAnnotationsToDefinition(
    const TVFInputArgumentType& argument,
    const FunctionArgumentType& declaration) {
  GOOGLESQL_RET_CHECK(argument.is_relation());
  GOOGLESQL_RET_CHECK(declaration.IsFixedRelation());

  const TVFRelation& argument_relation = argument.relation();
  const TVFRelation& declaration_relation =
      declaration.options().relation_input_schema();

  GOOGLESQL_RET_CHECK_EQ(argument_relation.columns().size(),
               declaration_relation.columns().size());
  for (int i = 0; i < argument_relation.columns().size(); ++i) {
    const TVFSchemaColumn& argument_column = argument_relation.column(i);
    const TVFSchemaColumn& declaration_column = declaration_relation.column(i);
    GOOGLESQL_RET_CHECK(!argument_column.is_pseudo_column);
    GOOGLESQL_RET_CHECK(!declaration_column.is_pseudo_column);

    GOOGLESQL_RET_CHECK(argument_column.type->Equals(declaration_column.type))
        << "Type mismatch: " << argument_column.type->DebugString() << " vs "
        << declaration_column.type->DebugString();
    if (!AnnotationMap::Equals(argument_column.annotation_map,
                               declaration_column.annotation_map)) {
      return false;
    }
  }
  return true;
}

static absl::StatusOr<bool> ArgsHaveSameAnnotationsAsDefinition(
    absl::Span<const TVFInputArgumentType> actual_arguments,
    const ResolvedCreateTableFunctionStmt& create_tvf_statement) {
  GOOGLESQL_RET_CHECK_EQ(actual_arguments.size(),
               create_tvf_statement.signature().arguments().size());
  for (int i = 0; i < actual_arguments.size(); ++i) {
    const TVFInputArgumentType& argument = actual_arguments[i];
    if (argument.is_scalar()) {
      GOOGLESQL_ASSIGN_OR_RETURN(AnnotatedType annotated_type,
                       argument.GetScalarArgAnnotatedType());
      if (!AnnotationMap::IsNullOrEmpty(annotated_type.annotation_map)) {
        return false;
      }
    } else if (argument.is_relation()) {
      GOOGLESQL_ASSIGN_OR_RETURN(bool arg_annotations_equal_to_definition,
                       TableArgHasEqualAnnotationsToDefinition(
                           actual_arguments[i],
                           create_tvf_statement.signature().argument(i)));
      if (!arg_annotations_equal_to_definition) {
        return false;
      }
    } else if (argument.is_connection() || argument.is_descriptor() ||
               argument.is_graph() || argument.is_model()) {
      // These argument types don't support annotations.
      continue;
    } else {
      GOOGLESQL_RET_CHECK_FAIL() << "Unexpected argument kind: "
                       << argument.DebugString();
    }
  }
  return true;
}

absl::Status SQLTableValuedFunction::ResolveInternal(
    const AnalyzerOptions* analyzer_options,
    const std::vector<TVFInputArgumentType>& actual_arguments,
    const FunctionSignature& concrete_signature, Catalog* catalog,
    TypeFactory* type_factory,
    std::shared_ptr<TVFSignature>* tvf_signature) const {
  // Note that the concrete signature might have deprecation warnings attached.
  // If so, then we need to propagate those deprecation warnings to the
  // returned signature.
  TVFSignatureOptions tvf_signature_options;
  tvf_signature_options.additional_deprecation_warnings =
      concrete_signature.AdditionalDeprecationWarnings();
  // If any arguments have annotations, treat it as a templated TVF and attach
  // a templated signature with a re-resolved body.
  GOOGLESQL_RET_CHECK(create_tvf_statement_ != nullptr);
  GOOGLESQL_ASSIGN_OR_RETURN(bool args_have_same_annotations_as_definition,
                   ArgsHaveSameAnnotationsAsDefinition(actual_arguments,
                                                       *create_tvf_statement_));
  if (!args_have_same_annotations_as_definition) {
    // Cannot use the optimization. We must resort to re-resolution and treat
    // this function as a templated TVF.
    return SQLTableValuedFunctionInterface::ResolveInternal(
        analyzer_options, actual_arguments, concrete_signature, catalog,
        type_factory, tvf_signature);
  }
  // We can just return the same cached resolved body!
  tvf_signature->reset(
      new TVFSignature(actual_arguments, tvf_schema_, tvf_signature_options));
  if (anonymization_info_ != nullptr) {
    auto anonymization_info =
        std::make_unique<AnonymizationInfo>(*anonymization_info_);
    tvf_signature->get()->SetAnonymizationInfo(std::move(anonymization_info));
  }
  return absl::OkStatus();
}

// static
TVFRelation SQLTableValuedFunction::GetQueryOutputSchema(
    const ResolvedCreateTableFunctionStmt& create_tvf_statement) {
  if (create_tvf_statement.is_value_table()) {
    return TVFRelation::ValueTable(
        create_tvf_statement.query()->column_list(0).annotated_type());
  }
  return create_tvf_statement.signature().result_type().options()
      .relation_input_schema();
}

absl::Status SQLTableValuedFunctionInterface::CheckIsValid() const {
  for (const FunctionSignature& signature : signatures_) {
    GOOGLESQL_RET_CHECK(std::all_of(
        signature.arguments().begin(), signature.arguments().end(),
        [](const FunctionArgumentType& arg) {
          return arg.required() || (arg.optional() && arg.HasDefault());
        }))
        << "Table-valued function declarations with argument(s) of templated "
        << "type do not support repeated arguments or non-default optional "
           "arguments when a SQL body is also present";
  }
  return absl::OkStatus();
}

absl::Status
SQLTableValuedFunctionInterface::ForwardNestedResolutionAnalysisError(
    const absl::Status& status, ErrorMessageOptions options) const {
  absl::Status new_status;
  if (status.ok()) {
    return absl::OkStatus();
  } else if (HasErrorLocation(status)) {
    new_status = MakeTVFQueryAnalysisError();
    googlesql::internal::AttachPayload(
        &new_status,
        SetErrorSourcesFromStatus(
            googlesql::internal::GetPayload<ErrorLocation>(status), status,
            options.mode, GetParseResumeLocation().input()));
  } else {
    new_status = StatusWithInternalErrorLocation(
        MakeTVFQueryAnalysisError(),
        ParseLocationPoint::FromByteOffset(
            GetParseResumeLocation().filename(),
            GetParseResumeLocation().byte_position()));
    googlesql::internal::AttachPayload(
        &new_status,
        SetErrorSourcesFromStatus(
            googlesql::internal::GetPayload<InternalErrorLocation>(new_status),
            status, options.mode, GetParseResumeLocation().input()));
  }
  // Update the <new_status> based on <mode>.
  return MaybeUpdateErrorFromPayload(
      options, GetParseResumeLocation().input(),
      ConvertInternalErrorPayloadsToExternal(new_status,
                                             GetParseResumeLocation().input()));
}

absl::Status SQLTableValuedFunctionInterface::MakeTVFQueryAnalysisError(
    absl::string_view message) const {
  std::string result =
      absl::StrCat("Analysis of table-valued function ", FullName(), " failed");
  if (!message.empty()) {
    absl::StrAppend(&result, ":\n", message);
  }
  return MakeSqlError() << result;
}

absl::Status SQLTableValuedFunctionInterface::ResolveInternal(
    const AnalyzerOptions* analyzer_options,
    const std::vector<TVFInputArgumentType>& input_arguments,
    const FunctionSignature& concrete_signature, Catalog* catalog,
    TypeFactory* type_factory,
    std::shared_ptr<TVFSignature>* tvf_signature) const {
  // TODO: Attach proper error locations to the returned Status.
  GOOGLESQL_RETURN_IF_ERROR(CheckIsValid());

  // Check if this function calls itself. If so, return an error. Otherwise, add
  // a pointer to this class to the cycle detector in the analyzer options.
  CycleDetector::ObjectInfo object(
      FullName(), this, analyzer_options->find_options().cycle_detector());
  // TODO: Attach proper error locations to the returned Status.
  GOOGLESQL_RETURN_IF_ERROR(object.DetectCycle("table function"));

  // Build maps for scalar and table-valued function arguments.
  IdStringHashMapCase<std::unique_ptr<ResolvedArgumentRef>> function_arguments;
  IdStringHashMapCase<TVFRelation> function_table_arguments;
  // TODO: Attach proper error locations to the returned Status.
  GOOGLESQL_RET_CHECK_EQ(GetArgumentNames().size(), input_arguments.size())
      << DebugString();
  for (int i = 0; i < input_arguments.size(); ++i) {
    const IdString tvf_arg_name =
        analyzer_options->id_string_pool()->Make(GetArgumentNames()[i]);
    const TVFInputArgumentType& tvf_arg_type = input_arguments[i];
    if (tvf_arg_type.is_relation()) {
      // TODO: Attach proper error locations to the returned Status.
      GOOGLESQL_RET_CHECK(googlesql_base::InsertIfNotPresent(&function_table_arguments, tvf_arg_name,
                                        tvf_arg_type.relation()));
    } else {
      // TODO: Attach proper error locations to the returned Status.
      GOOGLESQL_ASSIGN_OR_RETURN(AnnotatedType annotated_type,
                       tvf_arg_type.GetScalarArgAnnotatedType());
      if (function_arguments.contains(tvf_arg_name)) {
        // TODO: Attach proper error locations to the returned Status.
        return MakeTVFQueryAnalysisError(
            absl::StrCat("Duplicate argument name ", tvf_arg_name.ToString()));
      }
      auto arg_ref =
          MakeResolvedArgumentRef(annotated_type.type, tvf_arg_name.ToString(),
                                  ResolvedArgumentDefEnums::SCALAR);
      arg_ref->set_type_annotation_map(annotated_type.annotation_map);
      function_arguments[tvf_arg_name] = std::move(arg_ref);
    }
  }

  // Create a separate new parser and parse the templated TVFs SQL query body.
  // Use the same ID string pool from the original parser.
  ParserOptions parser_options(analyzer_options->id_string_pool(),
                               analyzer_options->arena(),
                               analyzer_options->language());
  std::unique_ptr<ParserOutput> parser_output;
  bool at_end_of_input = false;
  ParseResumeLocation this_parse_resume_location(GetParseResumeLocation());
  GOOGLESQL_RETURN_IF_ERROR(ForwardNestedResolutionAnalysisError(
      ParseNextStatement(&this_parse_resume_location, parser_options,
                         &parser_output, &at_end_of_input),
      analyzer_options->error_message_options()));
  if (parser_output->statement()->node_kind() != AST_QUERY_STATEMENT) {
    // TODO: Attach proper error locations to the returned Status.
    return MakeTVFQueryAnalysisError("SQL body is not a query");
  }

  if (resolution_catalog_ != nullptr) {
    catalog = resolution_catalog_;
  }

  // Create a separate new resolver and resolve the TVF's SQL query body, using
  // the specified function arguments. Note that if this resolver uses the
  // catalog passed into the class constructor, then the catalog may include
  // names that were not available when the function was initially declared.
  // TODO: This should share the output properties with the
  // enclosing query's resolver.
  AnalyzerOutputProperties analyzer_output_properties;
  Resolver resolver(catalog, type_factory, analyzer_options,
                    analyzer_output_properties);
  std::optional<TVFRelation> specified_output_schema;
  if (signatures_[0].result_type().options().has_relation_input_schema()) {
    specified_output_schema =
        signatures_[0].result_type().options().relation_input_schema();
  }
  std::unique_ptr<const ResolvedStatement> resolved_sql_body;
  std::shared_ptr<const NameList> tvf_body_name_list;
  GOOGLESQL_RETURN_IF_ERROR(ForwardNestedResolutionAnalysisError(
      resolver.ResolveQueryStatementWithFunctionArguments(
          GetParseResumeLocation().input(),
          static_cast<const ASTQueryStatement*>(parser_output->statement()),
          specified_output_schema, allow_query_parameters(),
          &function_arguments, &function_table_arguments, &resolved_sql_body,
          &tvf_body_name_list),
      analyzer_options->error_message_options()));
  // TODO: Attach proper error locations to the returned Status.
  GOOGLESQL_RET_CHECK_EQ(RESOLVED_QUERY_STMT, resolved_sql_body->node_kind());

  // Construct the output schema for the TemplatedSQLTVFSignature return object.
  TVFRelation return_tvf_relation({});
  if (specified_output_schema) {
    // Do not use the `specified_output_schema` directly as some extra
    // annotations may have propagated through the coercions, e.g. like lineage.
    std::vector<TVFRelation::Column> output_schema_columns;
    GOOGLESQL_RET_CHECK(resolved_sql_body->Is<ResolvedQueryStmt>());
    const auto* query_stmt = resolved_sql_body->GetAs<ResolvedQueryStmt>();
    GOOGLESQL_RET_CHECK_EQ(query_stmt->output_column_list_size(),
                 specified_output_schema->num_columns());
    output_schema_columns.reserve(query_stmt->output_column_list_size());
    for (int i = 0; i < query_stmt->output_column_list_size(); ++i) {
      const auto& output_column = query_stmt->output_column_list()[i];
      output_schema_columns.emplace_back(
          specified_output_schema->column(i).name,
          output_column->column().annotated_type(),
          specified_output_schema->column(i).is_pseudo_column,
          specified_output_schema->column(i).is_passthrough_column,
          specified_output_schema->column(i).type_modifiers);
    }
    if (specified_output_schema->is_value_table()) {
      GOOGLESQL_RET_CHECK_EQ(output_schema_columns.size(), 1);
      return_tvf_relation = TVFRelation::ValueTable(output_schema_columns[0]);
    } else {
      return_tvf_relation = TVFRelation(std::move(output_schema_columns));
    }
  } else if (tvf_body_name_list->is_value_table()) {
    // TODO: Attach proper error locations to the returned Status.
    GOOGLESQL_RET_CHECK_EQ(1, tvf_body_name_list->num_columns());
    return_tvf_relation = TVFRelation::ValueTable(
        tvf_body_name_list->column(0).column().annotated_type());
  } else {
    std::vector<TVFRelation::Column> output_schema_columns;
    output_schema_columns.reserve(tvf_body_name_list->num_columns());
    for (const NamedColumn& tvf_body_name_list_column :
         tvf_body_name_list->columns()) {
      // Check if any of the output column names are internally-generated. If
      // so, return an error since the enclosing query will never be able to
      // reference them. This behavior matches that of non-templated TVF calls.
      // TODO: Ideally make this work for a backquoted explicit column
      // that happens to return true for IsInternalAlias (e.g. `$col`).
      if (IsInternalAlias(tvf_body_name_list_column.name())) {
        // TODO: Attach proper error locations to the returned Status.
        return MakeTVFQueryAnalysisError(
            "Function body is missing one or more explicit output column "
            "names");
      }
      output_schema_columns.emplace_back(
          tvf_body_name_list_column.name().ToString(),
          tvf_body_name_list_column.column().annotated_type());
    }
    return_tvf_relation = TVFRelation(output_schema_columns);
  }

  TVFSignatureOptions tvf_signature_options;
  tvf_signature_options.additional_deprecation_warnings =
      concrete_signature.AdditionalDeprecationWarnings();

  // Return the final TVFSignature and resolved templated query.
  std::unique_ptr<const ResolvedQueryStmt> resolved_templated_query(
      static_cast<const ResolvedQueryStmt*>(resolved_sql_body.release()));
  tvf_signature->reset(new TemplatedSQLTVFSignature(
      input_arguments, return_tvf_relation, tvf_signature_options,
      std::move(resolved_templated_query), GetArgumentNames()));
  if (anonymization_info_ != nullptr) {
    auto anonymization_info =
        std::make_unique<AnonymizationInfo>(*anonymization_info_);
    tvf_signature->get()->SetAnonymizationInfo(std::move(anonymization_info));
  }
  return absl::OkStatus();
}

}  // namespace googlesql
