#include "exact.hpp"
#include <gtest/gtest.h>

#include <numeric>
#include <stdexcept>

namespace {
exact::number number(const std::string& text) {
    auto parsed = exact::parse(text);
    if (!parsed.value)
        throw std::runtime_error("invalid test constant: " + text);
    return *parsed.value;
}

std::string fraction(int numerator, int denominator) {
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const int common = std::gcd(numerator, denominator);
    numerator /= common;
    denominator /= common;
    return std::to_string(numerator)
        + (denominator == 1 ? "" : "/" + std::to_string(denominator));
}

struct spelling_case {
    std::string spelling, canonical;
};

class ExactSpelling : public testing::TestWithParam<spelling_case> { };
}

TEST_P(ExactSpelling, PreservesMathematicalValue) {
    const auto value = exact::parse(GetParam().spelling);
    ASSERT_TRUE(value.value);
    EXPECT_EQ(value.error, exact::failure::none);
    EXPECT_EQ(value.value->str(), GetParam().canonical);
}

INSTANTIATE_TEST_SUITE_P(
    Numbers, ExactSpelling,
    testing::Values(
        spelling_case { "0", "0" },
        spelling_case { "-0.000e999999999999999", "0" },
        spelling_case { "+12", "12" }, spelling_case { "36.6", "183/5" },
        spelling_case { "1.2", "6/5" }, spelling_case { "1.2300e2", "123" },
        spelling_case { "12E-3", "3/250" },
        spelling_case { "-0.000125", "-1/8000" },
        spelling_case { "9007199254740993", "9007199254740993" },
        spelling_case {
            "10000000000000000000000000000000000000000000000000000e-52", "1" }
    )
);

class InvalidExactSpelling : public testing::TestWithParam<std::string> { };

TEST_P(InvalidExactSpelling, RejectsNonNumericGrammar) {
    auto value = exact::parse(GetParam());
    EXPECT_FALSE(value.value);
    EXPECT_EQ(value.error, exact::failure::invalid_literal);
}

INSTANTIATE_TEST_SUITE_P(
    Numbers, InvalidExactSpelling,
    testing::Values(
        "", "+", "-", "01", "1.", ".5", "1e", "1e+", "1e-", "1e2e3", " 1", "1 ",
        "nan", "inf", "0x10", "1/2", "0e999999999999bad"
    )
);

TEST(Exact, DecimalProductAndCanonicalSigns) {
    auto product = exact::calculate(
        exact::operation::multiply, number("36.6"), number("1.2")
    );
    ASSERT_TRUE(product.value);
    EXPECT_EQ(product.value->str(), "1098/25");
    auto quotient
        = exact::calculate(exact::operation::divide, number("3"), number("-6"));
    ASSERT_TRUE(quotient.value);
    EXPECT_EQ(quotient.value->str(), "-1/2");
    EXPECT_EQ(exact::negate(*quotient.value).value->str(), "1/2");
    EXPECT_EQ(
        exact::calculate(
            exact::operation::subtract, *quotient.value, *quotient.value
        )
            .value->str(),
        "0"
    );
}

TEST(Exact, IndependentSmallFractionOracle) {
    for (int a = -7; a <= 7; ++a)
        for (int b = 1; b <= 7; ++b)
            for (int c = -7; c <= 7; ++c)
                for (int d = 1; d <= 7; ++d) {
                    const auto left = *exact::calculate(
                                           exact::operation::divide,
                                           number(std::to_string(a)),
                                           number(std::to_string(b))
                    )
                                           .value;
                    const auto right = *exact::calculate(
                                            exact::operation::divide,
                                            number(std::to_string(c)),
                                            number(std::to_string(d))
                    )
                                            .value;
                    EXPECT_EQ(
                        exact::calculate(exact::operation::add, left, right)
                            .value->str(),
                        fraction(a * d + c * b, b * d)
                    );
                    EXPECT_EQ(
                        exact::calculate(
                            exact::operation::subtract, left, right
                        )
                            .value->str(),
                        fraction(a * d - c * b, b * d)
                    );
                    EXPECT_EQ(
                        exact::calculate(
                            exact::operation::multiply, left, right
                        )
                            .value->str(),
                        fraction(a * c, b * d)
                    );
                    if (c != 0) {
                        EXPECT_EQ(
                            exact::calculate(
                                exact::operation::divide, left, right
                            )
                                .value->str(),
                            fraction(a * d, b * c)
                        );
                    }
                    EXPECT_EQ(
                        left.compare(right),
                        a * d == c * b      ? 0
                            : a * d < c * b ? -1
                                            : 1
                    );
                    EXPECT_EQ(left == right, a * d == c * b);
                }
}

TEST(Exact, RejectsGrowthBeforeLargeExponentOrProductAllocation) {
    const exact::limits budget { 8, 100 };
    for (const auto* text : { "256", "-256", "1e100000000000000000000000",
                              "1e-10000000000000000000000", "0.000000001" }) {
        const auto value = exact::parse(text, budget);
        EXPECT_FALSE(value.value) << text;
        EXPECT_EQ(value.error, exact::failure::growth) << text;
    }
    EXPECT_EQ(
        exact::parse(std::string(101, '1'), budget).error,
        exact::failure::growth
    );
    EXPECT_EQ(
        exact::calculate(
            exact::operation::multiply, number("128"), number("2"), budget
        )
            .error,
        exact::failure::growth
    );
    EXPECT_EQ(
        exact::calculate(
            exact::operation::add, number("255"), number("1"), budget
        )
            .error,
        exact::failure::growth
    );
    EXPECT_EQ(
        exact::calculate(
            exact::operation::subtract, number("-255"), number("1"), budget
        )
            .error,
        exact::failure::growth
    );
    EXPECT_EQ(
        exact::negate(number("256"), budget).error, exact::failure::growth
    );
    EXPECT_EQ(exact::parse("0.5", { 3, 100 }).value->str(), "1/2");
    EXPECT_EQ(exact::parse("1e99", { 1, 4 }).error, exact::failure::growth);
}

TEST(Exact, CancelsBeforeMultiplyingAndComparesWithoutGrowth) {
    const auto fraction_value
        = *exact::calculate(
               exact::operation::divide, number("254"), number("255")
        )
               .value;
    const auto reciprocal
        = *exact::calculate(
               exact::operation::divide, number("255"), number("254")
        )
               .value;
    EXPECT_EQ(
        exact::calculate(
            exact::operation::multiply, fraction_value, reciprocal, { 8, 100 }
        )
            .value->str(),
        "1"
    );
    EXPECT_LT(fraction_value.compare(reciprocal), 0);
    const auto half = number("0.5");
    EXPECT_EQ(
        exact::calculate(exact::operation::add, half, half, { 2, 100 })
            .value->str(),
        "1"
    );
}

TEST(Exact, LimitMasksCoverSmallWordBoundaryAndMaximumWidths) {
    for (const size_t bits :
         { 1U, 2U, 7U, 8U, 63U, 64U, 65U, 127U, 128U, 129U, 4096U, 65536U }) {
        const auto value = exact::parse("1", { bits, 100 });
        ASSERT_TRUE(value.value) << bits;
        EXPECT_TRUE(value.value->is_one());
    }
    EXPECT_EQ(exact::parse("2", { 1, 100 }).error, exact::failure::growth);
    const auto boundary = exact::parse("18446744073709551615", { 64, 100 });
    ASSERT_TRUE(boundary.value);
    EXPECT_EQ(boundary.value->bits(), 64U);
    EXPECT_EQ(
        exact::parse("18446744073709551616", { 64, 100 }).error,
        exact::failure::growth
    );
    EXPECT_EQ(
        exact::parse("18446744073709551616", { 65, 100 }).value->bits(), 65U
    );
}

TEST(Exact, DivisionByZeroIsAnOutcomeAndLimitsAreValidated) {
    EXPECT_EQ(
        exact::calculate(exact::operation::divide, number("1"), number("0"))
            .error,
        exact::failure::division_by_zero
    );
    EXPECT_THROW(exact::parse("1", { 0, 100 }), std::invalid_argument);
    EXPECT_THROW(exact::parse("1", { 65537, 100 }), std::invalid_argument);
    EXPECT_THROW(exact::parse("1", { 8, 0 }), std::invalid_argument);
    EXPECT_THROW(exact::parse("1", { 8, 1048577 }), std::invalid_argument);
    EXPECT_TRUE(exact::number().is_zero());
    EXPECT_TRUE(number("1.000").is_one());
}
