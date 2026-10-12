#include "grouper.hpp"
#include "normalized.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>

namespace {
group_ptr grouped(const std::string& text, size_t limit) {
    reader input(source::from_memory(text, "groups.qc"));
    grouper parser(input, limit);
    return parser.parse();
}

template <class Ptr> std::string dump(const Ptr& value) {
    std::ostringstream output;
    normalized::dump(output, value);
    return output.str();
}

void find_groups(const group_ptr& group, std::vector<group_ptr>& result) {
    result.push_back(group);
    for (const auto& child : group->nodes) {
        if (const auto sub = std::dynamic_pointer_cast<group_node>(child))
            find_groups(sub, result);
    }
}
}

TEST(NormalizedIntegration, EagerAndDeferredProgramsNormalizeIdentically) {
    const std::string text = "{[a,b,c,d],[e,f,g,h],[i,j,k,l]}";
    const auto eager = grouped(text, 1024);
    const auto lazy = grouped(text, 4);
    std::ostringstream before, after;
    lazy->dump(before, false);
    const auto result = normalized::normalize_program(*lazy);
    lazy->dump(after, false);
    EXPECT_EQ(before.str(), after.str());
    EXPECT_NE(before.str().find("Placeholder"), std::string::npos);
    EXPECT_EQ(dump(result), dump(normalized::normalize_program(*eager)));
}

TEST(NormalizedIntegration, TargetedPlaceholderUsesOnlyItsBoundedSource) {
    const auto tree = grouped("{[a,b,c,d],[e,f,g,h],[i,j,k,l]}", 4);
    std::vector<group_ptr> groups;
    find_groups(tree, groups);
    size_t count = 0;
    for (const auto& group : groups) {
        const auto lazy = std::dynamic_pointer_cast<placeholder_node>(group);
        if (!lazy)
            continue;
        const auto restored = lazy->materialize();
        const auto expected = dump(normalized::normalize_expression(*restored));
        lazy->limit = 1; // legacy materialization would now fail
        EXPECT_THROW(
            static_cast<void>(lazy->materialize()), std::runtime_error
        );
        EXPECT_EQ(dump(normalized::normalize_expression(*lazy)), expected);
        ++count;
    }
    EXPECT_GT(count, 0u);
}

TEST(NormalizedIntegration, GroupBoundaryTrimmingPreservesOwnedParentheses) {
    for (const std::string text : { "(a)", "(a);", "[a,b]", "[a,b];" }) {
        const auto tree = grouped(text, 1024);
        const auto command
            = std::dynamic_pointer_cast<group_node>(tree->nodes.front());
        ASSERT_NE(command, nullptr);
        const auto value = normalized::normalize_expression(*command);
        EXPECT_EQ(
            value->span.text(),
            text.back() == ';' ? text.substr(0, text.size() - 1) : text
        );
    }
    const auto tree = grouped("{[a,b],[c,d]}", 1024);
    std::vector<group_ptr> groups;
    find_groups(tree, groups);
    for (const auto& group : groups) {
        if (group->kind == group_kind::list) {
            EXPECT_EQ(
                normalized::normalize_expression(*group)->span.text().front(),
                '['
            );
        }
    }
}

TEST(NormalizedIntegration, MergedChainSpanRestoresEveryStatement) {
    const auto tree = grouped("{if(a)b;else c}", 1024);
    std::vector<group_ptr> groups;
    find_groups(tree, groups);
    bool found = false;
    for (const auto& group : groups) {
        if (group->is_chain) {
            const auto value = normalized::normalize_program(*group);
            EXPECT_NE(dump(value).find("(if "), std::string::npos);
            EXPECT_NE(dump(value).find("(id \"c\")"), std::string::npos);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(NormalizedIntegration, MissingSourceFailsExplicitly) {
    group_node node;
    EXPECT_THROW(
        static_cast<void>(normalized::normalize_expression(node)),
        std::invalid_argument
    );
    EXPECT_THROW(
        static_cast<void>(normalized::normalize_program(node)),
        std::invalid_argument
    );
}

TEST(NormalizedIntegration, DumpsEscapeControlBytesWithoutChangingTheTree) {
    const auto input = source::from_memory("return 'a\\nb\\t\\u0000';");
    const source_span span(input, 0, input->size());
    const auto value = normalized::normalize_program(span);
    const auto before = dump(value);
    std::ostringstream tokens;
    normalized::dump_tokens(tokens, span);
    EXPECT_NE(before.find("a\\nb\\t\\u0000"), std::string::npos);
    EXPECT_NE(tokens.str().find("a\\nb\\t\\u0000"), std::string::npos);
    EXPECT_EQ(std::count(before.begin(), before.end(), '\n'), 1);
    EXPECT_EQ(dump(value), before);
}

TEST(NormalizedIntegration, TokensExposeAdjacentCompoundOperatorsAndLocations) {
    const auto input = source::from_memory("\n a+=b; a + + b;");
    std::ostringstream tokens;
    normalized::dump_tokens(tokens, source_span(input, 0, input->size()));
    EXPECT_NE(tokens.str().find("3:5 <1:2> \"+=\""), std::string::npos);
    EXPECT_NE(tokens.str().find("10:11 <1:9> \"+\""), std::string::npos);
    EXPECT_NE(tokens.str().find("12:13 <1:11> \"+\""), std::string::npos);
}
