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

#include "grouper.hpp"
#include <gtest/gtest.h>

#include <iomanip>
#include <limits>
#include <sstream>

class AstExamples : public testing::TestWithParam<int> { };

TEST_P(AstExamples, DumpsAreRepeatableAndPreserveReaderPosition) {
    std::ostringstream name;
    name << "test" << std::setfill('0') << std::setw(2) << GetParam() << ".qc";
    const auto path = std::filesystem::path(QPILER_TEST_DATA_DIR) / name.str();
    reader r(path);
    grouper g { r, GetParam() == 12 ? 128u : 64u };
    const auto tree = g.parse();
    ASSERT_NE(tree, nullptr);
    ASSERT_FALSE(tree->empty());
    const auto position = r.get_position();
    const auto fixed_size = tree->fixed_size;
    const auto full_size = tree->full_size;
    std::ostringstream compact, full, repeated, compact_after;
    tree->dump(compact, false);
    tree->dump(full, true);
    tree->dump(repeated, true);
    tree->dump(compact_after, false);
    EXPECT_FALSE(compact.str().empty());
    EXPECT_FALSE(full.str().empty());
    EXPECT_EQ(full.str(), repeated.str());
    EXPECT_EQ(compact.str(), compact_after.str());
    EXPECT_EQ(tree->fixed_size, fixed_size);
    EXPECT_EQ(tree->full_size, full_size);
    EXPECT_EQ(r.get_position().offset, position.offset);
    EXPECT_EQ(r.get_position().line, position.line);
    EXPECT_EQ(r.get_position().column, position.column);

    reader eager_reader(path);
    grouper eager { eager_reader, std::numeric_limits<size_t>::max() };
    std::ostringstream eager_dump;
    eager.parse()->dump(eager_dump, true);
    EXPECT_EQ(full.str(), eager_dump.str());
}

INSTANTIATE_TEST_SUITE_P(Frontend, AstExamples, testing::Range(0, 13));

TEST_P(AstExamples, DeferredTreesSurviveParserScope) {
    std::ostringstream name;
    name << "test" << std::setfill('0') << std::setw(2) << GetParam() << ".qc";
    const auto path = std::filesystem::path(QPILER_TEST_DATA_DIR) / name.str();
    const auto parse = [&path](size_t limit) {
        reader input(path);
        grouper parser { input, limit };
        return parser.parse();
    };
    const auto tree = parse(GetParam() == 12 ? 128u : 64u);
    const auto eager = parse(std::numeric_limits<size_t>::max());
    std::ostringstream actual, expected;
    tree->dump(actual, true);
    eager->dump(expected, true);
    EXPECT_EQ(actual.str(), expected.str());
}
