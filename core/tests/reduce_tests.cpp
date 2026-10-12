#include "reduce.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <sstream>
#include <stdexcept>

namespace {
normalized::expression_ptr parse(const std::string& text) {
    const auto input = source::from_memory(text, "reduce.qc");
    return normalized::normalize_expression(
        source_span(input, 0, input->size())
    );
}

std::string shape(const normalized::expression_ptr& tree) {
    std::ostringstream out;
    normalized::dump(out, tree);
    auto value = out.str();
    value.pop_back();
    return value;
}

exact::number number(const std::string& spelling) {
    return *exact::parse(spelling).value;
}

void invariants(const normalized::expression_ptr& tree) {
    size_t height = 1;
    normalized::for_each_child(
        *tree,
        [&](const auto& child) {
            EXPECT_EQ(tree->span.owner(), child->span.owner());
            EXPECT_LE(tree->span.begin().offset, child->span.begin().offset);
            EXPECT_GE(tree->span.end().offset, child->span.end().offset);
            height = std::max(height, child->height + 1);
            invariants(child);
        },
        [&](const auto& child) { height = std::max(height, child->height + 1); }
    );
    EXPECT_EQ(tree->height, height);
}

struct fold_case {
    std::string text, expected;
};

class ScalarReduction : public testing::TestWithParam<fold_case> { };
}

TEST_P(ScalarReduction, FoldsExactlyAndReachesPointerFixedPoint) {
    const auto original = parse(GetParam().text);
    const auto result = reduction::reduce(original);
    EXPECT_EQ(result.kind, reduction::state::known);
    ASSERT_NE(result.value(), nullptr);
    EXPECT_EQ(result.why, 0U);
    EXPECT_EQ(shape(result.tree), GetParam().expected);
    invariants(result.tree);
    const auto again = reduction::reduce(result.tree);
    EXPECT_EQ(again.tree, result.tree);
    EXPECT_EQ(again.kind, result.kind);
    EXPECT_EQ(again.why, result.why);
}

INSTANTIATE_TEST_SUITE_P(
    Constants, ScalarReduction,
    testing::Values(
        fold_case { "2+3", "(exact \"5\")" },
        fold_case { "36.6*1.2", "(exact \"1098/25\")" },
        fold_case { "9007199254740993+1", "(exact \"9007199254740994\")" },
        fold_case { "1/3+1/6", "(exact \"1/2\")" },
        fold_case { "-(1/3)", "(exact \"-1/3\")" },
        fold_case { "+2", "(exact \"2\")" },
        fold_case { "2-5", "(exact \"-3\")" },
        fold_case { "-0.0", "(exact \"0\")" },
        fold_case { "1.25e2/5", "(exact \"25\")" },
        fold_case { "2<3", "(boolean \"true\")" },
        fold_case { "2<=2", "(boolean \"true\")" },
        fold_case { "2>3", "(boolean \"false\")" },
        fold_case { "-2>=-3", "(boolean \"true\")" },
        fold_case { "0.5==1/2", "(boolean \"true\")" },
        fold_case { "0.5!=1/2", "(boolean \"false\")" },
        fold_case { "true==false", "(boolean \"false\")" },
        fold_case { "'a'=='a'", "(boolean \"true\")" },
        fold_case { "'a'!='b'", "(boolean \"true\")" },
        fold_case { "null==null", "(boolean \"true\")" },
        fold_case { "!true", "(boolean \"false\")" },
        fold_case { "true&&false", "(boolean \"false\")" },
        fold_case { "false||true", "(boolean \"true\")" },
        fold_case { "true?2+3:1/0", "(exact \"5\")" },
        fold_case { "false?f():3", "(exact \"3\")" },
        fold_case { "false&&f()", "(boolean \"false\")" },
        fold_case { "true||x++", "(boolean \"true\")" },
        fold_case { "true?'a\\n':null", "(string \"a\\n\")" }
    )
);

struct residual_case {
    std::string text;
    reduction::reason reason;
};

class ResidualReduction : public testing::TestWithParam<residual_case> { };

TEST_P(ResidualReduction, PreservesOperationsAndStableReasons) {
    const auto first = reduction::reduce(parse(GetParam().text));
    EXPECT_EQ(first.kind, reduction::state::residual);
    EXPECT_EQ(first.value(), nullptr);
    EXPECT_NE(first.why & reduction::flag(GetParam().reason), 0U);
    invariants(first.tree);
    const auto again = reduction::reduce(first.tree);
    EXPECT_EQ(again.tree, first.tree);
    EXPECT_EQ(again.kind, first.kind);
    EXPECT_EQ(again.why, first.why);
}

INSTANTIATE_TEST_SUITE_P(
    Conservative, ResidualReduction,
    testing::Values(
        residual_case { "x+0", reduction::reason::unknown_symbol },
        residual_case { "x*0", reduction::reason::unknown_symbol },
        residual_case { "x/x", reduction::reason::unknown_symbol },
        residual_case { "f()*0", reduction::reason::side_effect },
        residual_case { "x++", reduction::reason::side_effect },
        residual_case { "--x", reduction::reason::side_effect },
        residual_case { "x=2+3", reduction::reason::side_effect },
        residual_case { "a[f()]+=2+3", reduction::reason::side_effect },
        residual_case { "1/0", reduction::reason::division_by_zero },
        residual_case { "0/0", reduction::reason::division_by_zero },
        residual_case { "true+1", reduction::reason::incompatible_types },
        residual_case { "'a'+'b'", reduction::reason::incompatible_types },
        residual_case { "null==false", reduction::reason::incompatible_types },
        residual_case { "'a'<'b'", reduction::reason::incompatible_types },
        residual_case { "!1", reduction::reason::incompatible_types },
        residual_case { "-true", reduction::reason::incompatible_types },
        residual_case { "+null", reduction::reason::incompatible_types },
        residual_case { "1?2:3", reduction::reason::incompatible_types },
        residual_case { "true&&2", reduction::reason::incompatible_types },
        residual_case { "0&&false", reduction::reason::incompatible_types },
        residual_case { "1%2", reduction::reason::unsupported_operation },
        residual_case { "1<<2", reduction::reason::unsupported_operation },
        residual_case { "1&2", reduction::reason::unsupported_operation },
        residual_case { "~1", reduction::reason::unsupported_operation },
        residual_case { "[1+2,x]", reduction::reason::unsupported_operation },
        residual_case { "(1+2,x)", reduction::reason::unsupported_operation },
        residual_case { "{a:1+2,a:x}",
                        reduction::reason::unsupported_operation },
        residual_case { "a.b[1+2]", reduction::reason::unsupported_operation },
        residual_case { "a[1+2:4+5:2]",
                        reduction::reason::unsupported_operation },
        residual_case { "a.(x,y){1+2}",
                        reduction::reason::unsupported_operation },
        residual_case { "x?f():g()", reduction::reason::unknown_symbol }
    )
);

TEST(Reducer, DistinguishesKnownInputAndUnresolvedBindings) {
    const reduction::environment environment {
        { "answer", normalized::scalar(number("42")) },
        { "x",
          reduction::input_binding { "row.x",
                                     reduction::input_type::exact_number } },
        { "flag",
          reduction::input_binding { "row.flag",
                                     reduction::input_type::boolean } }
    };
    const auto known = reduction::reduce(parse("answer+1"), environment);
    EXPECT_EQ(shape(known.tree), "(exact \"43\")");
    const auto input = reduction::reduce(parse("x"), environment);
    ASSERT_TRUE(input.input);
    EXPECT_EQ(input.kind, reduction::state::input);
    EXPECT_EQ(input.input->identity, "row.x");
    EXPECT_EQ(
        reduction::reduce(parse("missing"), environment).kind,
        reduction::state::unresolved
    );
    const auto residual
        = reduction::reduce(parse("x+missing+answer"), environment);
    EXPECT_EQ(
        residual.why,
        reduction::flag(reduction::reason::input_dependency)
            | reduction::flag(reduction::reason::unknown_symbol)
    );
    EXPECT_EQ(
        shape(residual.tree),
        "(+ (+ (id \"x\") (id \"missing\")) (exact \"42\"))"
    );
    EXPECT_EQ(
        reduction::reduce(parse("true&&flag"), environment).kind,
        reduction::state::input
    );
    EXPECT_EQ(
        reduction::reduce(parse("false||flag"), environment).kind,
        reduction::state::input
    );
    EXPECT_EQ(
        reduction::reduce(parse("true?x:missing"), environment).why,
        reduction::flag(reduction::reason::input_dependency)
    );
}

TEST(Reducer, IdentitiesRequireExplicitExactNumericFacts) {
    const reduction::environment environment {
        { "x",
          reduction::input_binding { "row.x",
                                     reduction::input_type::exact_number } }
    };
    for (const auto* text :
         { "x+0", "0+x", "x-0", "x*1", "1*x", "x/1", "+x" }) {
        const auto result = reduction::reduce(parse(text), environment);
        EXPECT_EQ(result.kind, reduction::state::input) << text;
        EXPECT_EQ(shape(result.tree), "(id \"x\")") << text;
        EXPECT_EQ(
            reduction::reduce(result.tree, environment).tree, result.tree
        );
    }
    for (const auto type :
         { reduction::input_type::unknown, reduction::input_type::boolean,
           reduction::input_type::string }) {
        const reduction::environment other {
            { "x", reduction::input_binding { "row.x", type } }
        };
        EXPECT_EQ(
            reduction::reduce(parse("x+0"), other).kind,
            reduction::state::residual
        );
    }
    EXPECT_EQ(
        reduction::reduce(parse("x*0"), environment).kind,
        reduction::state::residual
    );
    EXPECT_EQ(
        reduction::reduce(parse("x==x"), environment).kind,
        reduction::state::residual
    );
}

TEST(Reducer, PreservesLvaluesEvaluationOrderAndUnchangedSubtrees) {
    const reduction::environment environment {
        { "x", normalized::scalar(number("10")) }
    };
    const auto assignment = parse("x=2+3");
    const auto reduced = reduction::reduce(assignment, environment);
    EXPECT_EQ(shape(reduced.tree), "(= (id \"x\") (exact \"5\"))");
    EXPECT_EQ(
        std::get<normalized::binary>(assignment->value).left,
        std::get<normalized::binary>(reduced.tree->value).left
    );
    const auto increment = parse("x++");
    EXPECT_EQ(reduction::reduce(increment, environment).tree, increment);
    const auto call = reduction::reduce(parse("f(g(),x++,2+3)"));
    EXPECT_EQ(
        shape(call.tree),
        "(call (id \"f\") (call (id \"g\")) (postfix ++ (id \"x\")) (exact "
        "\"5\"))"
    );
    const auto partial = parse("x+(2+3)");
    const auto folded = reduction::reduce(partial);
    EXPECT_EQ(
        std::get<normalized::binary>(partial->value).left,
        std::get<normalized::binary>(folded.tree->value).left
    );
    EXPECT_EQ(
        shape(partial), "(+ (id \"x\") (+ (integer \"2\") (integer \"3\")))"
    );
    invariants(folded.tree);
}

TEST(Reducer, ShortCircuitSkipsEvenOversizedOrEffectfulBranches) {
    reduction::limits budget;
    budget.steps = 2;
    budget.numbers.bits = 8;
    EXPECT_EQ(
        reduction::reduce(parse("false&&(1e999999999999+f())"), {}, budget)
            .kind,
        reduction::state::known
    );
    budget.steps = 3;
    const auto selected
        = reduction::reduce(parse("true?x:(1/0+f())"), {}, budget);
    EXPECT_EQ(selected.kind, reduction::state::unresolved);
    EXPECT_EQ(selected.why, reduction::flag(reduction::reason::unknown_symbol));
    const auto both = reduction::reduce(parse("x?1/0:f()"));
    EXPECT_NE(
        both.why & reduction::flag(reduction::reason::division_by_zero), 0U
    );
    EXPECT_NE(both.why & reduction::flag(reduction::reason::side_effect), 0U);
}

TEST(Reducer, StructuralExhaustionRollsBackWithoutProgressOnRepeatedPasses) {
    const auto tree = parse("(1+2)+(3+4)");
    for (const size_t steps : { 0U, 1U, 4U, 6U }) {
        reduction::limits budget;
        budget.steps = steps;
        const auto reduced = reduction::reduce(tree, {}, budget);
        EXPECT_EQ(reduced.tree, tree);
        EXPECT_EQ(
            reduced.why, reduction::flag(reduction::reason::expression_growth)
        );
        const auto again = reduction::reduce(reduced.tree, {}, budget);
        EXPECT_EQ(again.tree, tree);
        EXPECT_EQ(again.why, reduced.why);
    }
    reduction::limits budget;
    budget.steps = 7;
    EXPECT_EQ(
        shape(reduction::reduce(tree, {}, budget).tree), "(exact \"10\")"
    );
    budget.depth = 2;
    EXPECT_EQ(reduction::reduce(tree, {}, budget).tree, tree);
    budget.depth = 3;
    EXPECT_EQ(
        reduction::reduce(tree, {}, budget).kind, reduction::state::known
    );
}

TEST(Reducer, NumericGrowthIsResidualAndLargerBudgetsCanResume) {
    reduction::limits budget;
    budget.numbers.bits = 8;
    const auto first = reduction::reduce(parse("128*2+1"), {}, budget);
    EXPECT_EQ(
        shape(first.tree), "(+ (* (exact \"128\") (exact \"2\")) (exact \"1\"))"
    );
    EXPECT_EQ(first.why, reduction::flag(reduction::reason::numeric_growth));
    EXPECT_EQ(reduction::reduce(first.tree, {}, budget).tree, first.tree);
    EXPECT_EQ(shape(reduction::reduce(first.tree).tree), "(exact \"257\")");
    const auto huge
        = reduction::reduce(parse("1e999999999999999999999999"), {}, budget);
    EXPECT_EQ(huge.why, reduction::flag(reduction::reason::numeric_growth));
    const reduction::environment environment {
        { "x", normalized::scalar(number("256")) }
    };
    EXPECT_EQ(
        reduction::reduce(parse("x"), environment, budget).why,
        reduction::flag(reduction::reason::numeric_growth)
    );
    EXPECT_EQ(
        reduction::reduce(reduction::reduce(parse("256")).tree, {}, budget).why,
        reduction::flag(reduction::reason::numeric_growth)
    );
}

TEST(Reducer, DeepInputsAndConcurrentReductionsRetainSourceOwnership) {
    std::string text = "1";
    for (int i = 0; i < 150; ++i)
        text += "+1";
    const auto tree = parse(text);
    auto one = std::async(std::launch::async, [&] {
        return reduction::reduce(tree);
    });
    auto two = std::async(std::launch::async, [&] {
        return reduction::reduce(tree);
    });
    const auto first = one.get(), second = two.get();
    EXPECT_EQ(shape(first.tree), "(exact \"151\")");
    EXPECT_EQ(shape(first.tree), shape(second.tree));
    EXPECT_EQ(first.tree->span.text(), text);
    reduction::limits budget;
    budget.depth = 100;
    EXPECT_EQ(reduction::reduce(tree, {}, budget).tree, tree);
}

TEST(Reducer, DumpsReasonsDeterministicallyAndEscapesInputIdentity) {
    const reduction::environment environment {
        { "x", reduction::input_binding { "row.\"x\"\n" } }
    };
    std::ostringstream out;
    reduction::dump(out, reduction::reduce(parse("x"), environment));
    EXPECT_EQ(
        out.str(), "input \"row.\\\"x\\\"\\n\" [input_dependency] (id \"x\")\n"
    );
    std::ostringstream residual;
    reduction::dump(
        residual, reduction::reduce(parse("x+y+f()+1/0"), environment)
    );
    EXPECT_NE(
        residual.str().find(
            "[unknown_symbol,input_dependency,unsupported_operation,side_"
            "effect,division_by_zero]"
        ),
        std::string::npos
    );
}

TEST(Reducer, FunctionBodiesRemainDeferredAcrossBindingScopes) {
    const auto function = parse("fu(x){return x+1/0;}");
    const reduction::environment environment {
        { "x", normalized::scalar(number("42")) }
    };
    const auto result = reduction::reduce(function, environment);
    EXPECT_EQ(result.tree, function);
    EXPECT_EQ(
        result.why, reduction::flag(reduction::reason::unsupported_operation)
    );
    EXPECT_EQ(
        shape(result.tree),
        "(function (parameters \"x\") (block (return (+ (id \"x\") (/ (integer "
        "\"1\") (integer \"0\"))))))"
    );
}

TEST(Reducer, KnownScalarBindingsPreserveTheirTypes) {
    const reduction::environment environment {
        { "s", normalized::scalar(std::string("value")) },
        { "b", normalized::scalar(true) },
        { "n", normalized::scalar(std::monostate {}) }
    };
    EXPECT_EQ(
        shape(reduction::reduce(parse("b?s:n"), environment).tree),
        "(string \"value\")"
    );
    EXPECT_EQ(
        shape(reduction::reduce(parse("n==null"), environment).tree),
        "(boolean \"true\")"
    );
}

TEST(Reducer, WideCollectionsRespectVisitBudgetBeforeCopyingChildren) {
    const auto tree = parse("[1+2,3+4,5+6,7+8]");
    reduction::limits budget;
    budget.steps = 3;
    const auto result = reduction::reduce(tree, {}, budget);
    EXPECT_EQ(result.tree, tree);
    EXPECT_EQ(
        result.why, reduction::flag(reduction::reason::expression_growth)
    );
}

TEST(Reducer, ValidatesApiLimitsAndNullExpressions) {
    EXPECT_THROW(reduction::reduce(nullptr), std::invalid_argument);
    reduction::limits budget;
    budget.depth = 513;
    EXPECT_THROW(
        reduction::reduce(parse("1"), {}, budget), std::invalid_argument
    );
    budget.depth = 256;
    budget.steps = 1000001;
    EXPECT_THROW(
        reduction::reduce(parse("1"), {}, budget), std::invalid_argument
    );
}
