#include "reader.hpp"
#include "source.hpp"
#include "test_source.hpp"
#include "token.hpp"
#include <gtest/gtest.h>

#include <future>
#include <sstream>

TEST(SourceTest, OwnsMemoryAndPreservesAbsolutePositions) {
    std::string text = "one\n\ntwo";
    const auto input = source::from_memory(text, "sample.qc");
    text.assign("changed");
    EXPECT_EQ(input->text(), "one\n\ntwo");
    EXPECT_EQ(input->name(), "sample.qc");
    EXPECT_EQ(input->position_at(0), (position { 0, 0, 0 }));
    EXPECT_EQ(input->position_at(4), (position { 4, 1, 0 }));
    EXPECT_EQ(input->position_at(5), (position { 5, 2, 0 }));
    EXPECT_EQ(input->position_at(8), (position { 8, 2, 3 }));
    EXPECT_THROW(input->position_at(-1), std::out_of_range);
    EXPECT_THROW(input->position_at(9), std::out_of_range);
}

TEST(SourceTest, EmptySourceAndTrailingNewlineHaveValidEndPositions) {
    const auto empty = source::from_memory("");
    EXPECT_EQ(empty->position_at(0), (position { 0, 0, 0 }));
    reader r(empty);
    token t;
    r.next_token(t);
    EXPECT_EQ(t.kind, token_kind::eof);
    EXPECT_EQ(t.pos, (position { 0, 0, 0 }));
    const auto input = source::from_memory("a\n");
    EXPECT_EQ(input->position_at(input->size()), (position { 2, 1, 0 }));
}

TEST(SourceTest, ValidatesSpanBoundsAndOwnership) {
    const auto input = source::from_memory("abc");
    EXPECT_THROW((source_span { nullptr, 0, 0 }), std::invalid_argument);
    EXPECT_THROW((source_span { input, -1, 0 }), std::out_of_range);
    EXPECT_THROW((source_span { input, 2, 1 }), std::out_of_range);
    EXPECT_THROW((source_span { input, 0, 4 }), std::out_of_range);
    EXPECT_THROW((reader { source_ptr {} }), std::invalid_argument);
    const source_span end(input, 3, 3);
    EXPECT_TRUE(end.text().empty());
    EXPECT_EQ(end.begin(), end.end());
}

TEST(SourceTest, SpanRetainsSourceAfterCreatorScope) {
    const auto span = [] {
        auto input = source::from_memory("before\nvalue after");
        return source_span(input, 7, 12);
    }();
    reader r(span);
    token t;
    r.next_token(t);
    EXPECT_EQ(t.word, "value");
    EXPECT_EQ(t.pos, (position { 7, 1, 0 }));
    r.next_token(t);
    EXPECT_EQ(t.kind, token_kind::eof);
    EXPECT_EQ(t.pos, (position { 12, 1, 5 }));
}

TEST(SourceTest, ReaderCannotEscapeSpanAndFailedJumpPreservesState) {
    const auto input = source::from_memory("first\nsecond\nthird");
    reader r(source_span(input, 6, 12));
    const auto original = r.get_position();
    EXPECT_THROW(r.jump_to_position(input->position_at(0)), std::runtime_error);
    EXPECT_THROW(
        r.jump_to_position(input->position_at(13)), std::runtime_error
    );
    EXPECT_THROW(r.jump_to_position({ 6, 0, 6 }), std::runtime_error);
    EXPECT_EQ(r.get_position(), original);
    EXPECT_THROW(
        r.span(input->position_at(0), input->position_at(12)), std::out_of_range
    );
    EXPECT_THROW(
        r.span({ 6, 0, 6 }, input->position_at(12)), std::invalid_argument
    );
    EXPECT_EQ(r.span(original, input->position_at(12)).text(), "second");
    token t;
    r.next_token(t);
    EXPECT_EQ(t.word, "second");
    r.next_token(t);
    EXPECT_EQ(t.kind, token_kind::eof);
    r.jump_to_position(original);
    r.next_token(t);
    EXPECT_EQ(t.word, "second");
}

TEST(SourceTest, TokenizationCannotUseBytesPastSpanEnd) {
    const auto input = source::from_memory("\"value\" 12345 /*comment*/");
    reader truncated_string(source_span(input, 0, 5));
    token t;
    EXPECT_THROW(truncated_string.next_token(t), std::runtime_error);
    reader number(source_span(input, 8, 11));
    number.next_token(t);
    EXPECT_EQ(t.word, "123");
    number.next_token(t);
    EXPECT_EQ(t.kind, token_kind::eof);
    reader comment(source_span(input, 14, 18));
    EXPECT_THROW(comment.next_token(t), std::runtime_error);
}

TEST(SourceTest, FileSnapshotSurvivesReplacementAndRemoval) {
    test_source_file file("before\nvalue");
    const auto input = source::from_file(file.path, 1);
    file.write("replacement");
    std::filesystem::remove(file.path);
    EXPECT_EQ(input->text(), "before\nvalue");
    EXPECT_EQ(input->name(), file.path.string());
    reader r(source_span(input, 7, input->size()));
    token t;
    r.next_token(t);
    EXPECT_EQ(t.word, "value");
    EXPECT_EQ(t.pos, (position { 7, 1, 0 }));
}

TEST(SourceTest, FileReadChunkSizesDoNotChangeTokens) {
    const std::string text
        = "first\n/*line\ncomment*/\"a\\n\\u1234\" 36.6e+2 last";
    test_source_file file(text);
    for (const std::streamsize chunk : { 1, 2, 7, 4096 }) {
        SCOPED_TRACE(chunk);
        reader from_file(file.path, chunk);
        reader from_memory(source::from_memory(text));
        token actual, expected;
        do {
            from_file.next_token(actual);
            from_memory.next_token(expected);
            EXPECT_EQ(actual.kind, expected.kind);
            EXPECT_EQ(actual.word, expected.word);
            EXPECT_EQ(actual.pos, expected.pos);
        } while (expected.kind != token_kind::eof);
        from_file.jump_to_position({ 0, 0, 0 });
        from_file.next_token(actual);
        EXPECT_EQ(actual.word, "first");
    }
}

TEST(SourceTest, RejectsMissingFilesAndNonpositiveReadChunks) {
    test_source_file file("");
    EXPECT_THROW(source::from_file(file.path, 0), std::invalid_argument);
    EXPECT_THROW(source::from_file(file.path, -1), std::invalid_argument);
    EXPECT_EQ(source::from_file(file.path, 1)->size(), 0);
    std::filesystem::remove(file.path);
    EXPECT_THROW(source::from_file(file.path), std::invalid_argument);
}

TEST(SourceTest, SeparateReadersOverSharedSpansAreIndependent) {
    const auto input = source::from_memory("first\nsecond\nthird");
    std::vector<std::future<std::string>> results;
    for (int i = 0; i < 8; ++i) {
        results.push_back(std::async(std::launch::async, [input, i] {
            const source_span span = i % 2 == 0 ? source_span(input, 0, 5)
                                                : source_span(input, 6, 12);
            reader r(span);
            std::ostringstream output;
            for (int repeat = 0; repeat < 20; ++repeat) {
                r.jump_to_position(span.begin());
                token t;
                r.next_token(t);
                output << t.word << ':' << t.pos.line << ';';
            }
            return output.str();
        }));
    }
    for (size_t i = 0; i < results.size(); ++i) {
        std::string expected;
        for (int repeat = 0; repeat < 20; ++repeat) {
            expected += i % 2 == 0 ? "first:0;" : "second:1;";
        }
        EXPECT_EQ(results[i].get(), expected);
    }
}

TEST(SourceTest, DiagnosticsRetainSourceNameAndAbsoluteLocation) {
    const auto input = source::from_memory("prefix\n\"bad", "broken.qc");
    reader r(source_span(input, 7, input->size()));
    token t;
    try {
        r.next_token(t);
        FAIL() << "unterminated string accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("broken.qc:2:5"), std::string::npos
        );
    }
}
