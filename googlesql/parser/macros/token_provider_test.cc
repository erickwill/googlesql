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

#include "googlesql/parser/macros/token_provider.h"

#include <optional>
#include <ostream>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/public/parse_location.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {
namespace macros {

using ::testing::_;
using ::testing::Eq;
using ::testing::ExplainMatchResult;
using ::testing::FieldsAre;
using ::absl_testing::IsOk;

// Template specialization to print tokens in failed test messages.
[[maybe_unused]] static void PrintTo(const TokenWithLocation& token,
                                     std::ostream* os) {
  *os << absl::StrFormat(
      "(kind: %i, location: %s, topmost_invocation_location:%s, text: '%s', "
      "prev_spaces: '%s')",
      token.kind, token.location.GetString(),
      token.topmost_invocation_location.GetString(), token.text,
      token.preceding_whitespaces);
}

MATCHER_P(IsOkAndHoldsToken, expected, "") {
  return ExplainMatchResult(IsOk(), arg, result_listener) &&
         ExplainMatchResult(
             FieldsAre(Eq(expected.kind), Eq(expected.location),
                       Eq(expected.text), Eq(expected.preceding_whitespaces),
                       Eq(expected.topmost_invocation_location),
                       _ /* parent */),
             arg.value(), result_listener);
}

static absl::string_view kFileName = "<filename>";

static ParseLocationRange MakeLocation(int start_offset, int end_offset) {
  ParseLocationRange location;
  location.set_start(
      ParseLocationPoint::FromByteOffset(kFileName, start_offset));
  location.set_end(ParseLocationPoint::FromByteOffset(kFileName, end_offset));
  return location;
}

static TokenProvider MakeTokenProvider(absl::string_view input) {
  return TokenProvider(kFileName, input,
                       /*start_offset=*/0, /*end_offset=*/std::nullopt,
                       /*offset_in_original_input=*/0);
}

static TokenWithLocation MakeIntToken(absl::string_view text, int start,
                                      int end, absl::string_view spaces = "") {
  return TokenWithLocation{
      .kind = Token::DECIMAL_INTEGER_LITERAL,
      .location = MakeLocation(start, end),
      .text = text,
      .preceding_whitespaces = spaces,
  };
}

static TokenWithLocation MakeEoiToken(int offset) {
  return TokenWithLocation{
      .kind = Token::EOI,
      .location = MakeLocation(offset, offset),
      .text = "",
      .preceding_whitespaces = "",
  };
}

TEST(TokenProviderTest, RawTokenizerMode) {
  absl::string_view input = "/*comment*/ 123";

  TokenProvider token_provider = MakeTokenProvider(input);
  EXPECT_THAT(token_provider.GetNextToken(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::COMMENT,
                  .location = MakeLocation(0, 11),
                  .text = "/*comment*/",
                  .preceding_whitespaces = "",
              }));

  EXPECT_THAT(token_provider.GetNextToken(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::DECIMAL_INTEGER_LITERAL,
                  .location = MakeLocation(12, 15),
                  .text = "123",
                  .preceding_whitespaces = " ",
              }));
}

TEST(TokenProviderTest, AlwaysEndsWithEOF) {
  absl::string_view input = "\t\t";
  TokenProvider token_provider = MakeTokenProvider(input);
  EXPECT_THAT(token_provider.PeekNextToken(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::EOI,
                  .location = MakeLocation(2, 2),
                  .text = "",
                  .preceding_whitespaces = "\t\t",
              }));
  // Preceding whitespaces are stripped for lookahead EOI tokens.
  EXPECT_THAT(token_provider.PeekLookahead2(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::EOI,
                  .location = MakeLocation(2, 2),
                  .text = "",
                  .preceding_whitespaces = "",
              }));
  EXPECT_THAT(token_provider.PeekLookahead3(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::EOI,
                  .location = MakeLocation(2, 2),
                  .text = "",
                  .preceding_whitespaces = "",
              }));

  EXPECT_THAT(token_provider.GetNextToken(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::EOI,
                  .location = MakeLocation(2, 2),
                  .text = "",
                  .preceding_whitespaces = "\t\t",
              }));
  // No preceding whitespaces for the second EOF.
  EXPECT_THAT(token_provider.GetNextToken(),
              IsOkAndHoldsToken(TokenWithLocation{
                  .kind = Token::EOI,
                  .location = MakeLocation(2, 2),
                  .text = "",
                  .preceding_whitespaces = "",
              }));
}

TEST(TokenProviderTest, CanPeekToken) {
  absl::string_view input = "\t123 identifier";
  TokenProvider token_provider = MakeTokenProvider(input);
  const TokenWithLocation int_token{
      .kind = Token::DECIMAL_INTEGER_LITERAL,
      .location = MakeLocation(1, 4),
      .text = "123",
      .preceding_whitespaces = "\t",
  };
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(int_token));
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(int_token));
  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(int_token));

  const TokenWithLocation identifier_token{
      .kind = Token::IDENTIFIER,
      .location = MakeLocation(5, 15),
      .text = "identifier",
      .preceding_whitespaces = " ",
  };
  EXPECT_THAT(token_provider.PeekNextToken(),
              IsOkAndHoldsToken(identifier_token));
  EXPECT_THAT(token_provider.PeekNextToken(),
              IsOkAndHoldsToken(identifier_token));
  EXPECT_THAT(token_provider.GetNextToken(),
              IsOkAndHoldsToken(identifier_token));
}

TEST(TokenProviderTest, CanPeekNthToken) {
  absl::string_view input = "1 2 3";
  TokenProvider token_provider = MakeTokenProvider(input);

  const TokenWithLocation token1 = MakeIntToken("1", 0, 1);
  const TokenWithLocation token2 = MakeIntToken("2", 2, 3, " ");
  const TokenWithLocation token3 = MakeIntToken("3", 4, 5, " ");

  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token1));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(token2));
  EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(token3));

  // Peek again to make sure tokens are not consumed.
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token1));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(token2));
  EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(token3));

  // Consume tokens and verify peeking still works.
  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(token1));
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token2));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(token3));
}

TEST(TokenProviderTest, PeekPastEndOfInput) {
  absl::string_view input = "1";
  TokenProvider token_provider = MakeTokenProvider(input);

  const TokenWithLocation token1 = MakeIntToken("1", 0, 1);
  const TokenWithLocation eoiToken = MakeEoiToken(1);

  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token1));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(eoiToken));
  // Peeking past EOI should still return EOI.
  EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(eoiToken));
}

TEST(TokenProviderTest, RepeatedPeekOnAllBuffers) {
  absl::string_view input = "10 20 30";
  TokenProvider token_provider = MakeTokenProvider(input);

  const TokenWithLocation token1 = MakeIntToken("10", 0, 2);
  const TokenWithLocation token2 = MakeIntToken("20", 3, 5, " ");
  const TokenWithLocation token3 = MakeIntToken("30", 6, 8, " ");

  for (int i = 0; i < 5; ++i) {
    EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token1));
    EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(token2));
    EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(token3));
  }
}

TEST(TokenProviderTest, SequentialConsumesShiftBuffers) {
  absl::string_view input = "100 200";
  TokenProvider token_provider = MakeTokenProvider(input);

  const TokenWithLocation token1 = MakeIntToken("100", 0, 3);
  const TokenWithLocation token2 = MakeIntToken("200", 4, 7, " ");
  const TokenWithLocation eoi_token = MakeEoiToken(7);

  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token1));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(token2));
  EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(eoi_token));

  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(token1));
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token2));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(eoi_token));
  EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(eoi_token));

  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(token2));
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(eoi_token));
  EXPECT_THAT(token_provider.PeekLookahead2(), IsOkAndHoldsToken(eoi_token));
  EXPECT_THAT(token_provider.PeekLookahead3(), IsOkAndHoldsToken(eoi_token));
}

TEST(TokenProviderTest, TracksCountOfConsumedTokensIncludingEOF) {
  absl::string_view input = "SELECT";
  TokenProvider token_provider = MakeTokenProvider(input);

  TokenWithLocation first_token{
      .kind = Token::KW_SELECT,
      .location = MakeLocation(0, 6),
      .text = "SELECT",
      .preceding_whitespaces = "",
  };

  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(first_token));
  EXPECT_EQ(token_provider.num_consumed_tokens(), 0);

  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(first_token));
  EXPECT_EQ(token_provider.num_consumed_tokens(), 1);

  TokenWithLocation second_token{
      .kind = Token::EOI,
      .location = MakeLocation(6, 6),
      .text = "",
      .preceding_whitespaces = "",
  };

  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(second_token));
  EXPECT_EQ(token_provider.num_consumed_tokens(), 1);

  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(second_token));
  EXPECT_EQ(token_provider.num_consumed_tokens(), 2);
}

TEST(TokenProviderTest,
     ErrorAtStartOfInputPropagatesAcrossLookaheadsAndConsumes) {
  absl::string_view input = "'unterminated string";
  TokenProvider token_provider = MakeTokenProvider(input);

  absl::StatusOr<TokenWithLocation> peek_1 = token_provider.PeekNextToken();
  absl::StatusOr<TokenWithLocation> peek_2 = token_provider.PeekLookahead2();
  absl::StatusOr<TokenWithLocation> peek_3 = token_provider.PeekLookahead3();

  EXPECT_FALSE(peek_1.ok());
  EXPECT_EQ(peek_1.status(), peek_2.status());
  EXPECT_EQ(peek_1.status(), peek_3.status());

  absl::StatusOr<TokenWithLocation> get_1 = token_provider.GetNextToken();
  absl::StatusOr<TokenWithLocation> get_2 = token_provider.GetNextToken();
  absl::StatusOr<TokenWithLocation> get_3 = token_provider.GetNextToken();

  EXPECT_EQ(peek_1.status(), get_1.status());
  EXPECT_EQ(peek_1.status(), get_2.status());
  EXPECT_EQ(peek_1.status(), get_3.status());
}

TEST(TokenProviderTest, ErrorAfterValidTokenPropagatesToLookaheadsAndConsumes) {
  absl::string_view input = "100 'unterminated string";
  TokenProvider token_provider = MakeTokenProvider(input);

  const TokenWithLocation token1 = MakeIntToken("100", 0, 3);
  EXPECT_THAT(token_provider.PeekNextToken(), IsOkAndHoldsToken(token1));

  absl::StatusOr<TokenWithLocation> peek_2 = token_provider.PeekLookahead2();
  absl::StatusOr<TokenWithLocation> peek_3 = token_provider.PeekLookahead3();

  EXPECT_FALSE(peek_2.ok());
  EXPECT_EQ(peek_2.status(), peek_3.status());

  // Consume valid token.
  EXPECT_THAT(token_provider.GetNextToken(), IsOkAndHoldsToken(token1));

  // Now Lookahead 1, 2, 3 should all hold the error status.
  EXPECT_EQ(token_provider.PeekNextToken().status(), peek_2.status());
  EXPECT_EQ(token_provider.PeekLookahead2().status(), peek_2.status());
  EXPECT_EQ(token_provider.PeekLookahead3().status(), peek_2.status());

  // Subsequent consumes should repeatedly return the exact same error status.
  EXPECT_EQ(token_provider.GetNextToken().status(), peek_2.status());
  EXPECT_EQ(token_provider.GetNextToken().status(), peek_2.status());
}

}  // namespace macros
}  // namespace parser
}  // namespace googlesql
