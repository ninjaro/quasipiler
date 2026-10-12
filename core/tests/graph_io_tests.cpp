#include "graph_io.hpp"
#include "reduce.hpp"
#include <gtest/gtest.h>

#include <bit>
#include <charconv>
#include <future>
#include <limits>

namespace {
using namespace runtime;

normalized::expression_ptr parse(std::string text) {
    const auto source = source::from_memory(std::move(text), "graph-data.qc");
    return normalized::normalize_expression(
        source_span(source, 0, source->size())
    );
}

graph_handle graph(std::string text) {
    return graph_from_expression(parse(std::move(text)));
}

value read(const graph_handle& root, std::string text) {
    return evaluate_in(
               parse(std::move(text)),
               { value::graph(root), value::graph(root) }
    )
        .output;
}

template <class Action> error failure(error_code code, Action action) {
    try {
        action();
        ADD_FAILURE() << "expected graph I/O failure";
    } catch (const error& result) {
        EXPECT_EQ(result.code(), code);
        return result;
    }
    throw std::runtime_error("graph I/O test did not fail");
}
}

TEST(
    GraphSource, HistoricalParentSelfAndRootPathsConstructWithoutEagerResolution
) {
    const auto root = graph("{key1:5,key2:@,key3:55,alias:@.key1,root:$}");
    EXPECT_EQ(to_int64(read(root, "$.key2.key2.key2.key1")), 5);
    EXPECT_EQ(to_int64(read(root, "$.alias")), 5);
    EXPECT_TRUE(read(root, "$.root").as_graph().same_identity(root));
    EXPECT_EQ(root.member("key2").kind(), graph_kind::reference);
}

TEST(GraphSource, MutualReferencesThroughCollectionsKeepAliasesVisible) {
    const auto root
        = graph("{a:{b:$.b,value:1},b:{a:$.a,value:2},list:[$.a,$.b,$.a]}");
    EXPECT_EQ(to_int64(read(root, "$.a.b.a.b.value")), 2);
    EXPECT_TRUE(read(root, "$.list[0]")
                    .as_graph()
                    .same_identity(read(root, "$.list[2]").as_graph()));
    EXPECT_TRUE(read(root, "$.a")
                    .as_graph()
                    .same_identity(read(root, "$.b.a").as_graph()));
}

TEST(
    GraphSource, IndependentScalarExpressionsUseExactArithmeticAndPureBuiltins
) {
    const auto root
        = graph("{a:36.6*1.2,b:size('hello'),c:min([4,1,2]),d:null,e:true}");
    EXPECT_EQ(read(root, "$.a").as_number().str(), "1098/25");
    EXPECT_EQ(to_int64(read(root, "$.b+$.c")), 6);
    EXPECT_EQ(read(root, "$.d").kind(), value_kind::null);
    EXPECT_TRUE(read(root, "$.e").as_bool());
    const auto reduced = reduction::reduce(parse("{value:2+3,self:@}"));
    EXPECT_EQ(
        to_int64(read(graph_from_expression(reduced.tree), "$.self.value")), 5
    );
}

TEST(GraphSource, InvalidConstructionHasStableLocatedErrorsAndSourceOrder) {
    const auto error
        = failure(error_code::division_by_zero, [] { graph("[1/0,missing]"); });
    EXPECT_EQ(error.location()->text(), "1/0");
    failure(error_code::duplicate_key, [] { graph("{a:1,a:2}"); });
    failure(error_code::unknown_name, [] { graph("{a:missing}"); });
    failure(error_code::invalid_reference, [] { graph("{a:$.x+1}"); });
    failure(error_code::type, [] { graph("true?[1]:[2]"); });
    EXPECT_THROW(graph_from_expression({}), std::invalid_argument);
}

TEST(GraphSource, ConstructionAndScalarExecutionShareFiniteBudgets) {
    limits options;
    options.steps = 8;
    failure(error_code::resource, [&] {
        graph_from_expression(parse("[1+2,3+4,5+6]"), {}, options);
    });
    options = {};
    options.elements = 1;
    failure(error_code::resource, [&] {
        graph_from_expression(parse("[1,2]"), {}, options);
    });
    options = {};
    options.depth = 2;
    failure(error_code::resource, [&] {
        graph_from_expression(parse("[[[1]]]"), {}, options);
    });
    graph_limits storage;
    storage.recipe_nodes = 1;
    failure(error_code::resource, [&] {
        graph_from_expression(parse("{a:$.b}"), storage);
    });
    storage = {};
    storage.nodes = 2;
    failure(error_code::resource, [&] {
        graph_from_expression(parse("[1,2]"), storage);
    });
}

TEST(GraphSource, RecipeSourcesFollowArenaLifetimeAndFailedBuildsReleaseThem) {
    std::weak_ptr<const source> source_lifetime;
    graph_handle root;
    {
        const auto tree = parse("{value:42,self:@}");
        source_lifetime = tree->span.owner();
        root = graph_from_expression(tree);
    }
    EXPECT_FALSE(source_lifetime.expired());
    EXPECT_EQ(to_int64(read(root, "$.self.value")), 42);
    root = {};
    EXPECT_TRUE(source_lifetime.expired());
    {
        const auto tree = parse("[missing,@]");
        source_lifetime = tree->span.owner();
        failure(error_code::unknown_name, [&] { graph_from_expression(tree); });
    }
    EXPECT_TRUE(source_lifetime.expired());
}

TEST(GraphJson, AcyclicValuesAndGraphsProduceDeterministicOrderedJson) {
    const auto tree = parse("{z:[1,true,null],a:'hello',empty:{}}");
    const auto ordinary = evaluate(tree).output;
    const auto expected = "{\"z\":[1,true,null],\"a\":\"hello\",\"empty\":{}}";
    EXPECT_EQ(serialize_json(ordinary), expected);
    EXPECT_EQ(
        serialize_json(value::graph(graph_from_expression(tree))), expected
    );
    EXPECT_EQ(serialize_json(value::list({})), "[]");
    EXPECT_EQ(serialize_json(value::object({})), "{}");
}

TEST(GraphJson, RepeatedAcyclicAliasesAreExpandedAndContextualRecipesResolve) {
    const auto root = graph(
        "{item:{value:7,alias:@.value},copy:$.item,list:[$.item,$.item]}"
    );
    EXPECT_EQ(
        serialize_json(value::graph(root)),
        "{\"item\":{\"value\":7,\"alias\":7},\"copy\":{\"value\":7,\"alias\":7}"
        ",\"list\":[{\"value\":7,\"alias\":7},{\"value\":7,\"alias\":7}]}"
    );
    EXPECT_EQ(
        serialize_json(read(root, "$.list{1,0}"), { value::graph(root), {} }),
        "[{\"value\":7,\"alias\":7},{\"value\":7,\"alias\":7}]"
    );
}

TEST(GraphJson, DirectSelfMutualAndRecipeCyclesHaveExplicitErrors) {
    for (const auto* text : { "{self:@}", "{a:{b:$.b},b:{a:$.a}}", "[@]" }) {
        const auto root = graph(text);
        failure(error_code::serialization_cycle, [&] {
            serialize_json(value::graph(root));
        });
    }
    graph_builder build;
    const auto root = build.declare();
    build.define_list(root, { root });
    const auto direct = build.publish(root);
    failure(error_code::serialization_cycle, [&] {
        serialize_json(value::graph(direct));
    });
    const auto loop = graph("{a:$.a}");
    failure(error_code::reference_cycle, [&] {
        serialize_json(value::graph(loop));
    });
}

TEST(GraphJson, ContextualCyclesThroughOrdinaryRuntimeCollectionsAreDetected) {
    graph_builder build;
    const auto slot = build.declare();
    build.define_reference(slot, parse("@"));
    const auto root = value::list({ value::graph(build.publish(slot)) });
    failure(error_code::serialization_cycle, [&] { serialize_json(root); });
}

TEST(
    GraphJson, FailedSerializationDoesNotPublishPartialOutputOrPoisonLaterCalls
) {
    const auto root = graph("{value:42,self:@}");
    std::string output = "previous successful result";
    failure(error_code::serialization_cycle, [&] {
        output = serialize_json(value::graph(root));
    });
    EXPECT_EQ(output, "previous successful result");
    EXPECT_EQ(to_int64(read(root, "$.self.value")), 42);
    EXPECT_EQ(serialize_json(read(root, "$.value")), "42");
    failure(error_code::serialization_cycle, [&] {
        serialize_json(value::graph(root));
    });
}

TEST(GraphJson, ExplicitRootFrameControlsSubgraphRecipeResolution) {
    const auto root = graph("{value:42,child:{copy:$.value}}");
    const auto child = read(root, "$.child");
    EXPECT_EQ(
        serialize_json(child, { value::graph(root), {} }), "{\"copy\":42}"
    );
    failure(error_code::missing_key, [&] { serialize_json(child); });
}

TEST(GraphJson, ExactNumbersRequireExplicitNonintegerExportConversion) {
    EXPECT_EQ(
        serialize_json(numeric_literal("1e40")),
        "10000000000000000000000000000000000000000"
    );
    for (const auto* text : { "0.5", "1/3" }) {
        const auto scalar = evaluate(parse(text)).output;
        failure(error_code::conversion, [&] { serialize_json(scalar); });
        EXPECT_FALSE(serialize_json(to_real(scalar)).empty());
    }
    failure(error_code::conversion, [] {
        serialize_json(value::builtin(builtin_id::size));
    });
}

TEST(GraphJson, Binary64OutputRoundTripsSubnormalsExtremaAndSignedZero) {
    for (const double real :
         { 0.0, -0.0, 0.1, std::numeric_limits<double>::denorm_min(),
           std::numeric_limits<double>::min(),
           std::numeric_limits<double>::max() }) {
        const auto text = serialize_json(value(real));
        double parsed = 0;
        const auto [end, code]
            = std::from_chars(text.data(), text.data() + text.size(), parsed);
        EXPECT_EQ(code, std::errc());
        EXPECT_EQ(end, text.data() + text.size());
        EXPECT_EQ(
            std::bit_cast<uint64_t>(parsed), std::bit_cast<uint64_t>(real)
        );
    }
}

TEST(GraphJson, StringsEscapeControlsAndPreserveValidUtf8InKeysAndValues) {
    const std::string text = "a\"\\\n\r\t\b\f" + std::string(1, '\0') + "é€😀";
    const auto json = serialize_json(value::object({ { text, value(text) } }));
    const auto escaped = "\"a\\\"\\\\\\n\\r\\t\\b\\f\\u0000é€😀\"";
    EXPECT_EQ(json, "{" + std::string(escaped) + ":" + escaped + "}");
    EXPECT_EQ(evaluate(parse(json)).output.entries()[0].first, text);
    EXPECT_EQ(
        evaluate(parse(json)).output.entries()[0].second.as_string(), text
    );
}

TEST(GraphJson, InvalidUtf8NeverProducesInvalidJson) {
    for (const auto& text :
         { std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
           std::string("\xf4\x90\x80\x80"), std::string("\x80"),
           std::string("\xe2\x82"), std::string("\xe2x\xac") }) {
        failure(error_code::conversion, [&] { serialize_json(value(text)); });
        failure(error_code::conversion, [&] {
            serialize_json(value::object({ { text, value() } }));
        });
    }
}

TEST(GraphJson, OutputAndExpandedDagWorkAreBoundedIndependently) {
    serialization_limits options;
    options.output_bytes = 3;
    EXPECT_EQ(
        serialize_json(value("x"), {}, standard_environment(), options), "\"x\""
    );
    options.output_bytes = 2;
    failure(error_code::resource, [&] {
        serialize_json(value("x"), {}, standard_environment(), options);
    });
    options.output_bytes = 0;
    failure(error_code::resource, [&] {
        serialize_json(value(), {}, standard_environment(), options);
    });
    options.output_bytes = 16777217;
    EXPECT_THROW(
        serialize_json(value(), {}, standard_environment(), options),
        std::invalid_argument
    );
    graph_builder build;
    auto previous = build.declare();
    build.define_scalar(previous, value::integer(1));
    for (size_t i = 0; i < 20; ++i) {
        const auto next = build.declare();
        build.define_list(next, { previous, previous });
        previous = next;
    }
    const auto dag = value::graph(build.publish(previous));
    options = {};
    options.execution.steps = 200;
    failure(error_code::resource, [&] {
        serialize_json(dag, {}, standard_environment(), options);
    });
    options = {};
    options.execution.elements = 100;
    failure(error_code::resource, [&] {
        serialize_json(dag, {}, standard_environment(), options);
    });
}

TEST(GraphJson, ReferenceEvaluationSharesSerializationStepBudget) {
    const auto root = value::graph(graph("{a:1,b:$.a,c:$.a,d:$.a}"));
    serialization_limits options;
    options.execution.steps = 10;
    failure(error_code::resource, [&] {
        serialize_json(root, {}, standard_environment(), options);
    });
    options.execution.steps = 100;
    EXPECT_EQ(
        serialize_json(root, {}, standard_environment(), options),
        "{\"a\":1,\"b\":1,\"c\":1,\"d\":1}"
    );
}

TEST(GraphJson, DeepGraphsUseExplicitFramesAndEnforceDepthLimit) {
    graph_builder build;
    auto previous = build.declare();
    build.define_scalar(previous, value());
    for (size_t i = 0; i < 300; ++i) {
        const auto next = build.declare();
        build.define_list(next, { previous });
        previous = next;
    }
    const auto root = value::graph(build.publish(previous));
    failure(error_code::resource, [&] { serialize_json(root); });
    serialization_limits options;
    options.execution.depth = 301;
    EXPECT_EQ(
        serialize_json(root, {}, standard_environment(), options).size(), 604U
    );
    options.execution.depth = 0;
    failure(error_code::resource, [&] {
        serialize_json(value(), {}, standard_environment(), options);
    });
}

TEST(GraphIo, EmbeddedExecutionCannotRestartTheEnclosingDepthBudget) {
    limits options;
    options.depth = 2;
    failure(error_code::resource, [&] {
        graph_from_expression(parse("{a:1+2}"), {}, options);
    });
    options.depth = 3;
    EXPECT_EQ(
        to_int64(
            read(graph_from_expression(parse("{a:1+2}"), {}, options), "$.a")
        ),
        3
    );
    const auto root = value::graph(graph("{a:{value:1,alias:@.value}}"));
    serialization_limits output;
    output.execution.depth = 4;
    failure(error_code::resource, [&] {
        serialize_json(root, {}, standard_environment(), output);
    });
    output.execution.depth = 5;
    EXPECT_EQ(
        serialize_json(root, {}, standard_environment(), output),
        "{\"a\":{\"value\":1,\"alias\":1}}"
    );
}

TEST(GraphJson, ForeignOwnersRemainDistinctAndConcurrentSerializationIsStable) {
    const auto a = graph("{value:1,alias:@.value}"),
               b = graph("{value:2,alias:@.value}");
    const auto root
        = value::list({ value::graph(a), value::graph(b), value::graph(a) });
    const auto expected = serialize_json(root);
    EXPECT_EQ(
        expected,
        "[{\"value\":1,\"alias\":1},{\"value\":2,\"alias\":2},{\"value\":1,"
        "\"alias\":1}]"
    );
    std::vector<std::future<std::string>> results;
    for (size_t i = 0; i < 8; ++i)
        results.push_back(std::async(std::launch::async, [root] {
            return serialize_json(root);
        }));
    for (auto& result : results)
        EXPECT_EQ(result.get(), expected);
}
