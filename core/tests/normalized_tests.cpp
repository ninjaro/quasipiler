#include "normalized.hpp"
#include "operators.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <sstream>

namespace {
normalized::expression_ptr
normalize(const std::string& text, normalized::limits budget = {}) {
    const auto input = source::from_memory(text, "expression.qc");
    return normalized::normalize_expression(
        source_span(input, 0, input->size()), budget
    );
}

std::string shape(const normalized::expression_ptr& expression) {
    std::ostringstream out;
    normalized::dump(out, expression);
    auto text = out.str();
    text.pop_back();
    return text;
}

void check_edges(const normalized::expression_ptr& parent) {
    ASSERT_NE(parent, nullptr);
    size_t height = 1;
    normalized::for_each_child(*parent, [&](const auto& child) {
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(parent->span.owner(), child->span.owner());
        EXPECT_LE(parent->span.begin().offset, child->span.begin().offset);
        EXPECT_GE(parent->span.end().offset, child->span.end().offset);
        height = std::max(height, child->height + 1);
        check_edges(child);
    });
    EXPECT_EQ(parent->height, height);
}

struct expression_case {
    std::string text, expected;
};

class NormalizedShape : public testing::TestWithParam<expression_case> { };
}

TEST_P(NormalizedShape, HasExplicitOperandsAndStableDump) {
    const auto expression = normalize(GetParam().text);
    EXPECT_EQ(shape(expression), GetParam().expected);
    EXPECT_EQ(shape(expression), shape(normalize(GetParam().text)));
    check_edges(expression);
}

INSTANTIATE_TEST_SUITE_P(
    Expressions, NormalizedShape,
    testing::Values(
        expression_case { "36.6", "(decimal \"36.6\")" },
        expression_case { "true", "(boolean \"true\")" },
        expression_case { "null", "(null \"null\")" },
        expression_case { "x", "(id \"x\")" },
        expression_case { "a+b*c", "(+ (id \"a\") (* (id \"b\") (id \"c\")))" },
        expression_case { "a-b-c", "(- (- (id \"a\") (id \"b\")) (id \"c\"))" },
        expression_case { "a=b=c", "(= (id \"a\") (= (id \"b\") (id \"c\")))" },
        expression_case {
            "!a&&b||c",
            "(|| (&& (prefix ! (id \"a\")) (id \"b\")) (id \"c\"))" },
        expression_case { "a?b:c?d:e",
                          "(?: (id \"a\") (id \"b\") (?: (id \"c\") (id \"d\") "
                          "(id \"e\")))" },
        expression_case { "a?b?c:d:e",
                          "(?: (id \"a\") (?: (id \"b\") (id \"c\") (id "
                          "\"d\")) (id \"e\"))" },
        expression_case { "++a--", "(prefix ++ (postfix -- (id \"a\")))" },
        expression_case { "-f(x).a[i]++",
                          "(prefix - (postfix ++ (index (member (call (id "
                          "\"f\") (id \"x\")) \"a\") (id \"i\"))))" },
        expression_case { "obj.a[foo(1)].b",
                          "(member (index (member (id \"obj\") \"a\") (call "
                          "(id \"foo\") (integer \"1\"))) \"b\")" },
        expression_case { "a.b[c](d).e",
                          "(member (call (index (member (id \"a\") \"b\") (id "
                          "\"c\")) (id \"d\")) \"e\")" },
        expression_case {
            "f(1)(2)",
            "(call (call (id \"f\") (integer \"1\")) (integer \"2\"))" },
        expression_case { "[1, x+2, f()]",
                          "(list (integer \"1\") (+ (id \"x\") (integer "
                          "\"2\")) (call (id \"f\")))" },
        expression_case { "{a:1,b:x+2}",
                          "(object (\"a\" (integer \"1\")) (\"b\" (+ (id "
                          "\"x\") (integer \"2\"))))" },
        expression_case { "[]", "(list)" },
        expression_case { "{}", "(object)" },
        expression_case { "()", "(tuple)" },
        expression_case { "(1)", "(integer \"1\")" },
        expression_case { "(1,)", "(tuple (integer \"1\"))" },
        expression_case {
            "a[(1,2)]",
            "(index (id \"a\") (tuple (integer \"1\") (integer \"2\")))" },
        expression_case { "obj.(a,'b').c",
                          "(member (members (id \"obj\") \"a\" \"b\") \"c\")" },
        expression_case {
            "obj{'a', 'b'}",
            "(select (id \"obj\") (string \"a\") (string \"b\"))" },
        expression_case { "a[:end:step]",
                          "(slice (id \"a\") _ (id \"end\") (id \"step\"))" },
        expression_case { "a[b?c:d:e]",
                          "(slice (id \"a\") (?: (id \"b\") (id \"c\") (id "
                          "\"d\")) (id \"e\") _)" },
        expression_case { "a + + b", "(+ (id \"a\") (prefix + (id \"b\")))" },
        expression_case { "a+/* separate */+b",
                          "(+ (id \"a\") (prefix + (id \"b\")))" },
        expression_case { "[1,]", "(list (integer \"1\"))" },
        expression_case { "{a:1,}", "(object (\"a\" (integer \"1\")))" },
        expression_case { "f(1,)", "(call (id \"f\") (integer \"1\"))" }
    )
);

class InvalidNormalizedExpression : public testing::TestWithParam<std::string> {
};

TEST_P(InvalidNormalizedExpression, RejectsRatherThanReturningPrefix) {
    EXPECT_THROW(static_cast<void>(normalize(GetParam())), std::runtime_error);
}

INSTANTIATE_TEST_SUITE_P(
    Expressions, InvalidNormalizedExpression,
    testing::Values(
        "", "a b", "a+", "a + * b", "a;", "f(,)", "f(1 2)", "f(1", "a.", "a.1",
        "a[]", "a[1,2]", "a[:::]", "a[1:2:3:4]", "a[1)", "[1,,2]", "{a}",
        "{a:}", "{1:2}", "{a:1;b:2}", "{a:1 b:2}", "a?b", "a?:b",
        "a?b:", "a?b:c:d", "return x", "a & & b", "a.(a+1)", "a.()", "a{}"
    )
);

TEST(NormalizedExpression, PreservesEveryLiteralSpellingAndDecodedStrings) {
    for (const std::string text :
         { "99999999999999999999999999999999999999", "36.6", "1.200E-30",
           "'line\\nvalue'", "false", "null" }) {
        const auto expression = normalize(text);
        const auto* literal
            = std::get_if<normalized::literal>(&expression->value);
        ASSERT_NE(literal, nullptr);
        EXPECT_EQ(literal->token.span.text(), text);
        if (text.front() == '\'')
            EXPECT_EQ(literal->token.text, "line\nvalue");
        else
            EXPECT_EQ(literal->token.text, text);
    }
    EXPECT_EQ(shape(normalize("'+'")), "(string \"+\")");
    EXPECT_EQ(shape(normalize("{'}':':'}")), "(object (\"}\" (string \":\")))");
}

TEST(NormalizedExpression, EveryOperatorRetainsSharedPrecedence) {
    for (const auto& op : binary_operators) {
        SCOPED_TRACE(op.spelling);
        const auto expression = normalize(
            "a" + std::string(op.spelling) + "b" + std::string(op.spelling)
            + "c"
        );
        const auto* binary
            = std::get_if<normalized::binary>(&expression->value);
        ASSERT_NE(binary, nullptr);
        EXPECT_EQ(binary->op.text, op.spelling);
        const auto nested = op.right_associative ? binary->right : binary->left;
        EXPECT_NE(std::get_if<normalized::binary>(&nested->value), nullptr);
    }
    // Every adjacent precedence tier, independent of the metadata table.
    const std::vector<std::string> order
        = { "||", "&&", "|", "^", "&", "==", "<", "<<", "+", "*" };
    for (size_t i = 0; i + 1 < order.size(); ++i) {
        const auto expression
            = normalize("a" + order[i] + "b" + order[i + 1] + "c");
        const auto& binary = std::get<normalized::binary>(expression->value);
        EXPECT_EQ(binary.op.text, order[i]);
        EXPECT_EQ(
            std::get<normalized::binary>(binary.right->value).op.text,
            order[i + 1]
        );
    }
}

TEST(NormalizedExpression, SliceOmissionsAndDelimiterLocationsAreExplicit) {
    for (const std::string text : { "a[:b]", "a[b:]", "a[::]", "a[b::]",
                                    "a[:b:]", "a[::b]", "a[b:c:d]" }) {
        SCOPED_TRACE(text);
        const auto expression = normalize(text);
        const auto& slice = std::get<normalized::slice>(expression->value);
        EXPECT_EQ(slice.open.span.text(), "[");
        EXPECT_EQ(slice.close.span.text(), "]");
        EXPECT_EQ(static_cast<bool>(slice.start), text[2] != ':');
        for (const auto& colon : slice.colons)
            EXPECT_EQ(colon.span.text(), ":");
        EXPECT_EQ(
            slice.colons.size(),
            static_cast<size_t>(std::count(text.begin(), text.end(), ':'))
        );
        check_edges(expression);
    }
}

TEST(NormalizedExpression, BoundedInputRetainsAbsoluteOperatorPositions) {
    const auto source
        = source::from_memory("ignored\n  a.b[c](d).e suffix", "positions.qc");
    const auto expression
        = normalized::normalize_expression(source_span(source, 10, 21));
    const auto& member = std::get<normalized::member>(expression->value);
    EXPECT_EQ(expression->span.text(), "a.b[c](d).e");
    EXPECT_EQ(member.dot.span.begin(), (position { 19, 1, 11 }));
    EXPECT_EQ(member.name.span.begin(), (position { 20, 1, 12 }));
    const auto& call = std::get<normalized::call>(member.base->value);
    EXPECT_EQ(call.open.span.text(), "(");
    EXPECT_EQ(call.close.span.text(), ")");
    check_edges(expression);
}

TEST(NormalizedExpression, TernaryColonsDoNotBorrowOperandLocations) {
    const auto expression = normalize("a ? b : c");
    const auto& ternary = std::get<normalized::ternary>(expression->value);
    EXPECT_EQ(ternary.question.span.begin().offset, 2);
    EXPECT_EQ(ternary.colon.span.begin().offset, 6);
}

TEST(NormalizedExpression, ParallelNormalizationRetainsSourceLifetime) {
    const auto input = source::from_memory("obj.a[f(1)].b");
    const source_span span(input, 0, input->size());
    std::vector<std::future<normalized::expression_ptr>> jobs;
    for (int i = 0; i < 8; ++i)
        jobs.push_back(std::async(std::launch::async, [span] {
            return normalized::normalize_expression(span);
        }));
    const auto expected = shape(normalized::normalize_expression(span));
    for (auto& job : jobs)
        EXPECT_EQ(shape(job.get()), expected);
    const auto surviving = normalize("f(1)(2)");
    EXPECT_EQ(surviving->span.text(), "f(1)(2)");
}

TEST(NormalizedExpression, LimitsCoverNestingAndFlatExpressionGrowth) {
    EXPECT_NO_THROW(static_cast<void>(normalize("a", { 1, 1 })));
    EXPECT_THROW(
        static_cast<void>(normalize("a", { 0, 1 })), std::runtime_error
    );
    EXPECT_THROW(
        static_cast<void>(normalize("a+b", { 32, 2 })), std::runtime_error
    );
    std::string prefix(40, '!');
    EXPECT_THROW(
        static_cast<void>(normalize(prefix + "a", { 32, 1000 })),
        std::runtime_error
    );
    std::string chain = "a", binary = "a";
    for (int i = 0; i < 40; ++i) {
        chain += ".a";
        binary += "+a";
    }
    EXPECT_THROW(
        static_cast<void>(normalize(chain, { 32, 1000 })), std::runtime_error
    );
    EXPECT_THROW(
        static_cast<void>(normalize(binary, { 32, 1000 })), std::runtime_error
    );
    EXPECT_NO_THROW(static_cast<void>(normalize(chain, { 64, 1000 })));
}

TEST(NormalizedExpression, ErrorsAreLocatedAndCannotReadBeyondSpan) {
    const auto input = source::from_memory("prefix\n  f(x)", "bad.qc");
    try {
        static_cast<void>(
            normalized::normalize_expression(source_span(input, 9, 12))
        );
        FAIL() << "missing close accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("bad.qc:2:6"), std::string::npos
        );
    }
}
