#include "evaluate.hpp"
#include "grouper.hpp"
#include "reduce.hpp"
#include <gtest/gtest.h>

#include <sstream>

namespace {
normalized::expression_ptr parse(const std::string& text) {
    const auto input = source::from_memory(text, "integration.qc");
    return normalized::normalize_expression(
        source_span(input, 0, input->size())
    );
}

std::string dump(const normalized::statement_ptr& tree) {
    std::ostringstream stream;
    normalized::dump(stream, tree);
    return stream.str();
}

runtime::error execute_error(const normalized::expression_ptr& tree) {
    try {
        runtime::evaluate(tree);
        ADD_FAILURE() << "expected runtime error";
    } catch (const runtime::error& error) {
        return error;
    }
    throw std::runtime_error("test expression did not fail");
}
}

TEST(RuntimeIntegration, ReportCollectionDoesNotChangeTreesOrExecution) {
    const auto input = source::from_memory("return max([36.6*1.2, 5]);");
    const auto program
        = normalized::normalize_program(source_span(input, 0, input->size()));
    const auto quiet
        = reduction::reduce_program(program, {}, reduction::reporting::none);
    const auto verbose = reduction::reduce_program(
        program, {}, reduction::reporting::expressions
    );
    EXPECT_TRUE(quiet.expressions.empty());
    ASSERT_EQ(verbose.expressions.size(), 1U);
    EXPECT_EQ(quiet.why, verbose.why);
    EXPECT_EQ(dump(quiet.tree), dump(verbose.tree));
    const auto expression = [](const auto& tree) {
        const auto& statements
            = std::get<normalized::block>(tree->value).statements;
        return std::get<normalized::jump>(statements.front()->value).value;
    };
    EXPECT_EQ(
        runtime::evaluate(expression(quiet.tree)).output.as_number().str(),
        "1098/25"
    );
    EXPECT_EQ(
        runtime::evaluate(expression(verbose.tree)).output.as_number().str(),
        "1098/25"
    );
    EXPECT_EQ(
        runtime::evaluate(expression(program)).output.as_number().str(),
        "1098/25"
    );
}

TEST(RuntimeIntegration, OptimizerRefusalCanResumeUnderExecutionBudget) {
    reduction::limits compiler;
    compiler.numbers.bits = 8;
    const auto original = parse("128*2+1");
    const auto reduced = reduction::reduce(original, {}, compiler);
    EXPECT_EQ(reduced.why, reduction::flag(reduction::reason::numeric_growth));
    EXPECT_EQ(runtime::evaluate(reduced.tree).output.as_number().str(), "257");
    EXPECT_EQ(runtime::evaluate(original).output.as_number().str(), "257");
    const auto large = parse("1e2000");
    EXPECT_EQ(reduction::reduce(large).kind, reduction::state::residual);
    EXPECT_EQ(runtime::evaluate(large).output.as_number().str().size(), 2001U);
}

TEST(RuntimeIntegration, ExplicitBindingFactsPreserveResultsAcrossInputValues) {
    for (const int input : { -5, 0, 1, 3, 100 }) {
        auto bindings = runtime::standard_environment();
        bindings.emplace("x", runtime::value::integer(input));
        const reduction::environment inputs {
            { "x",
              reduction::input_binding { "test.x",
                                         reduction::input_type::exact_number } }
        };
        const reduction::environment constants {
            { "x", normalized::scalar(bindings.at("x").as_number()) }
        };
        for (const auto* text :
             { "x+0", "x/1", "(x+3)*2", "x>0?x/3:0.25", "max([x+0,2+3])" }) {
            SCOPED_TRACE(text);
            const auto tree = parse(text);
            const auto direct = runtime::evaluate(tree, bindings).output;
            for (const auto* facts : { &inputs, &constants }) {
                const auto reduced = reduction::reduce(tree, *facts);
                const auto optimized
                    = runtime::evaluate(reduced.tree, bindings).output;
                EXPECT_TRUE(runtime::binary("==", direct, optimized).as_bool());
                EXPECT_EQ(
                    reduction::reduce(reduced.tree, *facts).tree, reduced.tree
                );
            }
        }
    }
}

TEST(RuntimeIntegration, ReductionPreservesErrorCodeAndMostSpecificLocation) {
    for (const auto* text :
         { "1+(1/0)", "missing+(1/0)", "true?1/0:missing", "size(1+2)",
           "{a:1}.missing", "[1,2][1+1]", "true+1", "min([])" }) {
        SCOPED_TRACE(text);
        const auto tree = parse(text);
        const auto before = execute_error(tree);
        const auto after = execute_error(reduction::reduce(tree).tree);
        EXPECT_EQ(before.code(), after.code());
        EXPECT_STREQ(before.what(), after.what());
        ASSERT_TRUE(before.location());
        ASSERT_TRUE(after.location());
        EXPECT_EQ(before.location()->begin(), after.location()->begin());
        EXPECT_EQ(before.location()->end(), after.location()->end());
    }
}

TEST(RuntimeIntegration, LegacyEagerAndLazySourcesExecuteWithIdenticalValues) {
    auto grouped = [](size_t limit) {
        reader input(source::from_memory("min([36.6*1.2,9/2,7]);"));
        grouper parser(input, limit);
        return parser.parse();
    };
    const auto eager = grouped(1024), lazy = grouped(4);
    std::ostringstream before, after;
    lazy->dump(before, false);
    for (const auto& group : { eager, lazy }) {
        const auto program = normalized::normalize_program(*group);
        const auto& statements
            = std::get<normalized::block>(program->value).statements;
        const auto tree = std::get<normalized::expression_statement>(
                              statements.front()->value
        )
                              .value;
        EXPECT_EQ(runtime::evaluate(tree).output.as_number().str(), "9/2");
        EXPECT_EQ(
            runtime::evaluate(reduction::reduce(tree).tree)
                .output.as_number()
                .str(),
            "9/2"
        );
    }
    lazy->dump(after, false);
    EXPECT_NE(before.str().find("Placeholder"), std::string::npos);
    EXPECT_EQ(before.str(), after.str());
}

TEST(RuntimeIntegration, HistoricalRootAndBuiltinScenarioUsesExplicitBindings) {
    // json-eval PathTest.FunctionsEvalJsonTest, with root/parent injection
    // expressed as lexical values and its variadic size bug removed.
    auto bindings = runtime::standard_environment();
    const auto root
        = runtime::evaluate(parse("{a:{b:[1,2,{c:'test'},[11,12]]}}")).output;
    bindings.emplace("root", root);
    bindings.emplace("a", root.member("a"));
    const auto result
        = runtime::evaluate(
              parse(
                  "[max(1,2),size(root),size([1,2,3,4]),max(a.b[0],a.b[1]),min("
                  "a.b[3]),size(a.b),size(a.b[2].c)]"
              ),
              bindings
        )
              .output;
    const std::vector<int64_t> expected { 2, 1, 4, 2, 11, 4, 4 };
    ASSERT_EQ(result.elements().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(runtime::to_int64(result.elements()[i]), expected[i]);
}

TEST(RuntimeIntegration, DeepExpressionsAndAllRealOperatorsStayBounded) {
    std::string expression = "1";
    for (int i = 0; i < 150; ++i)
        expression += "+1";
    const auto tree = parse(expression);
    EXPECT_EQ(runtime::evaluate(tree).output.as_number().str(), "151");
    EXPECT_EQ(
        runtime::evaluate(reduction::reduce(tree).tree)
            .output.as_number()
            .str(),
        "151"
    );
    auto bindings = runtime::standard_environment();
    bindings.emplace("a", runtime::value(2.0));
    bindings.emplace("b", runtime::value(4.0));
    EXPECT_TRUE(
        runtime::evaluate(parse("a<b&&a<=b&&b>a&&b>=a&&a!=b&&a==a"), bindings)
            .output.as_bool()
    );
    EXPECT_EQ(
        runtime::to_int64(
            runtime::evaluate(parse("(a+b)*(b-a)/a"), bindings).output
        ),
        6
    );
}
