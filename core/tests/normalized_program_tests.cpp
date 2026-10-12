#include "normalized.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace {
normalized::statement_ptr
program(const std::string& text, normalized::limits budget = {}) {
    const auto input = source::from_memory(text, "program.qc");
    return normalized::normalize_program(
        source_span(input, 0, input->size()), budget
    );
}

normalized::expression_ptr expression(const std::string& text) {
    const auto input = source::from_memory(text, "function.qc");
    return normalized::normalize_expression(
        source_span(input, 0, input->size())
    );
}

template <class Ptr> std::string shape(const Ptr& value) {
    std::ostringstream result;
    normalized::dump(result, value);
    return result.str();
}

template <class Ptr> void check_tree(const Ptr& parent) {
    ASSERT_NE(parent, nullptr);
    size_t height = 1;
    auto visit = [&](const auto& child) {
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(child->span.owner(), parent->span.owner());
        EXPECT_GE(child->span.begin().offset, parent->span.begin().offset);
        EXPECT_LE(child->span.end().offset, parent->span.end().offset);
        height = std::max(height, child->height + 1);
        check_tree(child);
    };
    normalized::for_each_child(*parent, visit, visit);
    EXPECT_EQ(parent->height, height);
}

const normalized::block& root(const normalized::statement_ptr& value) {
    return std::get<normalized::block>(value->value);
}
}

TEST(NormalizedProgram, FunctionExpressionsHaveParametersAndNormalizedBody) {
    const auto value = expression("fu(x){ return x+1; }");
    const auto* function = std::get_if<normalized::function>(&value->value);
    ASSERT_NE(function, nullptr);
    ASSERT_EQ(function->parameters.size(), 1u);
    EXPECT_EQ(function->parameters[0].text, "x");
    EXPECT_EQ(function->parameters[0].span.begin().offset, 3);
    ASSERT_EQ(root(function->body).statements.size(), 1u);
    const auto& jump
        = std::get<normalized::jump>(root(function->body).statements[0]->value);
    EXPECT_EQ(jump.keyword.text, "return");
    EXPECT_EQ(std::get<normalized::binary>(jump.value->value).op.text, "+");
    EXPECT_EQ(function->body->span.text(), "{ return x+1; }");
    check_tree(value);
}

TEST(NormalizedProgram, NestedClosuresCallsAndNamedDefinitionsCompose) {
    const auto value
        = program("adder(x){return fu(y){return x+y;};} result=adder(1)(2);");
    ASSERT_EQ(root(value).statements.size(), 2u);
    const auto& definition
        = std::get<normalized::definition>(root(value).statements[0]->value);
    EXPECT_EQ(definition.name.text, "adder");
    EXPECT_NE(
        shape(value).find(
            "(call (call (id \"adder\") (integer \"1\")) (integer \"2\"))"
        ),
        std::string::npos
    );
    check_tree(value);
    const auto immediate = expression("fu(x){return x;}(1).value");
    EXPECT_NE(
        shape(immediate).find("(member (call (function"), std::string::npos
    );
    check_tree(immediate);
}

TEST(NormalizedProgram, ObjectBlockAndLabelContextsRemainDistinct) {
    EXPECT_TRUE(
        std::holds_alternative<normalized::object>(expression("{}")->value)
    );
    const auto value
        = program("{} x={a:1,b:x+2}; {again: x=x+1;} {'quoted':3};");
    ASSERT_EQ(root(value).statements.size(), 4u);
    EXPECT_TRUE(
        std::holds_alternative<normalized::block>(
            root(value).statements[0]->value
        )
    );
    const auto& binding = std::get<normalized::expression_statement>(
        root(value).statements[1]->value
    );
    const auto& assignment = std::get<normalized::binary>(binding.value->value);
    EXPECT_TRUE(
        std::holds_alternative<normalized::object>(assignment.right->value)
    );
    const auto& body
        = std::get<normalized::block>(root(value).statements[2]->value);
    EXPECT_TRUE(
        std::holds_alternative<normalized::label>(body.statements[0]->value)
    );
    check_tree(value);
}

TEST(NormalizedProgram, ConditionalChainsAndDanglingElseBindStructurally) {
    const auto value = program("if(a)if(b)x();else y();elif(c)z();else q();");
    ASSERT_EQ(root(value).statements.size(), 1u);
    const auto& outer
        = std::get<normalized::branch>(root(value).statements[0]->value);
    ASSERT_NE(outer.yes, nullptr);
    const auto& inner = std::get<normalized::branch>(outer.yes->value);
    EXPECT_NE(inner.no, nullptr);
    ASSERT_NE(outer.no, nullptr);
    const auto& next = std::get<normalized::branch>(outer.no->value);
    EXPECT_EQ(next.keyword.text, "elif");
    EXPECT_NE(next.no, nullptr);
    check_tree(value);
}

TEST(NormalizedProgram, LoopsJumpsAndOmittedForClausesAreExplicit) {
    const auto value = program(
        "start: for(;;){if(ok)break;continue;} while(x<n)x++; goto start; "
        "return;"
    );
    ASSERT_EQ(root(value).statements.size(), 5u);
    const auto& loop
        = std::get<normalized::for_loop>(root(value).statements[1]->value);
    EXPECT_EQ(loop.setup, nullptr);
    EXPECT_EQ(loop.condition, nullptr);
    EXPECT_EQ(loop.step, nullptr);
    const auto& go
        = std::get<normalized::jump>(root(value).statements[3]->value);
    ASSERT_TRUE(go.target);
    EXPECT_EQ(go.target->text, "start");
    const auto& ret
        = std::get<normalized::jump>(root(value).statements[4]->value);
    EXPECT_EQ(ret.value, nullptr);
    check_tree(value);
    check_tree(program("for(i=0,j=1;i<n;i++,j--)f(i,j);"));
}

TEST(NormalizedProgram, TryHandlersAndFinallyHaveOrderedBodies) {
    const auto value = program(
        "try{f();}catch(a){return a;}catch(b)g(b);finally cleanup();"
    );
    const auto& block
        = std::get<normalized::try_statement>(root(value).statements[0]->value);
    ASSERT_EQ(block.handlers.size(), 2u);
    EXPECT_EQ(block.handlers[0].binding.text, "a");
    EXPECT_EQ(block.handlers[1].binding.text, "b");
    EXPECT_NE(block.finalizer, nullptr);
    check_tree(value);
    check_tree(program("try f();finally g();"));
}

TEST(NormalizedProgram, ReturnThroughConditionalRetainsControlStructure) {
    const auto value = program("return if(ok){result} else null;");
    const auto& ret
        = std::get<normalized::jump>(root(value).statements[0]->value);
    EXPECT_EQ(ret.value, nullptr);
    ASSERT_NE(ret.conditional, nullptr);
    EXPECT_TRUE(
        std::holds_alternative<normalized::branch>(ret.conditional->value)
    );
    check_tree(value);
}

TEST(NormalizedProgram, ParametersRemainSourceOwnedAndCanBeEmpty) {
    const auto value = expression("fu(){return null;}");
    const auto& function = std::get<normalized::function>(value->value);
    EXPECT_TRUE(function.parameters.empty());
    EXPECT_EQ(function.introducer.span.text(), "fu");
    EXPECT_EQ(function.open.span.text(), "(");
    EXPECT_EQ(function.close.span.text(), ")");
    check_tree(value);
    check_tree(expression("[fu(x){return x;}, {f:fu(y){return y;}}]"));
}

TEST(NormalizedProgram, EmptyProgramsAndOptionalFinalSemicolonsAreStable) {
    EXPECT_TRUE(root(program(" // empty\n")).statements.empty());
    EXPECT_EQ(shape(program("a")), shape(program("a;")));
    EXPECT_EQ(shape(program("return 1")), shape(program("return 1;")));
    check_tree(program("{a;b}"));
    EXPECT_EQ(root(program(";;")).statements.size(), 2u);
}

class InvalidNormalizedProgram : public testing::TestWithParam<std::string> { };

TEST_P(InvalidNormalizedProgram, RejectsMalformedStructureWithLocation) {
    try {
        static_cast<void>(program(GetParam()));
        FAIL() << "accepted invalid program";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("program.qc:"), std::string::npos
        );
    }
}

INSTANTIATE_TEST_SUITE_P(
    Programs, InvalidNormalizedProgram,
    testing::Values(
        "fu(1){}", "fu(x,x){}", "fu(true){}", "fu(x)", "fu(x){return x;",
        "if()x;", "if(a)", "else x;", "elif(a)x;", "while(a)", "for(a;b)x;",
        "goto;", "goto a+b;", "break x;", "continue x;", "catch(e)x;", "try{}",
        "try{}catch(){}", "try{}finally{}catch(e){}", "if(a){'x':1}",
        "return if(a)b;else c;", "x y;", "label ':'", "{a:1,b:2}", "a(1 2);"
    )
);

TEST(NormalizedProgram, DepthLimitsAlsoCoverFunctionAndStatementEdges) {
    std::string text = "return 1;";
    for (int i = 0; i < 30; ++i)
        text = "if(a){" + text + "}";
    EXPECT_THROW(
        static_cast<void>(program(text, { 32, 10000 })), std::runtime_error
    );
    EXPECT_NO_THROW(static_cast<void>(program(text, { 256, 10000 })));
    EXPECT_THROW(
        static_cast<void>(program("a;b;c;", { 256, 3 })), std::runtime_error
    );
}

class NormalizedSamples : public testing::TestWithParam<int> { };

TEST_P(NormalizedSamples, EntireExistingCorpusNormalizesAndKeepsInvariants) {
    std::ostringstream name;
    name << "test" << std::setfill('0') << std::setw(2) << GetParam() << ".qc";
    const auto input = source::from_file(
        std::filesystem::path(QPILER_TEST_DATA_DIR) / name.str()
    );
    const source_span span(input, 0, input->size());
    const auto value = normalized::normalize_program(span);
    EXPECT_FALSE(root(value).statements.empty());
    EXPECT_EQ(shape(value), shape(normalized::normalize_program(span)));
    check_tree(value);
}

INSTANTIATE_TEST_SUITE_P(Programs, NormalizedSamples, testing::Range(0, 13));
