#include "grouper.hpp"
#include "test_source.hpp"
#include <gtest/gtest.h>

#include <functional>
#include <future>
#include <limits>
#include <sstream>

namespace {
group_ptr parse_text(const std::string& text, size_t limit = 4) {
    reader r(source::from_memory(text, "lazy.qc"));
    grouper g { r, limit };
    return g.parse();
}

std::string dump(const ast_node_ptr& node, bool full = true) {
    std::ostringstream output;
    node->dump(output, full);
    return output.str();
}

void visit(
    const ast_node_ptr& node,
    const std::function<void(const ast_node_ptr&)>& action
) {
    if (!node)
        return;
    action(node);
    if (const auto group = std::dynamic_pointer_cast<group_node>(node)) {
        for (const auto& child : group->nodes)
            visit(child, action);
    } else if (
        const auto call = std::dynamic_pointer_cast<callexp_node>(node)
    ) {
        visit(call->paren, action);
        if (const auto function
            = std::dynamic_pointer_cast<fundecl_node>(node)) {
            visit(function->body, action);
        }
    } else if (
        const auto indirect = std::dynamic_pointer_cast<imcallexp_node>(node)
    ) {
        visit(indirect->callee, action);
        visit(indirect->paren, action);
    } else if (
        const auto control = std::dynamic_pointer_cast<control_node>(node)
    ) {
        visit(control->body, action);
        if (const auto condition
            = std::dynamic_pointer_cast<condition_node>(node)) {
            visit(condition->paren, action);
        }
    } else if (const auto unary = std::dynamic_pointer_cast<unary_node>(node)) {
        visit(unary->expr, action);
    } else if (
        const auto binary = std::dynamic_pointer_cast<binary_node>(node)
    ) {
        visit(binary->lhs, action);
        visit(binary->rhs, action);
    } else if (
        const auto ternary = std::dynamic_pointer_cast<ternary_node>(node)
    ) {
        visit(ternary->cond, action);
        visit(ternary->left, action);
        visit(ternary->right, action);
    }
}

std::vector<placeholder_node_ptr> placeholders(const ast_node_ptr& tree) {
    std::vector<placeholder_node_ptr> found;
    visit(tree, [&](const ast_node_ptr& node) {
        if (const auto lazy
            = std::dynamic_pointer_cast<placeholder_node>(node)) {
            found.push_back(lazy);
        }
    });
    return found;
}
}

TEST(LazyTest, TreeOutlivesReaderAndOriginalMemory) {
    const std::string text = "{[a,b,c,d],[e,f,g,h],[i,j,k,l]}";
    const auto tree = parse_text(text);
    ASSERT_FALSE(placeholders(tree).empty());
    EXPECT_EQ(dump(tree), dump(parse_text(text, 1024)));
    EXPECT_NE(dump(tree, false).find("Placeholder"), std::string::npos);
}

TEST(LazyTest, FileTreeOutlivesReaderAndRemovedFile) {
    const std::string text = "{[a,b,c,d],[e,f,g,h],[i,j,k,l]}";
    group_ptr tree;
    {
        test_source_file file(text);
        reader r(file.path, 2);
        grouper g { r, 4 };
        tree = g.parse();
        file.write("different bytes");
    }
    ASSERT_FALSE(placeholders(tree).empty());
    EXPECT_EQ(dump(tree), dump(parse_text(text, 1024)));
}

TEST(LazyTest, MaterializationIsFreshAndDoesNotMutatePlaceholder) {
    const auto tree = parse_text("{[a,b,c,d],[e,f,g,h],[i,j,k,l]}");
    const auto deferred = placeholders(tree);
    ASSERT_FALSE(deferred.empty());
    EXPECT_FALSE(deferred.front()->empty());
    EXPECT_EQ(deferred.front()->first(), deferred.front().get());
    const auto before = dump(tree, false);
    for (const auto& lazy : deferred) {
        const auto first = lazy->materialize();
        const auto second = lazy->materialize();
        EXPECT_NE(first, second);
        EXPECT_EQ(first->kind, lazy->kind);
        EXPECT_EQ(dump(first), dump(second));
        EXPECT_EQ(first->get_start(), lazy->get_start());
        first->nodes.clear();
        EXPECT_EQ(dump(lazy->materialize()), dump(second));
    }
    EXPECT_EQ(dump(tree, false), before);
}

TEST(LazyTest, ParallelMaterializationOfSameAndSeparateSpans) {
    const auto tree = parse_text("{[a,b,c,d],[e,f,g,h],[i,j,k,l]}");
    const auto expected = dump(tree);
    std::vector<std::future<std::string>> results;
    for (int i = 0; i < 8; ++i) {
        results.push_back(std::async(std::launch::async, [tree] {
            std::string output;
            for (int repeat = 0; repeat < 10; ++repeat)
                output = dump(tree);
            return output;
        }));
    }
    for (auto& result : results)
        EXPECT_EQ(result.get(), expected);
}

TEST(LazyTest, NestedPlaceholdersRestoreLargeStructures) {
    std::string text = "a";
    for (int depth = 0; depth < 48; ++depth) {
        text = "[" + text + ",[b,c,d,e],[f,g,h,i]]";
    }
    const auto tree = parse_text(text, 8);
    const auto deferred = placeholders(tree);
    ASSERT_FALSE(deferred.empty());
    bool nested = false;
    for (const auto& lazy : deferred) {
        nested = nested || !placeholders(lazy->materialize()).empty();
    }
    EXPECT_TRUE(nested);
    EXPECT_EQ(dump(tree), dump(parse_text(text, 100000)));
    EXPECT_LE(tree->fixed_size, 8u);
}

TEST(LazyTest, PreservesWrappedAndTokenSourceLocations) {
    const std::string text = "\n  [a,b,c];\n  [d,e,f];";
    const auto tree = parse_text(text, 1024);
    ASSERT_EQ(tree->nodes.size(), 3u);
    const auto command = std::dynamic_pointer_cast<group_node>(tree->nodes[0]);
    ASSERT_NE(command, nullptr);
    const auto wrapped
        = std::dynamic_pointer_cast<wrapped_node>(command->nodes[0]);
    ASSERT_NE(wrapped, nullptr);
    EXPECT_EQ(wrapped->get_start(), (position { 3, 1, 2 }));
    ASSERT_TRUE(wrapped->span);
    EXPECT_EQ(wrapped->span->text(), "a,b,c]");
    command->squeeze(0);
    const auto lazy
        = std::dynamic_pointer_cast<placeholder_node>(command->nodes[0]);
    ASSERT_NE(lazy, nullptr);
    EXPECT_EQ(lazy->get_start(), (position { 3, 1, 2 }));
    const auto restored = lazy->materialize();
    EXPECT_EQ(restored->get_start(), lazy->get_start());
    EXPECT_NE(dump(restored).find("<1:3>(\"a\")"), std::string::npos);
    EXPECT_EQ(dump(restored), dump(wrapped));
}

TEST(LazyTest, CompactDumpDoesNotMaterializeMalformedPlaceholder) {
    auto lazy = std::make_shared<placeholder_node>();
    lazy->kind = group_kind::list;
    std::ostringstream compact;
    EXPECT_NO_THROW(lazy->dump(compact, false));
    EXPECT_NE(compact.str().find("Placeholder(list)"), std::string::npos);
    EXPECT_THROW(static_cast<void>(lazy->materialize()), std::runtime_error);
    const auto input = source::from_memory("a,b");
    lazy->span = source_span(input, 0, input->size());
    EXPECT_NO_THROW(dump(lazy, false));
    EXPECT_THROW(dump(lazy), std::runtime_error);
}

TEST(LazyTest, RejectsUnusedBytesAndCannotReadClosingBracketOutsideSpan) {
    auto lazy = std::make_shared<placeholder_node>();
    const auto input = source::from_memory("a,b,c]");
    lazy->kind = group_kind::item;
    lazy->span = source_span(input, 0, input->size());
    EXPECT_THROW(static_cast<void>(lazy->materialize()), std::runtime_error);
    lazy->kind = group_kind::list;
    lazy->span = source_span(input, 0, input->size() - 1);
    EXPECT_THROW(static_cast<void>(lazy->materialize()), std::runtime_error);
    lazy->span = source_span(input, 0, input->size());
    EXPECT_NO_THROW(static_cast<void>(lazy->materialize()));
}

TEST(LazyTest, FailureDoesNotChangeAnExistingReader) {
    const auto input = source::from_memory("a,b,c]");
    reader original(input);
    token t;
    original.next_token(t);
    const auto before = original.get_position();
    placeholder_node lazy;
    lazy.kind = group_kind::list;
    lazy.span = source_span(input, 0, input->size() - 1);
    EXPECT_THROW(static_cast<void>(lazy.materialize()), std::runtime_error);
    EXPECT_EQ(original.get_position(), before);
    original.next_token(t);
    EXPECT_EQ(t.word, ",");
}

TEST(LazyTest, SourceIsReleasedWithLastTreeAndSpan) {
    std::weak_ptr<const source> lifetime;
    group_ptr tree;
    {
        auto input = source::from_memory("{[a,b,c,d],[e,f,g,h]}");
        lifetime = input;
        reader r(input);
        grouper g { r, 4 };
        tree = g.parse();
    }
    EXPECT_FALSE(lifetime.expired());
    EXPECT_NO_THROW(dump(tree));
    tree.reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST(LazyTest, SqueezeAndPopMaintainWeightAccounting) {
    const auto tree = parse_text("[a,b,c];[d,e,f];", 1024);
    const auto original_full = tree->full_size;
    const auto original_fixed = tree->fixed_size;
    const auto child_weight = tree->nodes.front()->fixed_size;
    tree->squeeze(0);
    tree->squeeze(0);
    EXPECT_EQ(tree->fixed_size, original_fixed - child_weight + 1);
    EXPECT_EQ(tree->full_size, original_full);
    tree->pop_back();
    tree->pop_back();
    tree->pop_back();
    EXPECT_TRUE(tree->empty());
    EXPECT_EQ(tree->fixed_size, 1u);
    EXPECT_EQ(tree->full_size, 1u);
    EXPECT_TRUE(tree->weights.empty());
    EXPECT_THROW(tree->pop_back(), std::runtime_error);
    EXPECT_THROW(tree->squeeze(0), std::out_of_range);
}

TEST(LazyTest, SqueezedControlChainsRestoreTheWholeSequence) {
    for (const std::string text : { "if(a)b;else c;", "if(a)b;elif(c)d;else e;",
                                    "try{a;}catch(e)b;finally c;" }) {
        SCOPED_TRACE(text);
        const auto tree = parse_text(text, 1024);
        const auto expected = dump(tree);
        tree->squeeze(0);
        EXPECT_EQ(dump(tree), expected);
    }
}

TEST(LazyTest, ControlChainInsideBodyRestoresItsTerminatingBoundary) {
    for (const std::string text : { "{if(a)b;else c}", "{if(a)b;else c;}",
                                    "{try{a;}catch(e)b;finally c}" }) {
        SCOPED_TRACE(text);
        const auto tree = parse_text(text, 1024);
        const auto expected = dump(tree);
        group_ptr body;
        visit(tree, [&](const ast_node_ptr& node) {
            const auto group = std::dynamic_pointer_cast<group_node>(node);
            if (!body && group && group->kind == group_kind::body)
                body = group;
        });
        ASSERT_NE(body, nullptr);
        body->squeeze(0);
        EXPECT_EQ(dump(tree), expected);
    }
}

TEST(LazyTest, ControlChainsRemainEquivalentUnderSmallLimits) {
    for (const std::string text : { "if(a)b;else c;", "if(a)b;elif(c)d;else e;",
                                    "try{a;}catch(e)b;finally c;" }) {
        SCOPED_TRACE(text);
        const auto expected = dump(parse_text(text, 1024));
        for (const size_t limit : { 12u, 16u, 24u }) {
            SCOPED_TRACE(limit);
            EXPECT_EQ(dump(parse_text(text, limit)), expected);
        }
    }
}
