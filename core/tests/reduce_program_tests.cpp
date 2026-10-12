#include "grouper.hpp"
#include "reduce.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace {
normalized::statement_ptr parse(const std::string& text) {
    const auto input = source::from_memory(text, "program.qc");
    return normalized::normalize_program(source_span(input, 0, input->size()));
}

template <class Ptr> std::string shape(const Ptr& tree) {
    std::ostringstream out;
    normalized::dump(out, tree);
    return out.str();
}

std::string report(const reduction::program_result& result) {
    std::ostringstream out;
    reduction::dump(out, result);
    return out.str();
}

template <class Ptr> void invariants(const Ptr& tree) {
    size_t height = 1;
    auto child = [&](const auto& node) {
        EXPECT_EQ(tree->span.owner(), node->span.owner());
        EXPECT_LE(tree->span.begin().offset, node->span.begin().offset);
        EXPECT_GE(tree->span.end().offset, node->span.end().offset);
        height = std::max(height, node->height + 1);
        invariants(node);
    };
    normalized::for_each_child(*tree, child, child);
    EXPECT_EQ(tree->height, height);
}
}

TEST(ProgramReduction, FoldsAssignmentsWithoutPropagatingMutableBindings) {
    const auto original = parse("a=2+3; b=a+1; a=10; return b;");
    const auto reduced = reduction::reduce_program(original);
    EXPECT_EQ(
        shape(reduced.tree),
        "(program (expression (= (id \"a\") (exact \"5\"))) (expression (= (id "
        "\"b\") (+ (id \"a\") (exact \"1\")))) (expression (= (id \"a\") "
        "(exact \"10\"))) (return (id \"b\")))\n"
    );
    ASSERT_EQ(reduced.expressions.size(), 4U);
    EXPECT_EQ(reduced.expressions[0].original.text(), "a=2+3");
    EXPECT_EQ(reduced.expressions[3].value.kind, reduction::state::unresolved);
    EXPECT_EQ(reduction::reduce_program(reduced.tree).tree, reduced.tree);
    invariants(reduced.tree);
}

TEST(ProgramReduction, VisitsControlExpressionsWithoutRewritingControlFlow) {
    const auto original = parse(
        "if(1<2){return 3+4;}else{return 5+6;} while(2>3){x=4+5;} "
        "for(i=1+1;i<2+3;i+=1+1){return 2*3;} "
    );
    const auto reduced = reduction::reduce_program(original);
    const auto text = shape(reduced.tree);
    EXPECT_NE(text.find("(if (boolean \"true\")"), std::string::npos);
    EXPECT_NE(text.find("(return (exact \"7\"))"), std::string::npos);
    EXPECT_NE(text.find("(return (exact \"11\"))"), std::string::npos);
    EXPECT_NE(text.find("(while (boolean \"false\")"), std::string::npos);
    EXPECT_NE(
        text.find(
            "(for (= (id \"i\") (exact \"2\")) (< (id \"i\") (exact \"5\")) "
            "(+= (id \"i\") (exact \"2\"))"
        ),
        std::string::npos
    );
    EXPECT_EQ(reduction::reduce_program(reduced.tree).tree, reduced.tree);
    invariants(reduced.tree);
}

TEST(ProgramReduction, PreservesLabelsJumpsHandlersAndDeferredFunctions) {
    const auto original = parse(
        "start:; try {return 1+2;} catch(e){return 2+3;} finally {x=3+4;} goto "
        "start; break; continue; f(x){return x+1/0;} return "
        "if(true){1+2;}else{3+4;};"
    );
    const auto reduced = reduction::reduce_program(original);
    const auto text = shape(reduced.tree);
    EXPECT_NE(text.find("(return (exact \"3\"))"), std::string::npos);
    EXPECT_NE(text.find("(return (exact \"5\"))"), std::string::npos);
    EXPECT_NE(text.find("(= (id \"x\") (exact \"7\"))"), std::string::npos);
    EXPECT_NE(
        text.find("(/ (integer \"1\") (integer \"0\"))"), std::string::npos
    );
    EXPECT_EQ(
        reduced.why & reduction::flag(reduction::reason::division_by_zero), 0U
    );
    const auto& before
        = std::get<normalized::block>(original->value).statements;
    const auto& after
        = std::get<normalized::block>(reduced.tree->value).statements;
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        if (std::holds_alternative<normalized::label>(before[i]->value)
            || std::holds_alternative<normalized::definition>(
                before[i]->value
            )) {
            EXPECT_EQ(before[i], after[i]);
        }
    }
    invariants(reduced.tree);
}

TEST(ProgramReduction, WholeProgramBudgetRollsBackTreesAndReportsTogether) {
    const auto tree = parse("return 1+2; return 3+4;");
    reduction::limits budget;
    budget.steps = 8;
    const auto reduced = reduction::reduce_program(tree, budget);
    EXPECT_EQ(reduced.tree, tree);
    EXPECT_TRUE(reduced.expressions.empty());
    EXPECT_EQ(report(reduced), "program [expression_growth]\n");
    EXPECT_EQ(reduction::reduce_program(reduced.tree, budget).tree, tree);
    budget.steps = 9;
    EXPECT_EQ(reduction::reduce_program(tree, budget).why, 0U);
    budget.depth = 3;
    EXPECT_EQ(reduction::reduce_program(tree, budget).tree, tree);
    budget.depth = 4;
    EXPECT_EQ(reduction::reduce_program(tree, budget).why, 0U);
    EXPECT_THROW(reduction::reduce_program(nullptr), std::invalid_argument);
}

TEST(
    ProgramReduction, ReportsExactResultsAndResidualLocationsDeterministically
) {
    const auto tree = parse("return 36.6*1.2; return x+0; return 1/0;");
    const auto reduced = reduction::reduce_program(tree);
    const auto text = report(reduced);
    EXPECT_EQ(text, report(reduction::reduce_program(tree)));
    EXPECT_NE(
        text.find("7:15 known [] (exact \"1098/25\")"), std::string::npos
    );
    EXPECT_NE(
        text.find("residual [unknown_symbol] (+ (id \"x\") (exact \"0\"))"),
        std::string::npos
    );
    EXPECT_NE(text.find("residual [division_by_zero]"), std::string::npos);
    EXPECT_EQ(
        shape(reduced.tree),
        shape(
            reduction::reduce_program(parse(std::string(tree->span.text())))
                .tree
        )
    );
}

TEST(
    ProgramReduction,
    EagerAndLazySourceAdaptersReduceIdenticallyAfterReaderDestruction
) {
    const std::string text
        = "{[1+2,3+4,5+6],[7+8,9+10,11+12],[13+14,15+16,17+18]}";
    auto grouped = [&](size_t limit) {
        reader input(source::from_memory(text));
        grouper parser(input, limit);
        return parser.parse();
    };
    const auto eager = grouped(1024), lazy = grouped(4);
    std::ostringstream before, after;
    lazy->dump(before, false);
    const auto first
        = reduction::reduce_program(normalized::normalize_program(*eager));
    const auto second
        = reduction::reduce_program(normalized::normalize_program(*lazy));
    lazy->dump(after, false);
    EXPECT_NE(before.str().find("Placeholder"), std::string::npos);
    EXPECT_EQ(before.str(), after.str());
    EXPECT_EQ(shape(first.tree), shape(second.tree));
    EXPECT_EQ(report(first), report(second));
    invariants(second.tree);
}

TEST(ProgramReduction, HistoricalCorpusReachesFixedPointWithValidOwnedTrees) {
    size_t count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(QPILER_TEST_DATA_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".qc")
            continue;
        SCOPED_TRACE(entry.path().filename().string());
        const auto input = source::from_file(entry.path());
        const auto tree = normalized::normalize_program(
            source_span(input, 0, input->size())
        );
        const auto first = reduction::reduce_program(tree);
        const auto second = reduction::reduce_program(first.tree);
        EXPECT_EQ(second.tree, first.tree);
        EXPECT_EQ(second.why, first.why);
        EXPECT_EQ(report(first), report(reduction::reduce_program(tree)));
        EXPECT_EQ(
            first.why & reduction::flag(reduction::reason::expression_growth),
            0U
        );
        invariants(first.tree);
        ++count;
    }
    EXPECT_EQ(count, 13U);
}
