#include "runtime.hpp"
#include <gtest/gtest.h>

#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>

namespace {
using runtime::numeric_literal;
using runtime::value;

uint64_t bits(double data) { return std::bit_cast<uint64_t>(data); }

value number(const char* text) { return numeric_literal(text); }

template <class Action> void fails(runtime::error_code code, Action action) {
    try {
        action();
        FAIL() << "expected runtime failure";
    } catch (const runtime::error& failure) {
        EXPECT_EQ(failure.code(), code) << failure.what();
        EXPECT_FALSE(failure.location());
    }
}

struct rounding_guard {
    int saved = std::fegetround();

    ~rounding_guard() { std::fesetround(saved); }
};

struct conversion_case {
    std::string spelling;
    uint64_t encoding;
};

class ExactRealConversion : public testing::TestWithParam<conversion_case> { };
}

TEST_P(ExactRealConversion, RoundsRationalDirectlyToNearestEven) {
    const auto exact = numeric_literal(GetParam().spelling).as_number();
    const auto converted = exact.as_real();
    ASSERT_TRUE(converted);
    EXPECT_EQ(bits(*converted), GetParam().encoding);
}

INSTANTIATE_TEST_SUITE_P(
    Binary64, ExactRealConversion,
    testing::Values(
        conversion_case { "0.1", 0x3fb999999999999aULL },
        conversion_case { "-0.1", 0xbfb999999999999aULL },
        conversion_case { "9007199254740993", 0x4340000000000000ULL },
        conversion_case { "9007199254740995", 0x4340000000000002ULL },
        conversion_case {
            "1.00000000000000011102230246251565404236316680908203125",
            0x3ff0000000000000ULL },
        conversion_case {
            "1.00000000000000033306690738754696212708950042724609375",
            0x3ff0000000000002ULL },
        conversion_case { "1e-1000", 0 },
        conversion_case { "-1e-1000", 0x8000000000000000ULL }
    )
);

TEST(RuntimeValues, ScalarKindsRemainDistinctAndConversionsAreExplicit) {
    EXPECT_EQ(value().kind(), runtime::value_kind::null);
    EXPECT_EQ(value(true).kind(), runtime::value_kind::boolean);
    EXPECT_EQ(number("1").kind(), runtime::value_kind::integer);
    EXPECT_EQ(number("0.1").kind(), runtime::value_kind::rational);
    EXPECT_EQ(value(0.1).kind(), runtime::value_kind::real);
    EXPECT_EQ(value("text").kind(), runtime::value_kind::string);
    EXPECT_EQ(
        runtime::to_exact(value(0.1)).as_number().str(),
        "3602879701896397/36028797018963968"
    );
    EXPECT_EQ(runtime::to_integer(value(2.0)).as_number().str(), "2");
    EXPECT_EQ(bits(runtime::to_real(number("0.1")).as_real()), bits(0.1));
    fails(runtime::error_code::conversion, [] {
        runtime::to_integer(value(0.5));
    });
    fails(runtime::error_code::type, [] { runtime::to_real(value(true)); });
    fails(runtime::error_code::type, [] {
        runtime::binary("+", number("1"), value(1.0));
    });
    fails(runtime::error_code::conversion, [] {
        value(std::numeric_limits<double>::infinity());
    });
    fails(runtime::error_code::conversion, [] {
        value(std::numeric_limits<double>::quiet_NaN());
    });
    EXPECT_EQ(
        exact::from_real(std::numeric_limits<double>::infinity()).error,
        exact::failure::conversion
    );
    EXPECT_EQ(
        exact::from_real(std::numeric_limits<double>::quiet_NaN()).error,
        exact::failure::conversion
    );
}

TEST(RuntimeValues, FiniteBinary64EncodingsRoundTripThroughExactRationals) {
    uint64_t state = 0x1928374655ULL;
    for (int i = 0; i < 1500; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        if (((state >> 52U) & 0x7ffU) == 0x7ffU)
            continue;
        const auto converted = exact::from_real(std::bit_cast<double>(state));
        ASSERT_TRUE(converted.value);
        ASSERT_TRUE(converted.value->as_real());
        EXPECT_EQ(bits(*converted.value->as_real()), state);
    }
    for (const uint64_t encoding :
         { 1ULL, 2ULL, 0xfffffffffffffULL, 0x10000000000000ULL,
           0x7fefffffffffffffULL, 0x8000000000000001ULL }) {
        const auto exact = exact::from_real(std::bit_cast<double>(encoding));
        ASSERT_TRUE(exact.value);
        EXPECT_EQ(bits(*exact.value->as_real()), encoding);
    }
}

TEST(RuntimeValues, SubnormalTiesAndSignedZeroHaveDefinedBoundaries) {
    const auto smallest
        = runtime::to_exact(value(std::numeric_limits<double>::denorm_min()));
    const auto half = runtime::binary("/", smallest, number("2"));
    EXPECT_EQ(bits(runtime::to_real(half).as_real()), 0U);
    EXPECT_EQ(
        bits(runtime::to_real(runtime::unary("-", half)).as_real()),
        0x8000000000000000ULL
    );
    const auto tie = runtime::binary("*", half, number("3"));
    EXPECT_EQ(bits(runtime::to_real(tie).as_real()), 2U);
    EXPECT_EQ(bits(value(-0.0).as_real()), 0x8000000000000000ULL);
    EXPECT_EQ(runtime::to_exact(value(-0.0)).as_number().str(), "0");
    EXPECT_TRUE(runtime::binary("==", value(-0.0), value(0.0)).as_bool());
    EXPECT_EQ(
        bits(runtime::unary("-", value(0.0)).as_real()), 0x8000000000000000ULL
    );
    EXPECT_EQ(
        bits(
            runtime::binary(
                "*", value(std::numeric_limits<double>::denorm_min()),
                value(1.0)
            )
                .as_real()
        ),
        1U
    );
    fails(runtime::error_code::division_by_zero, [] {
        runtime::binary("/", value(1.0), value(-0.0));
    });
    EXPECT_EQ(
        exact::from_real(
            std::numeric_limits<double>::denorm_min(), { 1074, 100 }
        )
            .error,
        exact::failure::growth
    );
    EXPECT_TRUE(
        exact::from_real(
            std::numeric_limits<double>::denorm_min(), { 1075, 100 }
        )
            .value
    );
}

TEST(
    RuntimeValues,
    FiniteOverflowAndLargeRationalConversionsDoNotUseIntermediateFloats
) {
    const auto maximum
        = runtime::to_exact(value(std::numeric_limits<double>::max()));
    const auto quarter_ulp = runtime::to_exact(value(std::ldexp(1.0, 969)));
    const auto half_ulp = runtime::to_exact(value(std::ldexp(1.0, 970)));
    EXPECT_EQ(
        bits(
            runtime::to_real(runtime::binary("+", maximum, quarter_ulp))
                .as_real()
        ),
        bits(std::numeric_limits<double>::max())
    );
    fails(runtime::error_code::conversion, [&] {
        runtime::to_real(runtime::binary("+", maximum, half_ulp));
    });
    fails(runtime::error_code::conversion, [] {
        runtime::to_real(number("1e1000"));
    });
    fails(runtime::error_code::conversion, [] {
        runtime::binary(
            "*", value(std::numeric_limits<double>::max()), value(2.0)
        );
    });
    const auto huge = number("1e1000");
    const auto ratio = runtime::binary(
        "/", runtime::binary("+", huge, number("1")),
        runtime::binary("-", huge, number("1"))
    );
    EXPECT_EQ(bits(runtime::to_real(ratio).as_real()), bits(1.0));
}

TEST(
    RuntimeValues,
    ExactConversionIgnoresRoundingModeAndRealArithmeticRejectsUnsupportedModes
) {
    const rounding_guard restore;
    for (const int mode : { FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO }) {
        ASSERT_EQ(std::fesetround(mode), 0);
        EXPECT_EQ(
            bits(runtime::to_real(number("0.1")).as_real()),
            0x3fb999999999999aULL
        );
        fails(runtime::error_code::numeric_environment, [] {
            runtime::binary("+", value(1.0), value(2.0));
        });
        EXPECT_EQ(std::fegetround(), mode);
    }
}

TEST(RuntimeValues, ExactAndMachineDomainsKeepDifferentRoundingSemantics) {
    const auto exact = runtime::binary(
        "-", runtime::binary("+", number("9007199254740992"), number("1")),
        number("9007199254740992")
    );
    const auto real = runtime::binary(
        "-", runtime::binary("+", value(9007199254740992.0), value(1.0)),
        value(9007199254740992.0)
    );
    EXPECT_EQ(exact.as_number().str(), "1");
    EXPECT_EQ(bits(real.as_real()), 0U);
    EXPECT_EQ(
        runtime::binary("*", number("36.6"), number("1.2")).as_number().str(),
        "1098/25"
    );
    EXPECT_TRUE(
        runtime::binary(
            "==", number("0.5"), runtime::binary("/", number("1"), number("2"))
        )
            .as_bool()
    );
    EXPECT_TRUE(runtime::binary("<", number("-0.1"), number("0")).as_bool());
    EXPECT_TRUE(runtime::unary("!", value(false)).as_bool());
    EXPECT_TRUE(runtime::binary("==", value(), value()).as_bool());
    EXPECT_TRUE(runtime::binary("!=", value("a"), value("b")).as_bool());
    fails(runtime::error_code::type, [] {
        runtime::binary("==", value(), value(false));
    });
    fails(runtime::error_code::type, [] {
        runtime::binary("+", value("a"), value("b"));
    });
    fails(runtime::error_code::type, [] { runtime::unary("!", number("0")); });
    fails(runtime::error_code::unsupported_operation, [] {
        runtime::binary("%", number("1"), number("2"));
    });
    fails(runtime::error_code::unsupported_operation, [] {
        runtime::unary("~", number("1"));
    });
    fails(runtime::error_code::division_by_zero, [] {
        runtime::binary("/", number("1"), number("0"));
    });
}

TEST(RuntimeValues, CheckedIntegerCastsAndIndicesNeverNarrowBeforeValidation) {
    EXPECT_EQ(
        runtime::to_int64(number("9223372036854775807")),
        std::numeric_limits<int64_t>::max()
    );
    EXPECT_EQ(
        runtime::to_int64(number("-9223372036854775808")),
        std::numeric_limits<int64_t>::min()
    );
    fails(runtime::error_code::conversion, [] {
        runtime::to_int64(number("9223372036854775808"));
    });
    fails(runtime::error_code::conversion, [] {
        runtime::to_int64(number("-9223372036854775809"));
    });
    const auto list
        = value::list({ value("first"), value("middle"), value("last") });
    EXPECT_EQ(runtime::access(list, number("0")).as_string(), "first");
    EXPECT_EQ(runtime::access(list, number("-1")).as_string(), "last");
    EXPECT_EQ(runtime::access(list, number("-3")).as_string(), "first");
    for (const auto* index :
         { "3", "-4", "-9223372036854775808", "1e100", "-1e100" })
        fails(runtime::error_code::index_bounds, [&] {
            runtime::access(list, number(index));
        });
    fails(runtime::error_code::index_bounds, [] {
        runtime::access(value::list({}), number("-1"));
    });
    fails(runtime::error_code::type, [&] {
        runtime::access(list, number("0.5"));
    });
    fails(runtime::error_code::type, [&] {
        runtime::access(list, value(1.0));
    });
    fails(runtime::error_code::type, [&] {
        runtime::access(number("1"), list);
    });
}

TEST(RuntimeValues, CollectionsPreserveOrderAndOutliveTheirConstructionInputs) {
    const auto make = [] {
        std::vector<value> items { value("owned") };
        auto list = value::list(items);
        items[0] = value("changed");
        return value::object(
            { { "z", list },
              { "a", value(true) },
              { std::string("x\0y", 3), value("nul key") } }
        );
    };
    auto temporary = make();
    const auto object = std::move(temporary);
    EXPECT_EQ(temporary.kind(), runtime::value_kind::null);
    EXPECT_EQ(object.entries()[0].first, "z");
    EXPECT_EQ(object.entries()[1].first, "a");
    EXPECT_EQ(object.size(), 3U);
    const auto selected = runtime::access(object, value("z"));
    EXPECT_EQ(runtime::access(selected, number("0")).as_string(), "owned");
    EXPECT_EQ(
        object.member(std::string_view("x\0y", 3)).as_string(), "nul key"
    );
    EXPECT_EQ(object.depth(), 3U);
    EXPECT_EQ(value(std::string("a\0b", 3)).size(), 3U);
    EXPECT_EQ(value("é").size(), 2U);
    fails(runtime::error_code::duplicate_key, [] {
        value::object({ { "a", value() }, { "a", value() } });
    });
    fails(runtime::error_code::missing_key, [&] { object.member("missing"); });
    fails(runtime::error_code::type, [&] {
        runtime::access(object, number("0"));
    });
    fails(runtime::error_code::unsupported_operation, [&] {
        runtime::binary("==", object, object);
    });
    value nested;
    for (size_t depth = 1; depth < 512; ++depth)
        nested = value::list({ nested });
    EXPECT_EQ(nested.depth(), 512U);
    fails(runtime::error_code::resource, [&] { value::list({ nested }); });
}

TEST(RuntimeValues, ResourceErrorsAndOwnedDiagnosticsAreStable) {
    fails(runtime::error_code::resource, [] {
        numeric_literal("1e99999999999999999999");
    });
    fails(runtime::error_code::resource, [] {
        runtime::binary("*", number("128"), number("2"), { 8, 100 });
    });
    runtime::limits options;
    options.steps = 2;
    options.elements = 1;
    options.string_bytes = 3;
    runtime::budget budget(options);
    budget.spend(2);
    fails(runtime::error_code::resource, [&] { budget.spend(); });
    EXPECT_EQ(budget.steps_used(), 2U);
    budget.elements(1);
    fails(runtime::error_code::resource, [&] { budget.elements(1); });
    budget.bytes(3);
    fails(runtime::error_code::resource, [&] { budget.bytes(1); });
    runtime::error located = [] {
        const auto input = source::from_memory("first\n 1/0", "error.qc");
        return runtime::error(
                   runtime::error_code::division_by_zero, "division by zero"
        )
            .located(source_span(input, 7, 10));
    }();
    EXPECT_STREQ(
        located.what(), "error.qc:2:2: division_by_zero: division by zero"
    );
    EXPECT_EQ(located.location()->text(), "1/0");
    EXPECT_STREQ(located.located(*located.location()).what(), located.what());
    options.depth = 513;
    EXPECT_THROW(runtime::budget invalid(options), std::invalid_argument);
}
