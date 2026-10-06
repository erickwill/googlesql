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

#include "googlesql/tools/execute_query/execute_query_web_handler.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/base/path.h"
#include "googlesql/common/options_utils.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/resolved_ast/sql_builder.h"
#include "googlesql/tools/execute_query/execute_query_tool.h"
#include "googlesql/tools/execute_query/web/embedded_resources.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/flags/flag.h"
#include "absl/random/random.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "file_based_test_driver/file_based_test_driver.h"
#include "file_based_test_driver/run_test_case_result.h"
#include "file_based_test_driver/test_case_options.h"

using testing::ContainsRegex;
using testing::Eq;
using testing::HasSubstr;
using testing::StartsWith;

namespace googlesql {

namespace {

class FakeQueryWebTemplates : public QueryWebTemplates {
 public:
  FakeQueryWebTemplates(std::string contents, std::string css, std::string body,
                        std::string inline_js = "")
      : contents_(std::move(contents)),
        css_(std::move(css)),
        body_(std::move(body)),
        inline_js_(std::move(inline_js)) {}
  ~FakeQueryWebTemplates() override = default;

  const std::string& GetWebPageContents() const override { return contents_; }
  const std::string& GetWebPageCSS() const override { return css_; }
  const std::string& GetWebPageBody() const override { return body_; }
  const std::string& GetInlineJS() const override { return inline_js_; }

 private:
  std::string contents_;
  std::string css_;
  std::string body_;
  std::string inline_js_;
};

bool HandleRequest(const ExecuteQueryWebRequest& request,
                   const QueryWebTemplates& templates, std::string& result) {
  ExecuteQueryWebHandler handler(templates);
  return handler.HandleRequest(request, [&result](absl::string_view s) {
    result = s;
    return true;
  });
}

}  // namespace

TEST(ExecuteQueryWebHandlerTest, TestCSS) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, "",
                             "none", "MAXIMUM", "", "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("CSS: {{css}}", "some_css", ""), result));
  EXPECT_THAT(result, Eq("CSS: some_css"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryPreserved) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, "foo bar",
                             "none", "MAXIMUM", "", "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "", "Query: {{query}}"), result));
  EXPECT_THAT(result, Eq("Query: foo bar"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryPreservedForScriptMode) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kScript,
                             SQLBuilder::TargetSyntaxMode::kStandard, "foo bar",
                             "none", "MAXIMUM", "", "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "", "Query: {{query}}"), result));
  EXPECT_THAT(result, Eq("Query: foo bar"));
}

TEST(ExecuteQueryWebHandlerTest, TestModesPreserved) {
  std::vector<std::string> modes = {"analyze", "explain", "parse"};
  std::string mode_template =
      "{{mode_analyze}}-{{mode_explain}}-{{mode_parse}}";
  std::string result;
  absl::BitGen bitgen;
  for (int i = 0; i < 20; i++) {
    for (int subset_size = 0; subset_size <= modes.size(); subset_size++) {
      std::vector<std::string> subset = modes;
      std::shuffle(subset.begin(), subset.end(), bitgen);
      subset.resize(subset_size);

      std::vector<std::string> expected_v;
      for (const std::string& s : modes) {
        expected_v.push_back(absl::c_linear_search(subset, s) ? "true" : "");
      }
      std::string expected = absl::StrJoin(expected_v, "-");

      EXPECT_TRUE(HandleRequest(
          ExecuteQueryWebRequest(subset, ExecuteQueryConfig::SqlMode::kQuery,
                                 SQLBuilder::TargetSyntaxMode::kStandard,
                                 "foo bar", "none", "MAXIMUM", "",
                                 "ALL_MINUS_DEV", ""),
          FakeQueryWebTemplates("{{> body}}", "", mode_template), result));
      EXPECT_THAT(result, Eq(expected))
          << "Failed for subset [" << absl::StrJoin(subset, ",") << "]";
    }
  }
}

TEST(ExecuteQueryWebHandlerTest, TestSqlModesPreserved) {
  std::vector<ExecuteQueryConfig::SqlMode> sql_modes = {
      ExecuteQueryConfig::SqlMode::kQuery,
      ExecuteQueryConfig::SqlMode::kExpression,
      ExecuteQueryConfig::SqlMode::kScript};
  std::string sql_mode_template =
      "{{sql_mode_query}}-{{sql_mode_expression}}-{{sql_mode_script}}";
  std::vector<std::string> expected_results = {"true--", "-true-", "--true"};
  std::string result;
  for (int index = 0; index < sql_modes.size(); ++index) {
    EXPECT_TRUE(HandleRequest(
        ExecuteQueryWebRequest(
            /*ToolMode is tested separately*/ {"execute"}, sql_modes[index],
            SQLBuilder::TargetSyntaxMode::kStandard, "foo bar", "none",
            "MAXIMUM", "", "ALL_MINUS_DEV", ""),
        FakeQueryWebTemplates("{{> body}}", "", sql_mode_template), result));
    EXPECT_THAT(result, Eq(expected_results[index]))
        << "Failed for subset [" << expected_results[index] << "]";
  }
}

TEST(ExecuteQueryWebHandlerTest, TestTargetSyntaxModesPreserved) {
  std::vector<SQLBuilder::TargetSyntaxMode> target_syntax_modes = {
      SQLBuilder::TargetSyntaxMode::kStandard,
      SQLBuilder::TargetSyntaxMode::kPipe};
  std::string target_syntax_mode_template =
      "{{target_syntax_mode_standard}}-{{target_syntax_mode_pipe}}";
  std::vector<std::string> expected_results = {"true-", "-true"};
  std::string result;
  for (int index = 0; index < target_syntax_modes.size(); ++index) {
    EXPECT_TRUE(HandleRequest(
        ExecuteQueryWebRequest(/*ToolMode is tested separately*/ {"execute"},
                               ExecuteQueryConfig::SqlMode::kQuery,
                               target_syntax_modes[index], "foo bar", "none",
                               "MAXIMUM", "", "ALL_MINUS_DEV", ""),
        FakeQueryWebTemplates("{{> body}}", "", target_syntax_mode_template),
        result));
    EXPECT_THAT(result, Eq(expected_results[index]))
        << "Failed for subset [" << expected_results[index] << "]";
  }
}

TEST(ExecuteQueryWebHandlerTest, TestQueryEscaped) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "</select> Exploit!", "none", "MAXIMUM", "",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "", "Query: {{query}}"), result));
  EXPECT_THAT(result, Eq("Query: &lt;&#x2F;select&gt; Exploit!"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryResultPresent) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "", "ALL_MINUS_DEV",
                             ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{result}}-{{error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, Eq("true-"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryExecutedSimpleResult) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "DESCRIBE RAND", "none", "MAXIMUM", "",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#result_executed_tables}}\n"
                            "{{#table}}{{> table}}{{/table}}\n"
                            "{{#error}}error:{{{error}}}{{/error}}\n"
                            "{{/result_executed_tables}}\n"
                            "{{/statements}}"),
      result));
  // We have one test showing the HTML for a table that comes out.
  // If this is too volatile, it can be replaced with a placeholder that
  // just checks some table is emitted.
  EXPECT_THAT(
      result,
      Eq("<table>\n  <thead>\n    <tr>\n        <th>#&zwnj;</th>\n        "
         "<th>Describe&zwnj;</th>\n    </tr>\n  </thead>\n  <tbody>\n      "
         "<tr>\n          <td>1</td>\n          <td>Function RAND\nSignature: "
         "RAND() -&gt; DOUBLE</td>\n      </tr>\n  </tbody>\n</table>\n\n"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryErrorPresent) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "bad request", "none", "MAXIMUM", "",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{result}}-{{error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, StartsWith("-"));
  EXPECT_THAT(result, HasSubstr("Syntax error"));
}

TEST(ExecuteQueryWebHandlerTest, TestCatalogUsed) {
  auto web_template =
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{{error}}}"
                            "{{#result_executed_tables}}"
                            "{{#table}}(table){{/table}}"
                            "{{#error}}error:{{{error}}}{{/error}}"
                            "{{/result_executed_tables}}"
                            "{{/statements}}");

  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "DESCRIBE Value", "none", "MAXIMUM", "",
                             "ALL_MINUS_DEV", ""),
      web_template, result));
  EXPECT_THAT(result, Eq("INVALID_ARGUMENT: Object not found"));

  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "DESCRIBE Value", "sample", "MAXIMUM", "",
                             "ALL_MINUS_DEV", ""),
      web_template, result));
  EXPECT_THAT(result, Eq("(table)"));
}

TEST(ExecuteQueryWebHandlerTest, TestEchoStatement) {
  std::string result;
  const auto web_template = FakeQueryWebTemplates("{{> body}}", "",
                                                  "{{#statements}}"
                                                  "{{#show_statement_text}}"
                                                  "{{statement_text}}"
                                                  "{{/show_statement_text}}"
                                                  "\n---\n"
                                                  "{{/statements}}");

  // With one input statement, show_statement_text isn't set.
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1;", "none", "MAXIMUM", "",
                             "ALL_MINUS_DEV", ""),
      web_template, result));
  EXPECT_THAT(result, Eq("\n---\n"));

  // With more than one input statement, show_statement_text is set.
  // Newlines are stripped off the beginning and end of statement_text.
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "\n\n\r\nSELECT 1;\n\r\nSELECT\n  2;\n\r\n",
                             "none", "MAXIMUM", "", "ALL_MINUS_DEV", ""),
      web_template, result));
  EXPECT_THAT(result, Eq("SELECT 1;\n---\nSELECT\n  2;\n---\n"));
}

static void RunFileBasedTest(
    absl::string_view test_case_input,
    file_based_test_driver::RunTestCaseResult* test_result) {
  absl::SetFlag(&FLAGS_parse_location_record_type, "FULL_NODE_SCOPE");
  file_based_test_driver::TestCaseOptions test_case_options;
  test_case_options.RegisterString("mode", "execute");
  test_case_options.RegisterString("sql_mode", "query");
  test_case_options.RegisterString("target_syntax_mode", "standard");
  test_case_options.RegisterString("catalog", "sample");
  test_case_options.RegisterString("enabled_language_features", "MAXIMUM");
  test_case_options.RegisterString("enabled_ast_rewrites", "ALL_MINUS_DEV");
  std::string input_sql = std::string(test_case_input);
  GOOGLESQL_ASSERT_OK(test_case_options.ParseTestCaseOptions(&input_sql));
  std::string result;
  std::vector<std::string> modes =
      absl::StrSplit(test_case_options.GetString("mode"), ' ');
  FakeQueryWebTemplates web_template(
      "{{> body}}", "",
      "query:{{query}}\n"
      "{{#mode_execute}}mode_execute\n{{/mode_execute}}"
      "{{#mode_analyze}}mode_analyze\n{{/mode_analyze}}"
      "{{#mode_parse}}mode_parse\n{{/mode_parse}}"
      "{{#mode_explain}}mode_explain\n{{/mode_explain}}"
      "{{#mode_unanalyze}}mode_unanalyze\n{{/mode_unanalyze}}"
      "{{#mode_unparse}}mode_unparse\n{{/mode_unparse}}"
      "{{#sql_mode_query}}sql_mode_query\n{{/sql_mode_query}}"
      "{{#sql_mode_expression}}sql_mode_expression\n{{/sql_mode_expression}}"
      "{{#sql_mode_script}}sql_mode_script\n{{/sql_mode_script}}"
      "{{#target_syntax_mode_standard}}target_syntax_mode_standard\n"
      "{{/target_syntax_mode_standard}}"
      "{{#target_syntax_mode_pipe}}target_syntax_mode_pipe\n"
      "{{/target_syntax_mode_pipe}}"
      "{{#statements}}\n"
      "{{#show_statement_text}}\n"
      "{{#statement_text}}\n"
      "statement_text:{{statement_text}}\n"
      "{{/statement_text}}\n"
      "{{/show_statement_text}}\n"
      "{{#error}}\n"
      "error:{{{error}}}\n"
      "{{/error}}\n"
      "{{#result}}\n"
      "{{#result_executed}}\n"
      "{{#result_executed_tables}}\n"
      "{{#table}}\n"
      "table:{{> table}}\n"
      "{{/table}}\n"
      "{{#error}}\n"
      "error:{{{error}}}\n"
      "{{/error}}\n"
      "{{/result_executed_tables}}\n"
      "{{#result_executed_text}}\n"
      "result_executed_text:{{result_executed_text}}\n"
      "{{/result_executed_text}}\n"
      "{{/result_executed}}\n"
      "{{#result_analyzed}}\n"
      "result_analyzed:{{{result_analyzed}}}\n"
      "{{/result_analyzed}}\n"
      "{{#result_parsed}}\n"
      "result_parsed:{{result_parsed}}\n"
      "{{/result_parsed}}\n"
      "{{#result_explained }}\n"
      "result_explained:{{result_explained}}\n"
      "{{/result_explained}}\n"
      "{{#result_unanalyzed}}\n"
      "result_unanalyzed:{{result_unanalyzed}}\n"
      "{{/result_unanalyzed}}\n"
      "{{#result_unparsed}}\n"
      "result_unparsed:{{result_unparsed}}\n"
      "{{/result_unparsed}}\n"
      "{{#result_log}}\n"
      "result_log:{{result_log}}\n"
      "{{/result_log}}\n"
      "{{/result}}\n"
      "{{/statements}}");

  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          modes,
          ExecuteQueryConfig::parse_sql_mode(
              test_case_options.GetString("sql_mode")),
          ExecuteQueryConfig::parse_target_syntax_mode(
              test_case_options.GetString("target_syntax_mode")),
          input_sql, test_case_options.GetString("catalog"),
          test_case_options.GetString("enabled_language_features"), "",
          test_case_options.GetString("enabled_ast_rewrites"), ""),
      web_template, result));

  test_result->AddTestOutput(result);
}

TEST(ExecuteQueryWebHandlerTest, FileBasedTest) {
  const std::string pattern =
      googlesql_base::JoinPath(::testing::SrcDir(),
                     "_main/googlesql/tools/execute_query/"
                     "testdata/execute_query_web_handler.test");

  EXPECT_TRUE(file_based_test_driver::RunTestCasesFromFiles(pattern,
                                                            &RunFileBasedTest));
}

TEST(ExecuteQueryWebHandlerTest, TestFlagDefaults) {
  // Tests that the default values for catalog, enabled_language_features, and
  // enabled_ast_rewrites are loaded from flags when no query parameters are
  // provided.
  absl::SetFlag(&FLAGS_catalog, "sample");

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(internal::EnumOptionsEntry<LanguageFeature> dev_features,
                       internal::ParseEnabledLanguageFeatures("DEV"));
  internal::EnabledLanguageFeatures enabled_language_features = {
      .enabled_language_features = dev_features.options};
  absl::SetFlag(&FLAGS_enabled_language_features,
                std::make_optional(enabled_language_features));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      internal::EnumOptionsEntry<ResolvedASTRewrite> none_rewrites,
      internal::ParseEnabledAstRewrites("NONE"));
  internal::EnabledAstRewrites enabled_ast_rewrites = {
      .enabled_ast_rewrites = none_rewrites.options};
  absl::SetFlag(&FLAGS_enabled_ast_rewrites, enabled_ast_rewrites);

  absl::SetFlag(&FLAGS_linear_resolved_ast, true);

  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          /*str_modes=*/{""}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard, /*query=*/"",
          /*catalog=*/"", /*enabled_language_features=*/"",
          /*enabled_language_features_text=*/"",
          /*enabled_ast_rewrites=*/"",
          /*enabled_ast_rewrites_text=*/""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "Catalog: "
                            "{{#catalogs}}"
                            "{{name}}-{{selected}} "
                            "{{/catalogs}}\n"
                            "Features: "
                            "{{#language_features}}"
                            "{{name}}-{{selected}} "
                            "{{/language_features}}\n"
                            "Rewrites: "
                            "{{#ast_rewrites}}"
                            "{{name}}-{{selected}} "
                            "{{/ast_rewrites}}\n"
                            "RenderMode: "
                            "{{#resolved_ast_render_mode_linear}}linear{{/"
                            "resolved_ast_render_mode_linear}}"
                            "{{#resolved_ast_render_mode_tree}}tree{{/"
                            "resolved_ast_render_mode_tree}}"),
      result));
  EXPECT_THAT(result, ContainsRegex("Catalog:.* sample-selected"));
  EXPECT_THAT(result, ContainsRegex("Features:.* DEV-selected "));
  EXPECT_THAT(result, ContainsRegex("Rewrites:.* NONE-selected "));
  EXPECT_THAT(result, ContainsRegex("RenderMode: linear"));
}

TEST(ExecuteQueryWebHandlerTest, TestRequestOverrides) {
  // Tests that query parameters override the flag defaults.
  absl::SetFlag(&FLAGS_catalog, "sample");

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(internal::EnumOptionsEntry<LanguageFeature> dev_features,
                       internal::ParseEnabledLanguageFeatures("DEV"));
  internal::EnabledLanguageFeatures enabled_language_features = {
      .enabled_language_features = dev_features.options};
  absl::SetFlag(&FLAGS_enabled_language_features,
                std::make_optional(enabled_language_features));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      internal::EnumOptionsEntry<ResolvedASTRewrite> all_minus_dev_rewrites,
      internal::ParseEnabledAstRewrites("ALL_MINUS_DEV"));
  internal::EnabledAstRewrites enabled_ast_rewrites = {
      .enabled_ast_rewrites = all_minus_dev_rewrites.options};
  absl::SetFlag(&FLAGS_enabled_ast_rewrites, enabled_ast_rewrites);
  absl::SetFlag(&FLAGS_linear_resolved_ast, true);

  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          /*str_modes=*/{""}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard, /*query=*/"",
          /*catalog=*/"none", /*enabled_language_features=*/"MAXIMUM", "",
          /*enabled_ast_rewrites=*/"ALL_MINUS_DEV", "",
          /*resolved_ast_render_mode=*/"tree"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "Catalog: "
                            "{{#catalogs}}"
                            "{{name}}-{{selected}} "
                            "{{/catalogs}}\n"
                            "Features: "
                            "{{#language_features}}"
                            "{{name}}-{{selected}} "
                            "{{/language_features}}\n"
                            "Rewrites: "
                            "{{#ast_rewrites}}"
                            "{{name}}-{{selected}} "
                            "{{/ast_rewrites}}\n"
                            "RenderMode: "
                            "{{#resolved_ast_render_mode_linear}}linear{{/"
                            "resolved_ast_render_mode_linear}}"
                            "{{#resolved_ast_render_mode_tree}}tree{{/"
                            "resolved_ast_render_mode_tree}}"),
      result));
  EXPECT_THAT(result, ContainsRegex("Catalog:.* none-selected .*"));
  EXPECT_THAT(result, ContainsRegex("Features:.* MAXIMUM-selected .*"));
  EXPECT_THAT(result, ContainsRegex("Rewrites:.* ALL_MINUS_DEV-selected "));
  EXPECT_THAT(result, ContainsRegex("RenderMode: tree"));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledLanguageFeaturesTextAddsFeature) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "FROM (SELECT 1)", "none", "NONE", "+PIPES",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}error:{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  // Adding PIPES enables the piped query syntax.
  EXPECT_THAT(result, Not(HasSubstr("error:")));
}

TEST(ExecuteQueryWebHandlerTest,
     TestEnabledLanguageFeaturesTextRemovesFeature) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "FROM (SELECT 1)", "none", "MAXIMUM", "-PIPES",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}error:{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  // Removing PIPES disables the piped query syntax.
  EXPECT_THAT(result, HasSubstr("error:INVALID_ARGUMENT: Syntax error"));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledLanguageFeaturesTextError) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "+INVALID",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(
      result,
      AllOf(HasSubstr("Error parsing enabled language features:"),
            HasSubstr("Invalid Feature entry: INVALID. Enum value not found")));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledLanguageFeaturesTextPrefixError) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM",
                             "+FEATURE_TABLESAMPLE", "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(
      result,
      AllOf(HasSubstr("Error parsing enabled language features:"),
            HasSubstr("Invalid Feature entry: FEATURE_TABLESAMPLE. For "
                      "consistency, do not include the FEATURE_ prefix")));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledLanguageFeaturesTextPrefixError2) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "TABLESAMPLE",
                             "ALL_MINUS_DEV", ""),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result,
              AllOf(HasSubstr("Error parsing enabled language features:"),
                    HasSubstr("Invalid Feature entry: TABLESAMPLE. Entries "
                              "should be prefixed with")));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledAstRewritesTextAddSubtract) {
  std::string result;
  std::string query =
      "SELECT FLATTEN(a1.a2) FROM (SELECT ARRAY<STRUCT<a2 "
      "ARRAY<INT64>>>[STRUCT([1]), STRUCT([1])] AS a1)";
  std::string expected_text_without_rewrite =
      ("<span class=\"ast-node\">Flatten</span>");
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"analyze"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, query,
                             "none", "MAXIMUM", "", "NONE", "+FLATTEN"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#has_rewrites}}"
                            "rewrites applied: "
                            "{{#result_analyzed_rewrites}}{{step_index}}={{"
                            "rewriter_name}}:{{{ast}}}{{/"
                            "result_analyzed_rewrites}}"
                            "{{/has_rewrites}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, HasSubstr("1=FlattenRewriter:"));
  EXPECT_THAT(result, Not(HasSubstr(expected_text_without_rewrite)));

  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"analyze"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, query,
                             "none", "MAXIMUM", "", "DEFAULTS", "-FLATTEN"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#result_analyzed}}"
                            "{{{result_analyzed}}}"
                            "{{/result_analyzed}}"
                            "{{#has_rewrites}}"
                            "rewrites applied!"
                            "{{/has_rewrites}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, Not(HasSubstr("rewrites applied!")));
  EXPECT_THAT(result, HasSubstr(expected_text_without_rewrite));
}

TEST(ExecuteQueryWebHandlerTest, TestAstRewritesDefaultTemplateMarkup) {
  std::string result;
  std::string query =
      "SELECT FLATTEN(a1.a2) FROM (SELECT ARRAY<STRUCT<a2 "
      "ARRAY<INT64>>>[STRUCT([1]), STRUCT([1])] AS a1)";
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"analyze"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, query,
                             "none", "MAXIMUM", "", "NONE", "+FLATTEN"),
      QueryWebTemplates::Default(), result));
  EXPECT_THAT(result, HasSubstr("data-step=\"0\""));
  EXPECT_THAT(result, HasSubstr("data-step=\"1\""));
  EXPECT_THAT(result, HasSubstr("data-step=\"final\""));
  EXPECT_THAT(result, HasSubstr("Post-rewrite"));
  EXPECT_THAT(result, HasSubstr("rewrite-step-item"));
  EXPECT_THAT(result, HasSubstr("rewrite-ast-panel"));
  EXPECT_THAT(result, HasSubstr("diff-mode-selector"));
  EXPECT_THAT(result, HasSubstr("Side-by-side"));
  EXPECT_THAT(result, HasSubstr("diff-side-by-side"));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledAstRewritesTextError) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"analyze"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "", "ALL",
                             "+INVALID"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(
      result,
      AllOf(HasSubstr("Error parsing enabled AST rewrites:"),
            HasSubstr("Invalid Rewrite entry: INVALID. Enum value not found")));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledAstRewritesTextPrefixError) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"analyze"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "", "ALL",
                             "+REWRITE_FLATTEN"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(
      result,
      AllOf(HasSubstr("Error parsing enabled AST rewrites:"),
            HasSubstr("Invalid Rewrite entry: REWRITE_FLATTEN. For "
                      "consistency, do not include the REWRITE_ prefix")));
}

TEST(ExecuteQueryWebHandlerTest, TestEnabledAstRewritesTextPrefixError2) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"analyze"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "", "ALL",
                             "FLATTEN"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#error}}{{{error}}}{{/error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, AllOf(HasSubstr("Error parsing enabled AST rewrites:"),
                            HasSubstr("Invalid Rewrite entry: FLATTEN. Entries "
                                      "should be prefixed with")));
}

}  // namespace googlesql
