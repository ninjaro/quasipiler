#include "graph.hpp"
#include <gtest/gtest.h>

#include <future>

namespace {
using namespace runtime;

graph_handle cyclic_graph() {
    graph_builder build;
    const auto a = build.declare(), b = build.declare(),
               number = build.declare();
    build.define_object(
        a, { { "self", a }, { "other", b }, { "value", number } }
    );
    build.define_list(b, { a, b, number });
    build.define_scalar(number, value::integer(42));
    return build.publish(a);
}

template <class Action> void fails(error_code code, Action action) {
    try {
        action();
        FAIL() << "expected graph failure";
    } catch (const error& failure) {
        EXPECT_EQ(failure.code(), code);
    }
}
}

TEST(Graph, SelfAndMutualEdgesPreserveIdentityAndFiniteAccess) {
    const auto root = cyclic_graph();
    EXPECT_TRUE(root.same_identity(root.member("self")));
    EXPECT_TRUE(root.same_identity(root.member("other").at(0)));
    EXPECT_TRUE(root.member("other").same_identity(root.member("other").at(1)));
    EXPECT_EQ(
        to_int64(root.member("self").member("self").member("value").scalar()),
        42
    );
    EXPECT_EQ(root.id(), 0U);
    EXPECT_EQ(root.member("other").id(), 1U);
    EXPECT_EQ(root.member("value").id(), 2U);
    EXPECT_EQ(root.key_at(0), "self");
}

TEST(Graph, LastExternalOwnerReclaimsCyclesAndSlotsDoNotRetainArena) {
    std::weak_ptr<const void> lifetime;
    graph_handle survivor;
    graph_slot slot;
    {
        graph_builder build;
        slot = build.declare();
        build.define_list(slot, { slot });
        const auto root = build.publish(slot);
        lifetime = root.lifetime();
        survivor = root.at(0);
    }
    EXPECT_FALSE(lifetime.expired());
    EXPECT_TRUE(survivor.same_identity(survivor.at(0)));
    survivor = {};
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(slot.id(), 0U);
}

TEST(Graph, ForwardDeclarationsMustAllBeDefinedBeforePublication) {
    graph_builder build;
    const auto root = build.declare(), child = build.declare();
    build.define_list(root, { child });
    fails(error_code::invalid_reference, [&] { build.publish(root); });
    build.define_scalar(child, value(true));
    EXPECT_TRUE(build.publish(root).at(0).scalar().as_bool());
    fails(error_code::invalid_reference, [&] { build.declare(); });
    fails(error_code::invalid_reference, [&] { build.publish(root); });
}

TEST(Graph, ForeignEmptyAndAlreadyDefinedSlotsFailWithoutChangingBuilder) {
    graph_builder build, other;
    const auto a = build.declare(), foreign = other.declare();
    fails(error_code::invalid_reference, [&] {
        build.define_list(a, { foreign });
    });
    fails(error_code::invalid_reference, [&] {
        build.define_list(a, { graph_slot() });
    });
    fails(error_code::invalid_reference, [&] {
        build.define_scalar(foreign, value());
    });
    build.define_list(a, { a });
    fails(error_code::invalid_reference, [&] {
        build.define_scalar(a, value());
    });
    EXPECT_TRUE(build.publish(a).at(0).valid());
}

TEST(Graph, MovedBuilderRetainsSlotScope) {
    graph_builder original;
    const auto slot = original.declare();
    graph_builder moved(std::move(original));
    fails(error_code::invalid_reference, [&] { original.declare(); });
    moved.define_scalar(slot, value("text"));
    EXPECT_EQ(moved.publish(slot).scalar().as_string(), "text");
}

TEST(Graph, PayloadBoundaryForbidsHiddenOwningGraphAndCollectionEdges) {
    const auto old = cyclic_graph();
    for (const auto& payload :
         { value::graph(old), value::list({ value::graph(old) }),
           value::object({}), value::builtin(builtin_id::size) }) {
        graph_builder build;
        const auto slot = build.declare();
        fails(error_code::type, [&] { build.define_scalar(slot, payload); });
        build.define_scalar(slot, value());
        EXPECT_EQ(build.publish(slot).scalar().kind(), value_kind::null);
    }
}

TEST(Graph, LimitsAreCheckedBeforePublicationAndFailedDefinitionsCanRetry) {
    graph_limits options;
    options.nodes = 1;
    options.edges = 1;
    options.string_bytes = 1;
    graph_builder build(options);
    const auto slot = build.declare();
    fails(error_code::resource, [&] { build.declare(); });
    fails(error_code::resource, [&] {
        build.define_list(slot, { slot, slot });
    });
    fails(error_code::resource, [&] {
        build.define_scalar(slot, value("xx"));
    });
    fails(error_code::resource, [&] {
        build.define_object(slot, { { "xx", slot } });
    });
    build.define_object(slot, { { "x", slot } });
    EXPECT_TRUE(build.publish(slot).member("x").valid());
    options.nodes = 1000001;
    EXPECT_THROW(
        static_cast<void>(graph_builder(options)), std::invalid_argument
    );
    options.nodes = 0;
    graph_builder empty(options);
    fails(error_code::resource, [&] { empty.declare(); });
}

TEST(Graph, DuplicateKeysAndTotalStringBudgetAreTransactional) {
    graph_limits options;
    options.string_bytes = 4;
    graph_builder build(options);
    const auto a = build.declare(), b = build.declare();
    build.define_scalar(b, value("ab"));
    fails(error_code::duplicate_key, [&] {
        build.define_object(a, { { "x", b }, { "x", b } });
    });
    fails(error_code::resource, [&] {
        build.define_object(a, { { "xyz", b } });
    });
    build.define_object(a, { { "xy", b } });
    EXPECT_EQ(build.publish(a).member("xy").scalar().as_string(), "ab");
}

TEST(Graph, NumberLimitAppliesToConstructionAndCloning) {
    graph_limits options;
    options.numbers.bits = 8;
    graph_builder build(options);
    const auto slot = build.declare();
    fails(error_code::resource, [&] {
        build.define_scalar(slot, value::integer(256));
    });
    build.define_scalar(slot, numeric_literal("0.5"));
    EXPECT_EQ(build.publish(slot).scalar().as_number().str(), "1/2");
    const auto large = graph_from_value(value::integer(256));
    fails(error_code::resource, [&] { clone_graph(large, options); });
}

TEST(Graph, AccessErrorsAreStructuredAndEmptyHandlesHaveNoIdentity) {
    const auto root = cyclic_graph();
    fails(error_code::missing_key, [&] { root.member("missing"); });
    fails(error_code::type, [&] { root.at(0); });
    fails(error_code::type, [&] { root.scalar(); });
    fails(error_code::index_bounds, [&] { root.child_at(3); });
    fails(error_code::index_bounds, [&] { root.member("other").at(3); });
    fails(error_code::invalid_reference, [] { graph_handle().kind(); });
    fails(error_code::invalid_reference, [] { value::graph({}); });
    EXPECT_FALSE(graph_handle().same_identity({}));
}

TEST(Graph, CloneCreatesNewOwnerPreservingAllAliasesAndDeterministicLocalIds) {
    const auto original = cyclic_graph();
    const auto copy = clone_graph(original);
    EXPECT_FALSE(original.same_identity(copy));
    EXPECT_EQ(original.id(), copy.id());
    EXPECT_TRUE(copy.same_identity(copy.member("other").at(0)));
    EXPECT_EQ(copy.node_count(), 3U);
    graph_limits options;
    options.nodes = 2;
    fails(error_code::resource, [&] { clone_graph(original, options); });
    const auto subcopy = clone_graph(original.member("other"));
    EXPECT_EQ(subcopy.id(), 1U);
    EXPECT_TRUE(subcopy.same_identity(subcopy.at(1)));
}

TEST(Graph, ValueImportDoesNotInventObservableIdentityFromStorageSharing) {
    const auto child = value::list({ value::integer(1), value("x") });
    const auto root
        = graph_from_value(value::object({ { "a", child }, { "b", child } }));
    EXPECT_FALSE(root.member("a").same_identity(root.member("b")));
    EXPECT_EQ(root.member("b").at(1).scalar().as_string(), "x");
    fails(error_code::type, [&] { graph_from_value(value::graph(root)); });
}

TEST(Graph, WalkIsCycleAwareDeterministicAndInvocationBounded) {
    const auto root = cyclic_graph();
    budget first, second;
    const auto a = walk_graph(root, first), b = walk_graph(root, second);
    ASSERT_EQ(a.size(), 3U);
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].id(), i);
        EXPECT_TRUE(a[i].same_identity(b[i]));
    }
    EXPECT_EQ(first.steps_used(), 7U);
    EXPECT_EQ(first.elements_used(), 3U);
    limits options;
    options.steps = 6;
    budget short_work(options);
    fails(error_code::resource, [&] { walk_graph(root, short_work); });
    options.steps = 100;
    options.elements = 2;
    budget short_output(options);
    fails(error_code::resource, [&] { walk_graph(root, short_output); });
}

TEST(Graph, LargeCycleUsesIterativeTraversalAndFlatDestruction) {
    graph_builder build;
    std::vector<graph_slot> nodes;
    for (size_t i = 0; i < 2000; ++i)
        nodes.push_back(build.declare());
    for (size_t i = 0; i < nodes.size(); ++i)
        build.define_list(nodes[i], { nodes[(i + 1) % nodes.size()] });
    const auto root = build.publish(nodes[0]);
    budget work;
    EXPECT_EQ(walk_graph(root, work).size(), 2000U);
}

TEST(Graph, ConcurrentReadsAndClonesShareNoMutableTraversalState) {
    const auto root = cyclic_graph();
    std::vector<std::future<size_t>> readers;
    for (size_t i = 0; i < 8; ++i)
        readers.push_back(std::async(std::launch::async, [root] {
            budget work;
            const auto copy = clone_graph(root);
            return walk_graph(copy, work).size() + root.member("other").size();
        }));
    for (auto& reader : readers)
        EXPECT_EQ(reader.get(), 6U);
}

TEST(Graph, RuntimeHandlesOwnArenaAndRetainStrictCollectionEquality) {
    auto root = cyclic_graph();
    const auto lifetime = root.lifetime();
    auto stored = value::graph(root);
    root = {};
    auto moved = std::move(stored);
    EXPECT_EQ(stored.kind(), value_kind::null);
    EXPECT_FALSE(lifetime.expired());
    fails(error_code::unsupported_operation, [&] {
        binary("==", moved, moved);
    });
    moved = {};
    EXPECT_TRUE(lifetime.expired());
}
