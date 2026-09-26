#include "evaluate.hpp"
#include "graph.hpp"
#include "grouper.hpp"
#include "reduce.hpp"
#include <gtest/gtest.h>

#include <future>
#include <sstream>

namespace {
using namespace runtime;

normalized::expression_ptr parse(std::string text) {
    auto source = source::from_memory(std::move(text), "reference.qc");
    return normalized::normalize_expression(
        source_span(source, 0, source->size())
    );
}

std::string dump(const normalized::expression_ptr& expression) {
    std::ostringstream output;
    normalized::dump(output, expression);
    return output.str();
}

graph_handle fixture() {
    graph_builder build;
    const auto root = build.declare(), left = build.declare(),
               right = build.declare(), local = build.declare(),
               self = build.declare(), one = build.declare(),
               two = build.declare(), list = build.declare(),
               absolute = build.declare();
    build.define_object(
        root,
        { { "left", left },
          { "right", right },
          { "list", list },
          { "self", self } }
    );
    build.define_object(
        left,
        { { "value", one },
          { "local", local },
          { "self", self },
          { "root", absolute } }
    );
    build.define_object(
        right, { { "value", two }, { "local", local }, { "self", self } }
    );
    build.define_reference(local, parse("@.value"));
    build.define_reference(self, parse("@"));
    build.define_reference(absolute, parse("$"));
    build.define_scalar(one, value::integer(1));
    build.define_scalar(two, value::integer(2));
    build.define_list(list, { left, right, left });
    return build.publish(root);
}

evaluation
execute(std::string text, const graph_handle& root, limits options = {}) {
    const auto value = value::graph(root);
    return evaluate_in(
        parse(std::move(text)), { value, value }, standard_environment(),
        options
    );
}

template <class Action> error failure(error_code code, Action action) {
    try {
        action();
        ADD_FAILURE() << "expected reference failure";
    } catch (const error& result) {
        EXPECT_EQ(result.code(), code);
        return result;
    }
    throw std::runtime_error("reference test did not fail");
}
}

TEST(References, RootAndCurrentSyntaxHaveDistinctSourceMappedNodes) {
    EXPECT_EQ(
        dump(parse("$.a[@.index]")),
        "(index (member (root) \"a\") (member (current) \"index\"))\n"
    );
    EXPECT_EQ(evaluate(parse("'$'")).output.as_string(), "$");
    EXPECT_EQ(evaluate(parse("'@'")).output.as_string(), "@");
    const auto tree = parse("  @");
    EXPECT_EQ(tree->span.begin().offset, 2);
    EXPECT_EQ(tree->span.end().offset, 3);
    size_t children = 0;
    normalized::for_each_child(*tree, [&](const auto&) { ++children; });
    EXPECT_EQ(children, 0U);
}

TEST(
    References, ContextBindingsAreSeparateFromLexicalNamesAndRequiredExplicitly
) {
    const auto root = fixture();
    const auto value = value::graph(root);
    environment bindings { { "root", value::integer(9) },
                           { "$", value::integer(100) } };
    EXPECT_EQ(
        to_int64(
            evaluate_in(parse("root+$.left.value"), { value, {} }, bindings)
                .output
        ),
        10
    );
    failure(error_code::invalid_reference, [&] {
        evaluate(parse("$"), bindings);
    });
    failure(error_code::invalid_reference, [&] {
        evaluate_in(parse("@"), { value, {} }, bindings);
    });
    EXPECT_EQ(
        evaluate_in(parse("$"), { runtime::value(), {} }, bindings)
            .output.kind(),
        value_kind::null
    );
}

TEST(References, FiniteSelfPathsAndMutualGraphEdgesExecute) {
    const auto root = fixture();
    EXPECT_EQ(to_int64(execute("$.left.self.self.value", root).output), 1);
    EXPECT_EQ(
        to_int64(execute("$.left.root.right.self.value", root).output), 2
    );
    graph_builder build;
    const auto a = build.declare(), b = build.declare(),
               number = build.declare();
    build.define_object(a, { { "b", b } });
    build.define_object(b, { { "a", a }, { "value", number } });
    build.define_scalar(number, value::integer(42));
    const auto mutual = build.publish(a);
    EXPECT_EQ(to_int64(execute("$.b.a.b.a.b.value", mutual).output), 42);
}

TEST(References, SharedRecipeUsesItsActualParentWithoutRewritingGraph) {
    const auto root = fixture();
    const auto local = root.member("left").member("local");
    EXPECT_TRUE(local.same_identity(root.member("right").member("local")));
    const auto before = dump(local.recipe());
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(to_int64(execute("$.left.local", root).output), 1);
        EXPECT_EQ(to_int64(execute("$.right.local", root).output), 2);
    }
    EXPECT_EQ(dump(local.recipe()), before);
}

TEST(References, DynamicIndicesUseSelectedBaseAsCurrentAndRestoreOuterFrame) {
    EXPECT_EQ(to_int64(evaluate(parse("[100,50,25,0][@[3]]")).output), 100);
    const auto root = graph_from_value(
        value::list(
            { value::integer(10), value::integer(20), value::integer(0) }
        )
    );
    EXPECT_EQ(to_int64(execute("$[@[2]]", root).output), 10);
    EXPECT_EQ(to_int64(execute("[8,9][0]+@[1]", root).output), 28);
    EXPECT_EQ(to_int64(execute("$[-1]", root).output), 0);
}

TEST(References, SelectionPreservesOrderDuplicatesAndGraphIdentity) {
    const auto root = fixture();
    const auto selected = execute("$.list{2,0,1}", root).output;
    ASSERT_EQ(selected.elements().size(), 3U);
    EXPECT_TRUE(selected.elements()[0].as_graph().same_identity(
        selected.elements()[1].as_graph()
    ));
    EXPECT_FALSE(selected.elements()[0].as_graph().same_identity(
        selected.elements()[2].as_graph()
    ));
    const auto members = execute("$.(right,left,right)", root).output;
    EXPECT_TRUE(members.elements()[0].as_graph().same_identity(
        members.elements()[2].as_graph()
    ));
    EXPECT_EQ(to_int64(execute("$.list{2,0,1}[2].local", root).output), 2);
    EXPECT_EQ(to_int64(evaluate(parse("{a:1,b:2}.(b,a,b)[2]")).output), 2);
    EXPECT_EQ(to_int64(evaluate(parse("[1,2]{1,0}[0]")).output), 2);
}

TEST(References, SelectorsExecuteSequentiallyAndFailureSkipsRemainingKeys) {
    const auto root = fixture();
    std::vector<std::string> trace;
    const resolver bindings
        = [&](std::string_view key) -> std::optional<value> {
        trace.emplace_back(key);
        if (key == "first")
            return value::integer(0);
        if (key == "bad")
            return value::integer(100);
        return value::integer(1);
    };
    failure(error_code::index_bounds, [&] {
        evaluate_with_context(
            parse("$.list{first,bad,never}"), { value::graph(root), {} },
            bindings
        );
    });
    EXPECT_EQ(trace, (std::vector<std::string> { "first", "bad" }));
}

TEST(References, NoProgressReferenceLoopsAreDistinctFromFiniteCyclicPaths) {
    graph_builder build;
    const auto root = build.declare(), a = build.declare(), b = build.declare();
    build.define_object(root, { { "a", a }, { "b", b } });
    build.define_reference(a, parse("$.b"));
    build.define_reference(b, parse("$.a"));
    const auto graph = build.publish(root);
    const auto error
        = failure(error_code::reference_cycle, [&] { execute("$.a", graph); });
    ASSERT_TRUE(error.location());
    EXPECT_EQ(error.location()->text(), "$.b");
    failure(error_code::reference_cycle, [&] { execute("$.a", graph); });
    EXPECT_TRUE(execute("$", graph).output.as_graph().same_identity(graph));
}

TEST(References, ReentrantRecipeInDifferentParentCanMakeProgress) {
    graph_builder build;
    const auto a = build.declare(), b = build.declare(), c = build.declare(),
               ref = build.declare(), number = build.declare();
    build.define_object(a, { { "next", b }, { "value", ref } });
    build.define_object(b, { { "next", c }, { "value", ref } });
    build.define_object(c, { { "value", number } });
    build.define_reference(ref, parse("@.next.value"));
    build.define_scalar(number, value::integer(42));
    EXPECT_EQ(to_int64(execute("$.value", build.publish(a)).output), 42);
}

TEST(References, GraphBuiltinsResolveListItemsInContext) {
    const auto root = graph_from_value(
        value::object(
            { { "list",
                value::list(
                    { value::integer(4), numeric_literal("0.5"),
                      value::integer(2) }
                ) } }
        )
    );
    EXPECT_EQ(to_int64(execute("size($)+len($.list)", root).output), 4);
    EXPECT_EQ(execute("min($.list)", root).output.as_number().str(), "1/2");
    EXPECT_EQ(to_int64(execute("max($.list)", root).output), 4);
    graph_builder build;
    const auto list = build.declare(), number = build.declare(),
               ref = build.declare();
    build.define_list(list, { number, ref });
    build.define_scalar(number, value::integer(3));
    build.define_reference(ref, parse("@[0]"));
    EXPECT_EQ(to_int64(execute("max($)", build.publish(list)).output), 3);
}

TEST(References, ErrorsKeepCheckedIndexRulesAndNearestSourceLocation) {
    const auto root = fixture();
    for (const auto& [text, code] :
         std::vector<std::pair<std::string, error_code>> {
             { "$.list[3]", error_code::index_bounds },
             { "$.list[-4]", error_code::index_bounds },
             { "$.list[0.5]", error_code::type },
             { "$.list[true]", error_code::type },
             { "$[0]", error_code::type },
             { "$.absent", error_code::missing_key },
             { "$.list.(value)", error_code::type } }) {
        SCOPED_TRACE(text);
        const auto result = failure(code, [&] { execute(text, root); });
        ASSERT_TRUE(result.location());
        EXPECT_EQ(result.location()->text(), text);
    }
}

TEST(References, RuntimeBudgetsBoundResolutionSelectionsAndGraphScalars) {
    const auto root = fixture();
    const auto normal = execute("$.left.self.self.value", root);
    limits options;
    options.steps = normal.steps - 1;
    failure(error_code::resource, [&] {
        execute("$.left.self.self.value", root, options);
    });
    options.steps = normal.steps;
    EXPECT_EQ(
        to_int64(execute("$.left.self.self.value", root, options).output), 1
    );
    options = {};
    options.elements = 1;
    failure(error_code::resource, [&] {
        execute("$.list{0,1}", root, options);
    });
    options = {};
    options.depth = 2;
    failure(error_code::resource, [&] {
        execute("$.left.local", root, options);
    });
    options = {};
    options.numbers.bits = 1;
    failure(error_code::resource, [&] {
        execute("$.right.value", root, options);
    });
    const auto text = graph_from_value(value("abc"));
    options = {};
    options.string_bytes = 2;
    failure(error_code::resource, [&] { execute("$", text, options); });
}

TEST(References, SharedExternalBudgetAlsoBoundsHostRecipeResolution) {
    const auto root = fixture();
    const resolver missing
        = [](std::string_view) -> std::optional<value> { return {}; };
    const auto ref = value::graph(root.member("left").member("local"));
    budget work;
    EXPECT_EQ(
        to_int64(resolve_references(
            ref, { value::graph(root), value::graph(root.member("left")) },
            missing, work
        )),
        1
    );
    const auto before = work.steps_used();
    resolve_references(
        ref, { value::graph(root), value::graph(root.member("right")) },
        missing, work
    );
    EXPECT_EQ(work.steps_used(), before * 2);
}

TEST(References, RecipeValidationBudgetsAndPublicationRemainTransactional) {
    graph_limits options;
    options.recipe_nodes = 2;
    graph_builder build(options);
    const auto ref = build.declare();
    failure(error_code::invalid_reference, [&] {
        build.define_reference(ref, parse("x.value"));
    });
    failure(error_code::invalid_reference, [&] {
        build.define_reference(ref, {});
    });
    failure(error_code::resource, [&] {
        build.define_reference(ref, parse("$.a.b"));
    });
    build.define_reference(ref, parse("$.a"));
    const auto graph = build.publish(ref);
    EXPECT_EQ(dump(graph.recipe()), "(member (root) \"a\")\n");
    options.recipe_nodes = 1;
    failure(error_code::resource, [&] { clone_graph(graph, options); });
    budget work;
    EXPECT_EQ(walk_graph(graph, work).size(), 1U);
}

TEST(References, SourceAndFrameDestructionDoNotInvalidateGraphResultsOrErrors) {
    value retained;
    std::weak_ptr<const void> owner;
    {
        const auto root = fixture();
        owner = root.lifetime();
        retained = execute("$.left.self", root).output;
    }
    EXPECT_FALSE(owner.expired());
    EXPECT_EQ(to_int64(retained.as_graph().member("value").scalar()), 1);
    retained = {};
    EXPECT_TRUE(owner.expired());
    const auto error
        = failure(error_code::invalid_reference, [] { evaluate(parse("@")); });
    EXPECT_EQ(error.location()->owner()->name(), "reference.qc");
    EXPECT_EQ(error.location()->text(), "@");
}

TEST(References, ReducerKeepsContextualReadsResidualAndPreservesExecution) {
    const auto root = fixture();
    for (const auto* text : { "$.left.local+(2+3)", "$.list[1+1].value",
                              "true?$.right.value:missing" }) {
        const auto tree = parse(text), reduced = reduction::reduce(tree).tree;
        const reference_frame frame { value::graph(root), value::graph(root) };
        EXPECT_EQ(
            evaluate_in(tree, frame).output.as_number().str(),
            evaluate_in(reduced, frame).output.as_number().str()
        );
        EXPECT_EQ(reduction::reduce(reduced).tree, reduced);
    }
    const auto root_node = parse("$");
    EXPECT_EQ(reduction::reduce(root_node).kind, reduction::state::residual);
}

TEST(References, EagerAndLazySourceAdaptersPreserveReferenceSyntax) {
    const auto root = fixture();
    for (const size_t limit : { size_t(8), size_t(1024) }) {
        group_ptr tree;
        {
            reader input(
                source::from_memory("min([$.left.value,$.right.value]);")
            );
            grouper parser(input, limit);
            tree = parser.parse();
        }
        std::ostringstream compact;
        tree->dump(compact, false);
        if (limit == 8) {
            EXPECT_NE(compact.str().find("Placeholder"), std::string::npos);
        }
        const auto program = normalized::normalize_program(*tree);
        const auto& first
            = std::get<normalized::block>(program->value).statements.front();
        const auto expression
            = std::get<normalized::expression_statement>(first->value).value;
        EXPECT_EQ(
            to_int64(
                evaluate_in(expression, { value::graph(root), {} }).output
            ),
            1
        );
    }
}

TEST(References, LongAcyclicRecipeChainsRespectTheHardDepthCeiling) {
    graph_builder build;
    const auto ref = build.declare(), number = build.declare();
    build.define_reference(ref, parse("@.next.value"));
    build.define_scalar(number, value::integer(42));
    auto previous = build.declare();
    build.define_object(previous, { { "value", number } });
    for (size_t i = 0; i < 520; ++i) {
        const auto next = build.declare();
        build.define_object(next, { { "next", previous }, { "value", ref } });
        previous = next;
    }
    const auto root = build.publish(previous);
    limits options;
    options.depth = 512;
    failure(error_code::resource, [&] { execute("$.value", root, options); });
    auto shorter = root;
    for (size_t i = 0; i < 220; ++i)
        shorter = shorter.member("next");
    EXPECT_EQ(to_int64(execute("$.value", shorter, options).output), 42);
}

TEST(References, ConcurrentResolutionAndCloneUseIndependentContextState) {
    const auto root = fixture();
    std::vector<std::future<int64_t>> results;
    for (size_t i = 0; i < 8; ++i)
        results.push_back(std::async(std::launch::async, [root, i] {
            const auto copy = clone_graph(root);
            return to_int64(
                execute(i % 2 ? "$.left.local" : "$.right.local", copy).output
            );
        }));
    for (size_t i = 0; i < results.size(); ++i)
        EXPECT_EQ(results[i].get(), i % 2 ? 1 : 2);
}
