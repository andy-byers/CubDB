// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "common.h"
#include "test.h"

#include "encoding.h"
#include "error.h"
#include "logging.h"

namespace calicodb::test
{

class AllocTests : public testing::Test
{
public:
    static constexpr size_t kFakeAllocationSize = 1'024;
    alignas(uint64_t) static char s_fake_allocation[kFakeAllocationSize];
    static uint64_t *s_alloc_size_ptr;
    static void *s_alloc_data_ptr;

    explicit AllocTests() = default;
    ~AllocTests() override = default;

    auto SetUp() -> void override
    {
        ASSERT_EQ(Alloc::bytes_used(), 0);
    }

    auto TearDown() -> void override
    {
        ASSERT_EQ(Alloc::bytes_used(), 0);
        ASSERT_EQ(Alloc::set_limit(0), 0);
        ASSERT_EQ(Alloc::set_methods(Alloc::kDefaultMethods), 0);
        Alloc::set_hook(nullptr, nullptr);
    }
};

// The wrapper functions in alloc.cpp add a header of 8 bytes to each allocation, which is
// used to store the number of bytes in the rest of the allocation.
alignas(uint64_t) char AllocTests::s_fake_allocation[kFakeAllocationSize];
uint64_t *AllocTests::s_alloc_size_ptr = reinterpret_cast<uint64_t *>(s_fake_allocation);
void *AllocTests::s_alloc_data_ptr = s_alloc_size_ptr + 1;

static constexpr Alloc::Methods kFakeMethods = {
    [](auto) -> void * {
        return AllocTests::s_fake_allocation;
    },
    [](auto *old_ptr, auto) -> void * {
        EXPECT_EQ(old_ptr, AllocTests::s_fake_allocation);
        return AllocTests::s_fake_allocation;
    },
    [](auto *ptr) {
        EXPECT_EQ(ptr, AllocTests::s_fake_allocation);
    },
};

static constexpr Alloc::Methods kFaultyMethods = {
    [](auto) -> void * {
        return nullptr;
    },
    [](auto *, auto) -> void * {
        return nullptr;
    },
    [](auto *) {},
};

TEST_F(AllocTests, Methods)
{
    auto *ptr = Alloc::malloc(123);
    ASSERT_NE(ptr, nullptr);
    auto *new_ptr = Alloc::realloc(ptr, 321);
    ASSERT_NE(new_ptr, nullptr);

    // Prevent malloc/realloc-free mismatch.
    ASSERT_EQ(Alloc::set_methods(kFakeMethods), -1);
    Alloc::free(new_ptr);

    ASSERT_EQ(Alloc::set_methods(kFakeMethods), 0);
    ASSERT_EQ(ptr = Alloc::malloc(123), s_alloc_data_ptr);
    ASSERT_EQ(123, *s_alloc_size_ptr);
    ASSERT_EQ(Alloc::realloc(ptr, 321), ptr);
    ASSERT_EQ(321, *s_alloc_size_ptr);
    ASSERT_EQ(Alloc::realloc(ptr, 42), ptr);
    ASSERT_EQ(42, *s_alloc_size_ptr);
    Alloc::free(nullptr);
    Alloc::free(ptr);

    ASSERT_EQ(Alloc::set_methods(kFaultyMethods), 0);
    ASSERT_EQ(Alloc::malloc(123), nullptr);
    ASSERT_EQ(Alloc::realloc(nullptr, 123), nullptr);
}

TEST_F(AllocTests, Limit)
{
    Alloc::set_limit(100);
    auto *a = Alloc::malloc(50 - sizeof(uint64_t));
    ASSERT_NE(a, nullptr);

    // 8-byte overhead causes this to exceed the limit.
    auto *b = Alloc::malloc(50);
    ASSERT_EQ(b, nullptr);

    b = Alloc::malloc(50 - sizeof(uint64_t));
    ASSERT_NE(b, nullptr);

    // 0 bytes available, fail to get 1 byte.
    auto *c = Alloc::realloc(a, 51 - sizeof(uint64_t));
    ASSERT_EQ(c, nullptr);

    c = Alloc::realloc(a, 20 - sizeof(uint64_t));
    ASSERT_NE(c, nullptr);

    ASSERT_EQ(Alloc::set_limit(1), -1);
    ASSERT_EQ(Alloc::set_limit(0), 0);

    // a was realloc'd.
    Alloc::free(b);
    Alloc::free(c);
}

TEST_F(AllocTests, AllocationHook)
{
    struct HookArg {
        int rc = 0;
    } hook_arg;

    const auto hook = [](auto *arg) {
        return static_cast<const HookArg *>(arg)->rc;
    };

    Alloc::set_hook(hook, &hook_arg);

    void *ptr;
    ASSERT_NE(ptr = Alloc::malloc(123), nullptr);
    ASSERT_NE(ptr = Alloc::realloc(ptr, 321), nullptr);
    Alloc::free(ptr);

    hook_arg.rc = -1;
    ASSERT_EQ(Alloc::malloc(123), nullptr);
    ASSERT_EQ(Alloc::realloc(ptr, 321), nullptr);
}

TEST_F(AllocTests, LargeAllocations)
{
    // Don't actually allocate anything.
    ASSERT_EQ(Alloc::set_methods(kFakeMethods), 0);

    void *p;
    ASSERT_EQ(nullptr, Alloc::malloc(kMaxAllocation + 1));
    ASSERT_NE(nullptr, p = Alloc::malloc(kMaxAllocation));
    ASSERT_EQ(nullptr, Alloc::realloc(p, kMaxAllocation + 1));
    ASSERT_NE(nullptr, p = Alloc::realloc(p, kMaxAllocation));
    Alloc::free(p);
}

TEST_F(AllocTests, ReallocSameSize)
{
    static constexpr size_t kSize = 42;

    void *ptr;
    ASSERT_NE(ptr = Alloc::malloc(kSize), nullptr);
    ASSERT_NE(ptr = Alloc::realloc(ptr, kSize), nullptr);
    Alloc::free(ptr);

    static constexpr Alloc::Methods kFaultyRealloc = {
        std::malloc,
        [](auto *, auto) -> void * {
            return nullptr;
        },
        std::free,
    };
    ASSERT_EQ(Alloc::set_methods(kFaultyRealloc), 0);
    ASSERT_NE(ptr = Alloc::malloc(kSize), nullptr);
    ASSERT_EQ(Alloc::realloc(ptr, kSize), nullptr);
    ASSERT_GT(Alloc::bytes_used(), kSize);
    Alloc::free(ptr);
}

TEST_F(AllocTests, SpecialCases)
{
    // NOOP
    ASSERT_EQ(Alloc::malloc(0), nullptr);
    ASSERT_EQ(Alloc::bytes_used(), 0);

    // NOOP
    ASSERT_EQ(Alloc::realloc(nullptr, 0), nullptr);
    ASSERT_EQ(Alloc::bytes_used(), 0);

    void *ptr;

    // Equivalent to Alloc::malloc(1).
    ASSERT_NE(ptr = Alloc::realloc(nullptr, 1), nullptr);
    ASSERT_NE(ptr, nullptr);
    ASSERT_NE(Alloc::bytes_used(), 0);

    // Equivalent to Alloc::free(ptr).
    ASSERT_EQ(Alloc::realloc(ptr, 0), nullptr);
    ASSERT_EQ(Alloc::bytes_used(), 0);
}

TEST_F(AllocTests, HeapObject)
{
    struct CustomObject : public HeapObject {
        int data[42];
    };

    auto *obj = new (std::nothrow) CustomObject;
    ASSERT_GE(Alloc::bytes_used(), sizeof(CustomObject));
    delete obj;
    ASSERT_EQ(Alloc::bytes_used(), 0);
}

#ifndef NDEBUG
TEST_F(AllocTests, DeathTest)
{
    void *ptr;
    ptr = Alloc::malloc(1);

    auto *size_ptr = reinterpret_cast<uint64_t *>(ptr) - 1;
    // Give back more memory than was allocated in-total. If more than 1 byte were already allocated, this
    // corruption would go undetected.
    *size_ptr = 2;
    ASSERT_DEATH(Alloc::free(ptr), "Assert");
    ASSERT_DEATH((void)Alloc::realloc(ptr, 123), "Assert");
    // Actual allocations must not be zero-length. Alloc::malloc() returns a nullptr if 0 bytes are
    // requested.
    *size_ptr = 0;
    ASSERT_DEATH(Alloc::free(ptr), "Assert");
    ASSERT_DEATH((void)Alloc::realloc(ptr, 123), "Assert");

    *size_ptr = 1;
    Alloc::free(ptr);
}
#endif // NDEBUG

TEST(UniquePtr, PointerWidth)
{
    static_assert(sizeof(UniquePtr<int, DefaultDestructor>) == sizeof(void *));
    static_assert(sizeof(UniquePtr<int, ObjectDestructor>) == sizeof(void *));
    static_assert(sizeof(UniquePtr<int, UserObjectDestructor>) == sizeof(void *));

    struct Destructor1 {
        auto operator()(void *) -> void
        {
        }
    };

    static_assert(sizeof(UniquePtr<int, Destructor1>) == sizeof(void *));

    struct Destructor2 {
        uint8_t u8;
        auto operator()(void *) -> void
        {
        }
    };

    // Object gets padded out to the size of 2 pointers.
    static_assert(sizeof(UniquePtr<int, Destructor2>) == sizeof(void *) * 2);
}

TEST(UniquePtr, DestructorIsCalled)
{
    int destruction_count = 0;

    struct Destructor {
        int *const count;

        explicit Destructor(int &count)
            : count(&count)
        {
        }

        auto operator()(int *ptr) const -> void
        {
            // Ignore calls that result in "delete nullptr".
            *count += ptr != nullptr;
            delete ptr;
        }
    } destructor(destruction_count);

    {
        UniquePtr<int, Destructor> ptr(new int(123), destructor);
        (void)ptr;
    }
    ASSERT_EQ(destruction_count, 1);

    UniquePtr<int, Destructor> ptr(new int(123), destructor);
    ptr.reset();
    ASSERT_EQ(destruction_count, 2);

    ptr.reset(new int(123));
    ASSERT_EQ(destruction_count, 2);

    delete ptr.release();
    ASSERT_EQ(destruction_count, 2);

    UniquePtr<int, Destructor> ptr2(new int(42), destructor);
    ptr = std::move(ptr2);
    ASSERT_EQ(destruction_count, 2);

    auto ptr3 = std::move(ptr);
    ASSERT_EQ(*ptr3, 42);
    ASSERT_EQ(destruction_count, 2);

    ptr3.reset();
    ASSERT_EQ(destruction_count, 3);
}

TEST(Encoding, Fixed32)
{
    std::string s;
    for (uint32_t v = 0; v < 100000; v++) {
        s.resize(s.size() + sizeof(uint32_t));
        put_u32(s.data() + s.size() - sizeof(uint32_t), v);
    }

    const char *p = s.data();
    for (uint32_t v = 0; v < 100000; v++) {
        uint32_t actual = get_u32(p);
        ASSERT_EQ(v, actual);
        p += sizeof(uint32_t);
    }
}

TEST(Coding, Fixed64)
{
    std::string s;
    for (int power = 0; power <= 63; power++) {
        uint64_t v = static_cast<uint64_t>(1) << power;
        s.resize(s.size() + sizeof(uint64_t) * 3);
        put_u64(s.data() + s.size() - sizeof(uint64_t) * 3, v - 1);
        put_u64(s.data() + s.size() - sizeof(uint64_t) * 2, v + 0);
        put_u64(s.data() + s.size() - sizeof(uint64_t) * 1, v + 1);
    }

    const char *p = s.data();
    for (int power = 0; power <= 63; power++) {
        uint64_t v = static_cast<uint64_t>(1) << power;
        uint64_t actual;
        actual = get_u64(p);
        ASSERT_EQ(v - 1, actual);
        p += sizeof(uint64_t);

        actual = get_u64(p);
        ASSERT_EQ(v + 0, actual);
        p += sizeof(uint64_t);

        actual = get_u64(p);
        ASSERT_EQ(v + 1, actual);
        p += sizeof(uint64_t);
    }
}

// Test that encoding routines generate little-endian encodings
TEST(Encoding, EncodingOutput)
{
    std::string dst(4, '\0');
    put_u32(dst.data(), 0x04030201);
    ASSERT_EQ(0x01, static_cast<int>(dst[0]));
    ASSERT_EQ(0x02, static_cast<int>(dst[1]));
    ASSERT_EQ(0x03, static_cast<int>(dst[2]));
    ASSERT_EQ(0x04, static_cast<int>(dst[3]));

    dst.resize(8);
    put_u64(dst.data(), 0x0807060504030201ull);
    ASSERT_EQ(0x01, static_cast<int>(dst[0]));
    ASSERT_EQ(0x02, static_cast<int>(dst[1]));
    ASSERT_EQ(0x03, static_cast<int>(dst[2]));
    ASSERT_EQ(0x04, static_cast<int>(dst[3]));
    ASSERT_EQ(0x05, static_cast<int>(dst[4]));
    ASSERT_EQ(0x06, static_cast<int>(dst[5]));
    ASSERT_EQ(0x07, static_cast<int>(dst[6]));
    ASSERT_EQ(0x08, static_cast<int>(dst[7]));
}

auto append_varint(std::string *s, uint32_t v) -> void
{
    const auto len = varint_length(v);
    s->resize(s->size() + len);
    encode_varint(s->data() + s->size() - len, v);
}

TEST(Coding, Varint32)
{
    std::string s;
    for (uint32_t i = 0; i < (32 * 32); i++) {
        uint32_t v = (i / 32) << (i % 32);
        append_varint(&s, v);
    }

    const char *p = s.data();
    const char *limit = p + s.size();
    for (uint32_t i = 0; i < (32 * 32); i++) {
        uint32_t expected = (i / 32) << (i % 32);
        uint32_t actual;
        const char *start = p;
        p = decode_varint(p, limit, actual);
        ASSERT_TRUE(p != nullptr);
        ASSERT_EQ(expected, actual);
        ASSERT_EQ(varint_length(actual), p - start);
    }
    ASSERT_EQ(p, s.data() + s.size());
}

TEST(Coding, Varint32Overflow)
{
    uint32_t result;
    std::string input("\x81\x82\x83\x84\x85\x11");
    ASSERT_TRUE(decode_varint(input.data(), input.data() + input.size(),
                              result) == nullptr);
}

TEST(Coding, Varint32Truncation)
{
    uint32_t large_value = (1u << 31) + 100;
    std::string s;
    append_varint(&s, large_value);
    uint32_t result;
    for (size_t len = 0; len < s.size() - 1; len++) {
        ASSERT_TRUE(decode_varint(s.data(), s.data() + len, result) == nullptr);
    }
    ASSERT_TRUE(decode_varint(s.data(), s.data() + s.size(), result) !=
                nullptr);
    ASSERT_EQ(large_value, result);
}

// TEST(Status, StatusMessages)
//{
//     ASSERT_EQ("OK", Status::ok().to_string());
//     ASSERT_EQ("I/O error", Status::io_error().to_string());
//     ASSERT_EQ("I/O error: msg", Status::io_error("msg").to_string());
//     ASSERT_EQ("corruption", Status::corruption().to_string());
//     ASSERT_EQ("corruption: msg", Status::corruption("msg").to_string());
//     ASSERT_EQ("invalid argument", Status::invalid_argument().to_string());
//     ASSERT_EQ("invalid argument: msg", Status::invalid_argument("msg").to_string());
//     ASSERT_EQ("not supported", Status::not_supported().to_string());
//     ASSERT_EQ("not supported: msg", Status::not_supported("msg").to_string());
//     ASSERT_EQ("busy", Status::busy().to_string());
//     ASSERT_EQ("busy: msg", Status::busy("msg").to_string());
//     ASSERT_EQ("busy: retry", Status::retry().to_string());
//     ASSERT_EQ("aborted", Status::aborted().to_string());
//     ASSERT_EQ("aborted: msg", Status::aborted("msg").to_string());
//     ASSERT_EQ("aborted: no memory", Status::no_memory().to_string());
//     // Choice of `Status::invalid_argument()` is arbitrary, any `Code-SubCode` combo
//     // is technically legal, but may not be semantically valid (for example, it makes
//     // no sense to retry when a read-only transaction attempts to write: repeating that
//     // action will surely fail next time as well).
//     ASSERT_EQ("invalid argument: retry", Status::invalid_argument(Status::kRetry).to_string());
// }

TEST(Status, StatusCodes)
{
#define CHECK_CODE(_Label, _Code)                \
    ASSERT_TRUE(Status::_Label().is_##_Label()); \
    ASSERT_EQ(Status::_Label().code(), Status::_Code)
#define CHECK_SUBCODE(_Label, _Code, _SubCode)         \
    ASSERT_TRUE(Status::_Label().is_##_Label());       \
    ASSERT_EQ(Status::_Label().code(), Status::_Code); \
    ASSERT_EQ(Status::_Label().subcode(), Status::_SubCode)

    CHECK_CODE(ok, kOK);

    CHECK_CODE(invalid_argument, kInvalidArgument);
    CHECK_CODE(io_error, kIOError);
    CHECK_CODE(not_supported, kNotSupported);
    CHECK_CODE(corruption, kCorruption);
    CHECK_CODE(not_found, kNotFound);
    CHECK_CODE(busy, kBusy);
    CHECK_CODE(aborted, kAborted);

    CHECK_SUBCODE(retry, kBusy, kRetry);
    CHECK_SUBCODE(no_memory, kAborted, kNoMemory);

#undef CHECK_CODE
#undef CHECK_SUBCODE
}

// TEST(Status, Copy)
//{
//     const auto s = Status::invalid_argument("status message");
//     const auto t = s;
//     ASSERT_TRUE(t.is_invalid_argument());
//     ASSERT_EQ(t.to_string(), std::string("invalid argument: ") + "status message");
//
//     ASSERT_TRUE(s.is_invalid_argument());
//     ASSERT_EQ(s.to_string(), std::string("invalid argument: ") + "status message");
// }
//
// TEST(Status, Reassign)
//{
//     auto s = Status::ok();
//     ASSERT_TRUE(s.is_ok());
//
//     s = Status::invalid_argument("status message");
//     ASSERT_TRUE(s.is_invalid_argument());
//     ASSERT_EQ(s.to_string(), "invalid argument: status message");
//
//     s = Status::not_supported("status message");
//     ASSERT_TRUE(s.is_not_supported());
//     ASSERT_EQ(s.to_string(), "not supported: status message");
//
//     s = Status::ok();
//     ASSERT_TRUE(s.is_ok());
// }
//
// TEST(Status, MoveConstructor)
//{
//     {
//         Status ok = Status::ok();
//         Status ok2 = std::move(ok);
//
//         ASSERT_TRUE(ok2.is_ok());
//     }
//
//     {
//         Status status = Status::not_found("custom kNotFound status message");
//         Status status2 = std::move(status);
//
//         ASSERT_TRUE(status2.is_not_found());
//         ASSERT_EQ("not found: custom kNotFound status message", status2.to_string());
//     }
//
//     {
//         Status self_moved = Status::io_error("custom kIOError status message");
//
//         // Needed to bypass compiler warning about explicit move-assignment.
//         Status &self_moved_reference = self_moved;
//         self_moved_reference = std::move(self_moved);
//     }
// }

static auto number_to_string(uint64_t number) -> std::string
{
    String str;
    EXPECT_EQ(append_number(str, number), 0);
    return str.c_str();
}

TEST(Logging, NumberToString)
{
    ASSERT_EQ("0", number_to_string(0));
    ASSERT_EQ("1", number_to_string(1));
    ASSERT_EQ("9", number_to_string(9));

    ASSERT_EQ("10", number_to_string(10));
    ASSERT_EQ("11", number_to_string(11));
    ASSERT_EQ("19", number_to_string(19));
    ASSERT_EQ("99", number_to_string(99));

    ASSERT_EQ("100", number_to_string(100));
    ASSERT_EQ("109", number_to_string(109));
    ASSERT_EQ("190", number_to_string(190));
    ASSERT_EQ("123", number_to_string(123));
    ASSERT_EQ("12345678", number_to_string(12345678));

    static_assert(std::numeric_limits<uint64_t>::max() == 18446744073709551615U,
                  "Test consistency check");
    ASSERT_EQ("18446744073709551000", number_to_string(18446744073709551000U));
    ASSERT_EQ("18446744073709551600", number_to_string(18446744073709551600U));
    ASSERT_EQ("18446744073709551610", number_to_string(18446744073709551610U));
    ASSERT_EQ("18446744073709551614", number_to_string(18446744073709551614U));
    ASSERT_EQ("18446744073709551615", number_to_string(18446744073709551615U));
}

void ConsumeDecimalNumberRoundtripTest(uint64_t number,
                                       const std::string &padding = "")
{
    std::string decimal_number = number_to_string(number);
    std::string input_string = decimal_number + padding;
    Slice input(input_string);
    Slice output = input;
    uint64_t result;
    ASSERT_TRUE(consume_decimal_number(output, &result));
    ASSERT_EQ(number, result);
    ASSERT_EQ(decimal_number.size(), output.data() - input.data());
    ASSERT_EQ(padding.size(), output.size());
}

TEST(Logging, ConsumeDecimalNumberRoundtrip)
{
    ConsumeDecimalNumberRoundtripTest(0);
    ConsumeDecimalNumberRoundtripTest(1);
    ConsumeDecimalNumberRoundtripTest(9);

    ConsumeDecimalNumberRoundtripTest(10);
    ConsumeDecimalNumberRoundtripTest(11);
    ConsumeDecimalNumberRoundtripTest(19);
    ConsumeDecimalNumberRoundtripTest(99);

    ConsumeDecimalNumberRoundtripTest(100);
    ConsumeDecimalNumberRoundtripTest(109);
    ConsumeDecimalNumberRoundtripTest(190);
    ConsumeDecimalNumberRoundtripTest(123);
    ASSERT_EQ("12345678", number_to_string(12345678));

    for (uint64_t i = 0; i < 100; ++i) {
        uint64_t large_number = std::numeric_limits<uint64_t>::max() - i;
        ConsumeDecimalNumberRoundtripTest(large_number);
    }
}

TEST(Logging, ConsumeDecimalNumberRoundtripWithPadding)
{
    ConsumeDecimalNumberRoundtripTest(0, " ");
    ConsumeDecimalNumberRoundtripTest(1, "abc");
    ConsumeDecimalNumberRoundtripTest(9, "x");

    ConsumeDecimalNumberRoundtripTest(10, "_");
    ConsumeDecimalNumberRoundtripTest(11, std::string("\0\0\0", 3));
    ConsumeDecimalNumberRoundtripTest(19, "abc");
    ConsumeDecimalNumberRoundtripTest(99, "padding");

    ConsumeDecimalNumberRoundtripTest(100, " ");

    for (uint64_t i = 0; i < 100; ++i) {
        uint64_t large_number = std::numeric_limits<uint64_t>::max() - i;
        ConsumeDecimalNumberRoundtripTest(large_number, "pad");
    }
}

void ConsumeDecimalNumberOverflowTest(const std::string &input_string)
{
    Slice input(input_string);
    Slice output = input;
    uint64_t result;
    ASSERT_EQ(false, consume_decimal_number(output, &result));
}

TEST(Logging, ConsumeDecimalNumberOverflow)
{
    static_assert(std::numeric_limits<uint64_t>::max() == 18446744073709551615U,
                  "Test consistency check");
    ConsumeDecimalNumberOverflowTest("18446744073709551616");
    ConsumeDecimalNumberOverflowTest("18446744073709551617");
    ConsumeDecimalNumberOverflowTest("18446744073709551618");
    ConsumeDecimalNumberOverflowTest("18446744073709551619");
    ConsumeDecimalNumberOverflowTest("18446744073709551620");
    ConsumeDecimalNumberOverflowTest("18446744073709551621");
    ConsumeDecimalNumberOverflowTest("18446744073709551622");
    ConsumeDecimalNumberOverflowTest("18446744073709551623");
    ConsumeDecimalNumberOverflowTest("18446744073709551624");
    ConsumeDecimalNumberOverflowTest("18446744073709551625");
    ConsumeDecimalNumberOverflowTest("18446744073709551626");

    ConsumeDecimalNumberOverflowTest("18446744073709551700");

    ConsumeDecimalNumberOverflowTest("99999999999999999999");
}

void ConsumeDecimalNumberNoDigitsTest(const std::string &input_string)
{
    Slice input(input_string);
    Slice output = input;
    uint64_t result;
    ASSERT_EQ(false, consume_decimal_number(output, &result));
    ASSERT_EQ(input.data(), output.data());
    ASSERT_EQ(input.size(), output.size());
}

TEST(Logging, ConsumeDecimalNumberNoDigits)
{
    ConsumeDecimalNumberNoDigitsTest("");
    ConsumeDecimalNumberNoDigitsTest(" ");
    ConsumeDecimalNumberNoDigitsTest("a");
    ConsumeDecimalNumberNoDigitsTest(" 123");
    ConsumeDecimalNumberNoDigitsTest("a123");
    ConsumeDecimalNumberNoDigitsTest(std::string("\000123", 4));
    ConsumeDecimalNumberNoDigitsTest(std::string("\177123", 4));
    ConsumeDecimalNumberNoDigitsTest(std::string("\377123", 4));
}

TEST(Logging, AppendFmtString)
{
    String str;
    ASSERT_EQ(0, append_fmt_string(str, "hello %d %s", 42, "goodbye"));
    const std::string long_str(128, '*');
    ASSERT_EQ(0, append_fmt_string(str, "%s", long_str.data()));
    ASSERT_EQ(0, append_fmt_string(str, "empty"));
    ASSERT_EQ(str.c_str(), "hello 42 goodbye" + long_str + "empty");
}

TEST(Slice, Construction)
{
    std::string s("123");
    ASSERT_EQ(s, Slice(s));

    const auto *p = "123";
    ASSERT_EQ(p, Slice(p));
    ASSERT_EQ(p, Slice(p, 3));
}

TEST(Slice, StartsWith)
{
    Slice slice("Hello, world!");
    ASSERT_TRUE(slice.starts_with(""));
    ASSERT_TRUE(slice.starts_with("Hello"));
    ASSERT_TRUE(slice.starts_with("Hello, world!"));
    ASSERT_FALSE(slice.starts_with(" Hello"));
    ASSERT_FALSE(slice.starts_with("ello"));
    ASSERT_FALSE(slice.starts_with("Hello, world! "));
}

TEST(Slice, Comparisons)
{
    Slice slice("Hello, world!");
    const auto shorter = slice.range(0, slice.size() - 1);
    ASSERT_LT(shorter, slice);

    ASSERT_TRUE(Slice("10") > Slice("01"));
    ASSERT_TRUE(Slice("01") < Slice("10"));
    ASSERT_TRUE(Slice("10") >= Slice("01"));
    ASSERT_TRUE(Slice("01") <= Slice("10"));
}

TEST(Slice, Ranges)
{
    Slice slice("Hello, world!");
    ASSERT_TRUE(slice.range(0, 0).is_empty());
    ASSERT_EQ(slice.range(7, 5), Slice("world"));
    ASSERT_EQ(slice, slice.range(0));
    ASSERT_EQ(slice, slice.range(0, slice.size()));
}

TEST(Slice, Advance)
{
    Slice slice("Hello, world!");
    auto copy = slice;
    slice.advance(0);
    ASSERT_EQ(slice, copy);

    slice.advance(5);
    ASSERT_EQ(slice, ", world!");

    slice.advance(slice.size());
    ASSERT_TRUE(slice.is_empty());
}

TEST(Slice, Truncate)
{
    Slice slice("Hello, world!");
    auto copy = slice;
    slice.truncate(slice.size());
    ASSERT_TRUE(slice == copy);

    slice.truncate(5);
    ASSERT_EQ(slice, "Hello");

    slice.truncate(0);
    ASSERT_TRUE(slice.is_empty());
}

TEST(Slice, Clear)
{
    Slice slice("42");
    slice.clear();
    ASSERT_TRUE(slice.is_empty());
    ASSERT_EQ(0, slice.size());
}

static constexpr auto constexpr_slice_test(Slice s, Slice answer) -> int
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != answer[i]) {
            return -1;
        }
    }

    (void)s.starts_with(answer);
    (void)s.data();
    (void)s.range(0, 0);
    (void)s.is_empty();
    s.advance(0);
    s.truncate(s.size());
    return 0;
}

TEST(Slice, ConstantExpressions)
{
    static constexpr std::string_view sv("42");
    static constexpr Slice s1("42");
    static constexpr Slice s2(sv);
    ASSERT_EQ(0, constexpr_slice_test(s1, sv));
    ASSERT_EQ(0, constexpr_slice_test(s1, s2));
}

TEST(Slice, NonPrintableSlice)
{
    {
        const std::string s("\x00\x01", 2);
        ASSERT_EQ(2, Slice(s).size());
    }
    {
        const std::string s("\x00", 1);
        ASSERT_EQ(0, Slice(s).compare(s));
    }
    {
        std::string s("\x00\x00", 2);
        std::string t("\x00\x01", 2);
        ASSERT_LT(Slice(s).compare(t), 0);
    }
    {
        std::string u("\x0F", 1);
        std::string v("\x00", 1);
        v[0] = static_cast<char>(0xF0);

        // Signed comparison. 0xF0 overflows a signed byte and becomes negative.
        ASSERT_LT(v[0], u[0]);

        // Unsigned comparison should come out the other way.
        ASSERT_LT(Slice(u).compare(v), 0);
    }
}

TEST(ErrorState, ReallocateError)
{
    ErrorState state;
    static constexpr auto *kFixed1 = "Hello";
    const auto *msg1 = state.format_error(ErrorState::kCorruptedPage, kFixed1, 1);
    const auto len1 = std::strlen(msg1);
    ASSERT_EQ(msg1[len1 - 1], '1');

    static constexpr auto *kFixed2 = "Hello, world!";
    const auto *msg2 = state.format_error(ErrorState::kCorruptedPage, "Hello, world!", 2);
    const auto len2 = std::strlen(msg2);
    ASSERT_EQ(len1 + std::strlen(kFixed2), len2 + std::strlen(kFixed1));
    ASSERT_EQ(msg2[len2 - 1], '2');
}

#if not NDEBUG
TEST(Expect, DeathTest)
{
    ASSERT_DEATH(CALICODB_EXPECT_TRUE(false), "");
}

TEST(Slice, DeathTest)
{
    Slice slice("Hello, world!");
    const auto oob = slice.size() + 1;

    ASSERT_DEATH(slice.advance(oob), "Assert");
    ASSERT_DEATH(slice.truncate(oob), "Assert");
    ASSERT_DEATH((void)slice.range(oob, 1), "Assert");
    ASSERT_DEATH((void)slice.range(0, oob), "Assert");
    ASSERT_DEATH((void)slice.range(oob / 2, oob - 1), "Assert");
    ASSERT_DEATH((void)slice.range(oob), "Assert");
    ASSERT_DEATH((void)slice[oob], "Assert");
    ASSERT_DEATH(Slice(nullptr), "Assert");
    ASSERT_DEATH(Slice(nullptr, 123), "Assert");
}
#endif // not NDEBUG

} // namespace calicodb::test
