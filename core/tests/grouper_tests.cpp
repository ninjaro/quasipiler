/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Yaroslav Riabtsev <yaroslav.riabtsev@rwth-aachen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <gtest/gtest.h>

#include "grouper.hpp"

TEST(GrouperTest, ParsesSimpleBody) {
    std::string input = "{a;b}";
    reader r { input };
    grouper g { r };

    auto res = g.parse();
    ASSERT_EQ(res->kind, group_kind::file);
    ASSERT_EQ(res->size(), 1u);

    auto* halt = dynamic_cast<group_node*>(res->nodes[0].get());
    ASSERT_NE(halt, nullptr);
    EXPECT_EQ(halt->kind, group_kind::halt);
    ASSERT_EQ(halt->size(), 1u);

    auto* body = dynamic_cast<group_node*>(halt->nodes[0].get());
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->kind, group_kind::body);
    ASSERT_EQ(body->size(), 2u);

    auto* cmd = dynamic_cast<group_node*>(body->nodes[0].get());
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->kind, group_kind::command);
    auto* a = dynamic_cast<token_node*>(cmd->nodes[0].get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->value.word, "a");

    auto* trailing = dynamic_cast<group_node*>(body->nodes[1].get());
    ASSERT_NE(trailing, nullptr);
    auto* b = dynamic_cast<token_node*>(trailing->nodes[0].get());
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->value.word, "b");
}

TEST(GrouperTest, ParsesNestedListBody) {
    std::string input = "[a,{b;c}]";
    reader r { input };
    grouper g { r };

    auto res = g.parse();
    ASSERT_EQ(res->kind, group_kind::file);
    ASSERT_EQ(res->size(), 1u);

    auto* list = dynamic_cast<const group_node*>(res->get());
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->kind, group_kind::list);
    ASSERT_EQ(list->size(), 2u);

    auto* item = dynamic_cast<group_node*>(list->nodes[0].get());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, group_kind::item);
    auto* a = dynamic_cast<token_node*>(item->nodes[0].get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->value.word, "a");

    auto* next = dynamic_cast<group_node*>(list->nodes[1].get());
    ASSERT_NE(next, nullptr);
    auto* body = dynamic_cast<group_node*>(next->nodes[0].get());
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->kind, group_kind::body);
}

TEST(GrouperTest, MissingClosingThrows) {
    std::string input = "[a";
    reader r { input };
    grouper g { r };
    EXPECT_THROW(g.parse(), std::runtime_error);
}

TEST(GrouperTest, ConstructorEnforcesLimit) {
    std::string input = "a";
    reader r { input };
    EXPECT_THROW(grouper(r, 1);, std::runtime_error);
}

TEST(GrouperTest, LimitTooSmallThrows) {
    for (auto [str, lim] : std::vector<std::pair<std::string, size_t>> {
             { "{a;[b,c,d];e}", 2 },
             { "a,b,c,d,e,f", 5 },
             { "{[a,a,a,a,a],[b,b,b,b]}", 4 } }) {
        {
            reader r { str };
            grouper g { r, lim };
            EXPECT_THROW(g.parse(), std::runtime_error);
        }
        {
            reader r { str };
            grouper g { r, lim + 1 };
            EXPECT_NO_THROW(g.parse());
        }
    }
}

TEST(GrouperChainTest, ErrorScenarios) {
    struct Case {
        std::string input;
        std::string msg;
    } cases[] = {
        { "else a", "orphan secondary keyword" },
        { "a,else b", "invalid predecessor for keyword" },
        { "a;else b", "invalid predecessor for keyword" },
        { "try{b};else{c}", "unexpected keyword order" },
    };

    for (const auto& c : cases) {
        auto input = c.input;
        reader r { input };
        grouper g { r };
        try {
            g.parse();
            FAIL() << "no exception";
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(c.msg), std::string::npos)
                << c.input;
        }
    }
}

TEST(GrouperIdentifyTest, MissingCondition) {
    std::string input = "if{a}";
    reader r { input };
    grouper g { r };
    EXPECT_THROW({ g.parse(); }, std::runtime_error);
}

TEST(GrouperPlaceholderTest, PreservesPlaceholders) {
    std::string input = "{[a,b,c,d],[e,f,g,h],[i,j,k,l]}";
    reader r { input };
    grouper g { r, 4 };

    auto res = g.parse();
    auto* halt = dynamic_cast<group_node*>(res->nodes[0].get());
    ASSERT_NE(halt, nullptr);
    auto* body = dynamic_cast<group_node*>(halt->nodes[0].get());
    ASSERT_NE(body, nullptr);

    bool has_placeholder = false;
    for (const auto& ch : body->nodes) {
        if (std::dynamic_pointer_cast<placeholder_node>(ch)) {
            has_placeholder = true;
            break;
        }
    }
    EXPECT_TRUE(has_placeholder);
}

TEST(GrouperTest, RejectsMismatchedAndUnexpectedBrackets) {
    for (std::string input : { "[a}", "(a]", "{a)", "]", "}", ")" }) {
        SCOPED_TRACE(input);
        reader r { input };
        grouper g { r };
        EXPECT_THROW(g.parse(), std::runtime_error);
    }
}

TEST(GrouperPlaceholderTest, FullDumpMatchesEagerAndPreservesSizes) {
    std::string input = "{[a,b,c,d],[e,f,g,h],[i,j,k,l]}";
    reader r { input };
    grouper lazy { r, 4 };
    const auto tree = lazy.parse();
    EXPECT_LE(tree->fixed_size, 4u);
    EXPECT_GT(tree->full_size, tree->fixed_size);
    std::ostringstream compact, full;
    tree->dump(compact, false);
    EXPECT_NE(compact.str().find("Placeholder"), std::string::npos);
    tree->dump(full, true);
    EXPECT_EQ(full.str().find("Placeholder"), std::string::npos);
    reader other { input };
    grouper eager { other, 1024 };
    std::ostringstream expected;
    eager.parse()->dump(expected, true);
    EXPECT_EQ(full.str(), expected.str());
}
