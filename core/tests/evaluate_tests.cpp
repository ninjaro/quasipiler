#include "evaluate.hpp"
#include "reduce.hpp"
#include <gtest/gtest.h>

#include <bit>
#include <future>
#include <sstream>

namespace {
using runtime::error_code;
using runtime::value;
using runtime::value_kind;

normalized::expression_ptr parse(const std::string& text) {
    const auto input = source::from_memory(text, "execute.qc");
    return normalized::normalize_expression(
        source_span(input, 0, input->size())
    );
}

std::string text(const value& data) {
    switch (data.kind()) {
    case value_kind::null:
        return "null";
    case value_kind::boolean:
        return data.as_bool() ? "true" : "false";
    case value_kind::integer:
    case value_kind::rational:
        return data.as_number().str();
    case value_kind::string:
        return data.as_string();
    default:
        throw std::runtime_error("test expected scalar");
    }
}

runtime::error failure(
    const std::string& text,
    const runtime::environment& bindings = runtime::standard_environment(),
    runtime::limits limits = {}
) {
    try {
        static_cast<void>(runtime::evaluate(parse(text), bindings, limits));
        ADD_FAILURE() << "expected execution error: " << text;
    } catch (const runtime::error& result) {
        EXPECT_TRUE(result.location());
        return result;
    }
    throw std::runtime_error("execution unexpectedly succeeded");
}

struct execution_case {
    std::string expression, expected;
    value_kind kind = value_kind::integer;
};

class BaseExecution : public testing::TestWithParam<execution_case> { };
}

TEST_P(
    BaseExecution, ExecutesHistoricalAndExactBehaviorBeforeAndAfterReduction
) {
    const auto original = parse(GetParam().expression);
    const auto reduced = reduction::reduce(original);
    const auto direct = runtime::evaluate(original).output;
    const auto optimized = runtime::evaluate(reduced.tree).output;
    EXPECT_EQ(direct.kind(), GetParam().kind);
    EXPECT_EQ(optimized.kind(), direct.kind());
    EXPECT_EQ(text(direct), GetParam().expected);
    EXPECT_EQ(text(optimized), text(direct));
    EXPECT_EQ(reduction::reduce(reduced.tree).tree, reduced.tree);
}

INSTANTIATE_TEST_SUITE_P(
    Expressions, BaseExecution,
    testing::Values(
        execution_case { "2+3*4", "14" },
        execution_case { "36.6*1.2", "1098/25", value_kind::rational },
        execution_case { "1/3+1/6", "1/2", value_kind::rational },
        execution_case { "-(1+2)", "-3" }, execution_case { "+2", "2" },
        execution_case { "(9007199254740992+1)-9007199254740992", "1" },
        execution_case { "1<=2&&3>2", "true", value_kind::boolean },
        execution_case { "1<2&&2>=2", "true", value_kind::boolean },
        execution_case { "0.5==1/2", "true", value_kind::boolean },
        execution_case { "true!=false", "true", value_kind::boolean },
        execution_case { "null==null", "true", value_kind::boolean },
        execution_case { "'a'=='a'", "true", value_kind::boolean },
        execution_case { "!false||false", "true", value_kind::boolean },
        execution_case { "false&&(1/0)", "false", value_kind::boolean },
        execution_case { "true||missing", "true", value_kind::boolean },
        execution_case { "true?2+3:missing()", "5" },
        execution_case { "false?1e999999999999999999:2", "2" },
        execution_case { "[1,2,3][-1]", "3" },
        execution_case { "[1,2,3][-3]", "1" },
        execution_case { "{a:[{b:7}]}.a[0].b", "7" },
        execution_case { "{'x y':5}['x y']", "5" },
        execution_case { "{a:null}.a", "null", value_kind::null },
        execution_case { "['first','last'][-1]", "last", value_kind::string },
        execution_case { "size([])", "0" },
        execution_case { "size([1,[2],3])", "3" },
        execution_case { "size({a:1,b:2})", "2" },
        execution_case { "size('hello')", "5" },
        execution_case { "len('')", "0" }, execution_case { "len('é')", "2" },
        execution_case { "size('a\\u0000b')", "3" },
        execution_case { "len('a\\nb')", "3" },
        execution_case { "len({})", "0" }, execution_case { "min(7)", "7" },
        execution_case { "max(-4)", "-4" },
        execution_case { "min(0.5)", "1/2", value_kind::rational },
        execution_case { "min(4,2,-3,7)", "-3" },
        execution_case { "max([4,2,-3,7])", "7" },
        execution_case { "min([1,1/3,0.5])", "1/3", value_kind::rational },
        execution_case { "max(9007199254740992,9007199254740993)",
                         "9007199254740993" },
        execution_case { "max(min(1,2),size([3,4,5]))", "3" },
        execution_case { "[min,max][0](5,2)", "2" },
        execution_case { "{f:size}.f('abc')", "3" },
        execution_case { "(true?min:max)(5,2)", "2" }
    )
);

struct error_case {
    std::string expression;
    error_code code;
};

class ExecutionError : public testing::TestWithParam<error_case> { };

TEST_P(ExecutionError, RequiredExecutionHasStructuredSourceLocatedFailure) {
    const auto result = failure(GetParam().expression);
    EXPECT_EQ(result.code(), GetParam().code) << result.what();
    ASSERT_TRUE(result.location());
    EXPECT_EQ(result.location()->owner()->name(), "execute.qc");
    EXPECT_NE(
        std::string(result.what()).find("execute.qc:1:"), std::string::npos
    );
}

INSTANTIATE_TEST_SUITE_P(
    Errors, ExecutionError,
    testing::Values(
        error_case { "missing", error_code::unknown_name },
        error_case { "missing()", error_code::unknown_name },
        error_case { "1(2)", error_code::not_callable },
        error_case { "1/0", error_code::division_by_zero },
        error_case { "true&&1", error_code::type },
        error_case { "1?2:3", error_code::type },
        error_case { "null==false", error_code::type },
        error_case { "'a'<'b'", error_code::type },
        error_case { "[]==[]", error_code::unsupported_operation },
        error_case { "[][-1]", error_code::index_bounds },
        error_case { "[1,2][2]", error_code::index_bounds },
        error_case { "[1][-9223372036854775808]", error_code::index_bounds },
        error_case { "[1][0.5]", error_code::type },
        error_case { "1[0]", error_code::type },
        error_case { "{a:1}.b", error_code::missing_key },
        error_case { "{a:1}[1]", error_code::type },
        error_case { "{a:1,a:2}", error_code::duplicate_key },
        error_case { "size()", error_code::arity },
        error_case { "size(1,2,3,4)", error_code::arity },
        error_case { "size(1)", error_code::type },
        error_case { "size(null)", error_code::type },
        error_case { "min()", error_code::arity },
        error_case { "max([])", error_code::empty_collection },
        error_case { "min('abc')", error_code::type },
        error_case { "max(null)", error_code::type },
        error_case { "min(false)", error_code::type },
        error_case { "min({a:1})", error_code::type },
        error_case { "max([[1]])", error_code::type },
        error_case { "min([1],2)", error_code::type },
        error_case { "x=1", error_code::unsupported_operation },
        error_case { "x++", error_code::unsupported_operation },
        error_case { "++x", error_code::unsupported_operation },
        error_case { "1%2", error_code::unsupported_operation },
        error_case { "~1", error_code::unsupported_operation },
        error_case { "fu(x){return x;}", error_code::unsupported_operation },
        error_case { "(1,2)", error_code::unsupported_operation },
        error_case { "[1,2][:]", error_code::unsupported_operation },
        error_case { "1e9999999999999999999", error_code::resource }
    )
);

TEST(
    Evaluator, CallableIdentitySupportsAliasesShadowingAndCompleteEnvironments
) {
    auto bindings = runtime::standard_environment();
    bindings.emplace("smallest", bindings.at("min"));
    bindings["min"] = value::integer(7);
    EXPECT_EQ(
        text(runtime::evaluate(parse("smallest(5,3)"), bindings).output), "3"
    );
    EXPECT_EQ(failure("min(5,3)", bindings).code(), error_code::not_callable);
    EXPECT_EQ(failure("size([])", {}).code(), error_code::unknown_name);
    ASSERT_EQ(runtime::builtins().size(), 4U);
    EXPECT_EQ(
        bindings.at("size").as_builtin(), bindings.at("len").as_builtin()
    );
    const auto compiled = reduction::reduce(parse("size('abc')"));
    EXPECT_EQ(compiled.kind, reduction::state::residual);
    EXPECT_EQ(text(runtime::evaluate(compiled.tree).output), "3");
}

TEST(Evaluator, CalleeArgumentsAndOperandsExecuteInSourceOrder) {
    std::vector<std::string> trace;
    auto bindings = runtime::standard_environment();
    bindings.emplace("left", value::integer(3));
    bindings.emplace("right", value::integer(2));
    bindings.emplace("index", value::integer(1));
    const runtime::resolver resolver
        = [&](std::string_view name) -> std::optional<value> {
        trace.emplace_back(name);
        const auto found = bindings.find(name);
        return found == bindings.end() ? std::nullopt
                                       : std::optional(found->second);
    };
    const auto result = runtime::evaluate_with(
        parse("min(left,right)+[left,right][index]"), resolver
    );
    EXPECT_EQ(text(result.output), "4");
    EXPECT_EQ(
        trace,
        (std::vector<std::string> { "min", "left", "right", "left", "right",
                                    "index" })
    );
    trace.clear();
    EXPECT_EQ(
        text(
            runtime::evaluate_with(parse("{a:left,b:right}.a"), resolver).output
        ),
        "3"
    );
    EXPECT_EQ(trace, (std::vector<std::string> { "left", "right" }));
    trace.clear();
    try {
        runtime::evaluate_with(parse("size(left,right)"), resolver);
        FAIL();
    } catch (const runtime::error& error) {
        EXPECT_EQ(error.code(), error_code::arity);
    }
    EXPECT_EQ(trace, (std::vector<std::string> { "size", "left", "right" }));
    trace.clear();
    try {
        runtime::evaluate_with(parse("left(right,index)"), resolver);
        FAIL();
    } catch (const runtime::error& error) {
        EXPECT_EQ(error.code(), error_code::not_callable);
    }
    EXPECT_EQ(trace, (std::vector<std::string> { "left", "right", "index" }));
}

TEST(Evaluator, ShortCircuitAndErrorsStopUnselectedOrLaterReads) {
    std::vector<std::string> trace;
    const runtime::resolver resolver
        = [&](std::string_view name) -> std::optional<value> {
        trace.emplace_back(name);
        if (name == "condition")
            return value(false);
        if (name == "chosen")
            return value::integer(7);
        throw runtime::error(
            error_code::unknown_name, "host binding unavailable"
        );
    };
    EXPECT_FALSE(
        runtime::evaluate_with(parse("condition&&never"), resolver)
            .output.as_bool()
    );
    EXPECT_EQ(trace, (std::vector<std::string> { "condition" }));
    trace.clear();
    EXPECT_EQ(
        text(
            runtime::evaluate_with(parse("condition?never:chosen"), resolver)
                .output
        ),
        "7"
    );
    EXPECT_EQ(trace, (std::vector<std::string> { "condition", "chosen" }));
    trace.clear();
    try {
        runtime::evaluate_with(parse("missing+chosen"), resolver);
        FAIL();
    } catch (const runtime::error& error) {
        EXPECT_EQ(error.location()->text(), "missing");
    }
    EXPECT_EQ(trace, (std::vector<std::string> { "missing" }));
    trace.clear();
    try {
        runtime::evaluate_with(parse("{a:chosen,a:never}"), resolver);
        FAIL();
    } catch (const runtime::error& error) {
        EXPECT_EQ(error.code(), error_code::duplicate_key);
    }
    EXPECT_EQ(trace, (std::vector<std::string> { "chosen" }));
}

TEST(Evaluator, FunctionAndMutationSyntaxNeverExecutesItsChildren) {
    size_t calls = 0;
    const runtime::resolver resolver
        = [&](std::string_view) -> std::optional<value> {
        ++calls;
        return value();
    };
    for (const auto* source :
         { "x=y", "x++", "fu(x){return y;}", "a[:b]", "(x,y)" }) {
        try {
            runtime::evaluate_with(parse(source), resolver);
            FAIL() << source;
        } catch (const runtime::error& error) {
            EXPECT_EQ(error.code(), error_code::unsupported_operation);
        }
    }
    EXPECT_EQ(calls, 0U);
}

TEST(Evaluator, InvocationBudgetsCoverNodesBuiltinScansAndConstruction) {
    runtime::limits options;
    options.steps = 9;
    EXPECT_EQ(
        failure("min([3,2,1])", runtime::standard_environment(), options)
            .code(),
        error_code::resource
    );
    options.steps = 10;
    const auto result = runtime::evaluate(
        parse("min([3,2,1])"), runtime::standard_environment(), options
    );
    EXPECT_EQ(text(result.output), "1");
    EXPECT_EQ(result.steps, 10U);
    EXPECT_EQ(result.elements, 4U);
    options.elements = 3;
    EXPECT_EQ(
        failure("min([3,2,1])", runtime::standard_environment(), options)
            .code(),
        error_code::resource
    );
    options = {};
    options.string_bytes = 2;
    EXPECT_EQ(failure("'abc'", {}, options).code(), error_code::resource);
    EXPECT_EQ(failure("{abc:1}", {}, options).code(), error_code::resource);
    options.depth = 2;
    EXPECT_EQ(failure("1+(2+3)", {}, options).code(), error_code::resource);
    options = {};
    options.steps = 2;
    EXPECT_FALSE(
        runtime::evaluate(parse("false&&(1e999999999999+missing)"), {}, options)
            .output.as_bool()
    );
    options.steps = 0;
    EXPECT_EQ(failure("1", {}, options).code(), error_code::resource);
    options = {};
    options.numbers.bits = 8;
    EXPECT_EQ(failure("128*2", {}, options).code(), error_code::resource);
    const auto compiled = reduction::reduce(parse("128*2"));
    try {
        runtime::evaluate(compiled.tree, {}, options);
        FAIL();
    } catch (const runtime::error& error) {
        EXPECT_EQ(error.code(), error_code::resource);
    }
    EXPECT_EQ(
        failure("x", { { "x", runtime::numeric_literal("256") } }, options)
            .code(),
        error_code::resource
    );
}

TEST(Evaluator, RealsAndMinMaxTiesPreserveTheirDomainAndFirstValue) {
    auto bindings = runtime::standard_environment();
    bindings.emplace("negative", value(-0.0));
    bindings.emplace("positive", value(0.0));
    bindings.emplace("a", value(0.25));
    bindings.emplace("b", value(0.5));
    bindings.emplace("list", value::list({ value(1.0), value(-2.0) }));
    EXPECT_EQ(
        std::bit_cast<uint64_t>(
            runtime::evaluate(parse("min(negative,positive)"), bindings)
                .output.as_real()
        ),
        0x8000000000000000ULL
    );
    EXPECT_EQ(
        std::bit_cast<uint64_t>(
            runtime::evaluate(parse("max(positive,negative)"), bindings)
                .output.as_real()
        ),
        0U
    );
    EXPECT_EQ(
        std::bit_cast<uint64_t>(
            runtime::evaluate(parse("min(a,b)+a"), bindings).output.as_real()
        ),
        std::bit_cast<uint64_t>(0.5)
    );
    EXPECT_EQ(
        std::bit_cast<uint64_t>(
            runtime::evaluate(parse("min(list)"), bindings).output.as_real()
        ),
        std::bit_cast<uint64_t>(-2.0)
    );
    EXPECT_EQ(failure("min(a,1)", bindings).code(), error_code::type);
    EXPECT_EQ(failure("a+1", bindings).code(), error_code::type);
}

TEST(
    Evaluator, IndependentConcurrentInvocationsReleaseSuccessfulSourceSnapshots
) {
    std::weak_ptr<const source> weak;
    value output;
    {
        const auto input = source::from_memory("{a:['owned',3]}");
        weak = input;
        const auto tree = normalized::normalize_expression(
            source_span(input, 0, input->size())
        );
        auto one = std::async(std::launch::async, [&] {
            return runtime::evaluate(tree);
        });
        auto two = std::async(std::launch::async, [&] {
            return runtime::evaluate(tree);
        });
        output = one.get().output;
        EXPECT_EQ(
            two.get().output.member("a").elements()[1].as_number().str(), "3"
        );
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(output.member("a").elements()[0].as_string(), "owned");
    EXPECT_THROW(runtime::evaluate(nullptr), std::invalid_argument);
    EXPECT_THROW(runtime::evaluate_with(parse("1"), {}), std::invalid_argument);
}
