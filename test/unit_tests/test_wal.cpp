#include <array>
#include <gtest/gtest.h>
#include "calico/bytes.h"
#include "calico/options.h"
#include "calico/storage.h"
#include "fakes.h"
#include "tools.h"
#include "unit_tests.h"
#include "utils/layout.h"
#include "utils/logging.h"
#include "wal/basic_wal.h"
#include "wal/helpers.h"
#include "wal/reader.h"
#include "wal/writer.h"

namespace calico {

namespace internal {
    extern std::uint32_t random_seed;
} // namespace internal

namespace fs = std::filesystem;

template<class Base>
class TestWithWalSegments: public Base {
public:
    [[nodiscard]]
    static auto get_segment_name(SegmentId id) -> std::string
    {
        return Base::PREFIX + id.to_name();
    }

    [[nodiscard]]
    static auto get_segment_name(Size index) -> std::string
    {
        return Base::PREFIX + SegmentId::from_index(index).to_name();
    }

    template<class Id>
    [[nodiscard]]
    auto get_segment_size(const Id &id) const -> Size
    {
        Size size {};
        EXPECT_TRUE(expose_message(Base::store->file_size(get_segment_name(id), size)));
        return size;
    }

    template<class Id>
    [[nodiscard]]
    auto get_segment_data(const Id &id) const -> std::string
    {
        RandomReader *reader {};
        EXPECT_TRUE(expose_message(Base::store->open_random_reader(get_segment_name(id), &reader)));

        std::string data(get_segment_size(id), '\x00');
        auto bytes = stob(data);
        EXPECT_TRUE(expose_message(reader->read(bytes, 0)));
        EXPECT_EQ(bytes.size(), data.size());
        delete reader;
        return data;
    }
};

using TestWithWalSegmentsOnHeap = TestWithWalSegments<TestOnHeap>;
using TestWithWalSegmentsOnDisk = TestWithWalSegments<TestOnDisk>;

template<class Store>
[[nodiscard]]
static auto get_file_size(const Store &store, const std::string &path) -> Size
{
    Size size {};
    EXPECT_TRUE(expose_message(store.file_size(path, size)));
    return size;
}

class WalPayloadSizeLimitTests: public testing::TestWithParam<Size> {
public:
    WalPayloadSizeLimitTests()
        : scratch(max_size, '\x00'),
          image {random.get<std::string>('\x00', '\xFF', GetParam())}
    {
        static_assert(WAL_SCRATCH_SCALE >= 1);
    }

    ~WalPayloadSizeLimitTests() override = default;

    Size max_size {GetParam() * WAL_SCRATCH_SCALE};
    Size min_size {max_size - GetParam()};
    Random random {internal::random_seed};
    std::string scratch;
    std::string image;
};

TEST_P(WalPayloadSizeLimitTests, LargestPossibleRecord)
{
    std::vector<PageDelta> deltas;

    for (Size i {}; i < GetParam(); i += 2)
        deltas.emplace_back(PageDelta {i, 1});

    auto size = encode_deltas_payload(SequenceId {1}, PageId {2}, stob(image), deltas, stob(scratch));
    ASSERT_GE(size, min_size) << "Excessive scratch memory allocated";
    ASSERT_LE(size, max_size) << "Scratch memory cannot fit maximally sized WAL record payload";
}

INSTANTIATE_TEST_SUITE_P(
    LargestPossibleRecord,
    WalPayloadSizeLimitTests,
    ::testing::Values(
        0x100,
        0x100 << 1,
        0x100 << 2,
        0x100 << 3,
        0x100 << 4,
        0x100 << 5,
        0x100 << 6,
        0x100 << 7
        )
);

class WalRecordMergeTests: public testing::Test {
public:
    auto setup(const std::array<WalRecordHeader::Type, 3> &types) -> void
    {
        lhs.type = types[0];
        rhs.type = types[1];
        lhs.size = 1;
        rhs.size = 2;
    }

    auto check(const WalRecordHeader &header, WalRecordHeader::Type type) -> bool
    {
        return header.type == type && header.size == 3;
    }

    std::vector<std::array<WalRecordHeader::Type, 3>> valid_left_merges {
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type {}, WalRecordHeader::Type::FIRST, WalRecordHeader::Type::FIRST},
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type {}, WalRecordHeader::Type::FULL, WalRecordHeader::Type::FULL},
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type::FIRST, WalRecordHeader::Type::MIDDLE, WalRecordHeader::Type::FIRST},
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type::FIRST, WalRecordHeader::Type::LAST, WalRecordHeader::Type::FULL},
    };
    std::vector<std::array<WalRecordHeader::Type, 3>> valid_right_merges {
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type::LAST, WalRecordHeader::Type {}, WalRecordHeader::Type::LAST},
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type::FULL, WalRecordHeader::Type {}, WalRecordHeader::Type::FULL},
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type::MIDDLE, WalRecordHeader::Type::LAST, WalRecordHeader::Type::LAST},
        std::array<WalRecordHeader::Type, 3> {WalRecordHeader::Type::FIRST, WalRecordHeader::Type::LAST, WalRecordHeader::Type::FULL},
    };
    WalRecordHeader lhs {};
    WalRecordHeader rhs {};
};

TEST_F(WalRecordMergeTests, MergeEmptyRecordsDeathTest)
{
    ASSERT_DEATH(const auto ignore = merge_records_left(lhs, rhs), EXPECTATION_MATCHER);
    ASSERT_DEATH(const auto ignore = merge_records_right(lhs, rhs), EXPECTATION_MATCHER);
}

TEST_F(WalRecordMergeTests, ValidLeftMerges)
{
    ASSERT_TRUE(std::all_of(cbegin(valid_left_merges), cend(valid_left_merges), [this](const auto &triplet) {
        setup(triplet);
        const auto s = merge_records_left(lhs, rhs);
        return s.is_ok() && check(lhs, triplet[2]);
    }));
}

TEST_F(WalRecordMergeTests, ValidRightMerges)
{
    ASSERT_TRUE(std::all_of(cbegin(valid_right_merges), cend(valid_right_merges), [this](const auto &triplet) {
        setup(triplet);
        const auto s = merge_records_right(lhs, rhs);
        return s.is_ok() && check(rhs, triplet[2]);
    }));
}

TEST_F(WalRecordMergeTests, MergeInvalidTypesDeathTest)
{
    setup({WalRecordHeader::Type::FIRST, WalRecordHeader::Type::FIRST});
    ASSERT_DEATH(const auto ignore = merge_records_left(lhs, rhs), EXPECTATION_MATCHER);
    ASSERT_DEATH(const auto ignore = merge_records_right(lhs, rhs), EXPECTATION_MATCHER);

    setup({WalRecordHeader::Type {}, WalRecordHeader::Type::MIDDLE});
    ASSERT_DEATH(const auto ignore = merge_records_left(lhs, rhs), EXPECTATION_MATCHER);
    ASSERT_DEATH(const auto ignore = merge_records_right(lhs, rhs), EXPECTATION_MATCHER);

    setup({WalRecordHeader::Type::MIDDLE, WalRecordHeader::Type::FIRST});
    ASSERT_DEATH(const auto ignore = merge_records_left(lhs, rhs), EXPECTATION_MATCHER);

    setup({WalRecordHeader::Type::FIRST, WalRecordHeader::Type::MIDDLE});
    ASSERT_DEATH(const auto ignore = merge_records_right(lhs, rhs), EXPECTATION_MATCHER);
}

class WalPayloadTests: public testing::Test {
public:
    static constexpr Size PAGE_SIZE {0x80};

    WalPayloadTests()
        : image {random.get<std::string>('\x00', '\xFF', PAGE_SIZE)},
          scratch(PAGE_SIZE * WAL_SCRATCH_SCALE, '\x00')
    {}

    Random random {internal::random_seed};
    std::string image;
    std::string scratch;
};

TEST_F(WalPayloadTests, EncodeAndDecodeFullImage)
{
    const auto size = encode_full_image_payload(SequenceId {1}, PageId::root(), stob(image), stob(scratch));
    const auto descriptor = decode_full_image_payload(stob(scratch).truncate(size));
    ASSERT_EQ(descriptor.page_id, 1);
    ASSERT_EQ(descriptor.image.to_string(), image);
}

TEST_F(WalPayloadTests, EncodeAndDecodeDeltas)
{
    WalRecordGenerator generator;
    auto deltas = generator.setup_deltas(stob(image));
    const auto size = encode_deltas_payload(SequenceId {1}, PageId::root(), stob(image), deltas, stob(scratch));
    const auto descriptor = decode_deltas_payload(stob(scratch).truncate(size));
    ASSERT_EQ(descriptor.page_lsn, 123);
    ASSERT_EQ(descriptor.page_id, 1);
    ASSERT_EQ(descriptor.deltas.size(), deltas.size());
    ASSERT_TRUE(std::all_of(cbegin(descriptor.deltas), cend(descriptor.deltas), [this](const DeltaContent &delta) {
        return delta.data == stob(image).range(delta.offset, delta.data.size());
    }));
}

[[nodiscard]]
auto get_ids(const WalCollection &c)
{
    std::vector<SegmentId> ids;
    std::transform(cbegin(c.segments()), cend(c.segments()), back_inserter(ids), [](const auto &id) {
        return id;
    });
    return ids;
}

class WalCollectionTests: public testing::Test {
public:
    auto add_segments(Size n)
    {
        for (Size i {}; i < n; ++i) {
            auto id = SegmentId::from_index(i);
            collection.add_segment(id);
        }
        ASSERT_EQ(collection.last(), SegmentId::from_index(n - 1));
    }

    WalCollection collection;
};

TEST_F(WalCollectionTests, NewCollectionState)
{
    ASSERT_TRUE(collection.last().is_null());
}

TEST_F(WalCollectionTests, AddSegment)
{
    collection.add_segment(SegmentId {1});
    ASSERT_EQ(collection.last().value, 1);
}

TEST_F(WalCollectionTests, RecordsMostRecentSegmentId)
{
    add_segments(20);
    ASSERT_EQ(collection.last(), SegmentId::from_index(19));
}

template<class Itr>
[[nodiscard]]
auto contains_n_consecutive_segments(const Itr &begin, const Itr &end, SegmentId id, Size n)
{
    return std::distance(begin, end) == std::ptrdiff_t(n) && std::all_of(begin, end, [&id](auto current) {
        return current.value == id.value++;
    });
}

TEST_F(WalCollectionTests, RecordsSegmentInfoCorrectly)
{
    add_segments(20);

    const auto ids = get_ids(collection);
    ASSERT_EQ(ids.size(), 20);

    const auto result = get_ids(collection);
    ASSERT_TRUE(contains_n_consecutive_segments(cbegin(result), cend(result), SegmentId {1}, 20));
}

TEST_F(WalCollectionTests, RemovesAllSegmentsFromLeft)
{
    add_segments(20);
    // SegmentId::from_index(20) is one past the end.
    collection.remove_before(SegmentId::from_index(20));

    const auto ids = get_ids(collection);
    ASSERT_TRUE(ids.empty());
}

TEST_F(WalCollectionTests, RemovesAllSegmentsFromRight)
{
    add_segments(20);
    // SegmentId::null() is one before the beginning.
    collection.remove_after(SegmentId::null());

    const auto ids = get_ids(collection);
    ASSERT_TRUE(ids.empty());
}

TEST_F(WalCollectionTests, RemovesSomeSegmentsFromLeft)
{
    add_segments(20);
    collection.remove_before(SegmentId::from_index(10));

    const auto ids = get_ids(collection);
    ASSERT_TRUE(contains_n_consecutive_segments(cbegin(ids), cend(ids), SegmentId::from_index(10), 10));
}

TEST_F(WalCollectionTests, RemovesSomeSegmentsFromRight)
{
    add_segments(20);
    collection.remove_after(SegmentId::from_index(9));

    const auto ids = get_ids(collection);
    ASSERT_TRUE(contains_n_consecutive_segments(cbegin(ids), cend(ids), SegmentId::from_index(0), 10));
}

class LogTests: public TestWithWalSegmentsOnHeap {
public:
    static constexpr Size PAGE_SIZE {0x100};

    LogTests()
        : reader_payload(wal_scratch_size(PAGE_SIZE), '\x00'),
          reader_tail(wal_block_size(PAGE_SIZE), '\x00'),
          writer_tail(wal_block_size(PAGE_SIZE), '\x00')
    {}

    auto get_reader(SegmentId id) -> LogReader
    {
        const auto path = get_segment_name(id);
        EXPECT_TRUE(expose_message(store->open_random_reader(path, &reader_file)));
        return LogReader {*reader_file};
    }

    auto get_writer(SegmentId id) -> LogWriter
    {
        const auto path = get_segment_name(id);
        EXPECT_TRUE(expose_message(store->open_append_writer(path, &writer_file)));
        return LogWriter {*writer_file, stob(writer_tail), flushed_lsn};
    }

    auto write_string(LogWriter &writer, const std::string &payload) -> void
    {
        ASSERT_TRUE(expose_message(writer.write(++last_lsn, BytesView {payload})));
    }

    auto read_string(LogReader &reader) -> std::string
    {
        Bytes out {reader_payload};
        EXPECT_TRUE(expose_message(reader.read(out, Bytes {reader_tail})));
        return out.to_string();
    }

    auto run_basic_test(const std::vector<std::string> &payloads) -> void
    {
        auto writer = get_writer(SegmentId {1});
        auto reader = get_reader(SegmentId {1});
        for (const auto &payload: payloads) {
            ASSERT_LE(payload.size(), wal_scratch_size(PAGE_SIZE));
            write_string(writer, payload);
        }
        ASSERT_TRUE(expose_message(writer.flush()));

        for (const auto &payload: payloads) {
            ASSERT_EQ(read_string(reader), payload);
        }
    }

    [[nodiscard]]
    auto get_small_payload() -> std::string
    {
        return random.get<std::string>('a', 'z', wal_scratch_size(PAGE_SIZE) / random.get(10UL, 20UL));
    }

    [[nodiscard]]
    auto get_large_payload() -> std::string
    {
        return random.get<std::string>('a', 'z', 2 * wal_scratch_size(PAGE_SIZE) / random.get(2UL, 4UL));
    }

    std::atomic<SequenceId> flushed_lsn {};
    std::string reader_payload;
    std::string reader_tail;
    std::string writer_tail;
    RandomReader *reader_file {};
    AppendWriter *writer_file {};
    SequenceId last_lsn;
    Random random {internal::random_seed};
};

TEST_F(LogTests, DoesNotFlushEmptyBlock)
{
    auto writer = get_writer(SegmentId {1});
    ASSERT_TRUE(expose_message(writer.flush()));

    Size file_size {};
    ASSERT_TRUE(expose_message(store->file_size("test/wal-000001", file_size)));
    ASSERT_EQ(file_size, 0);
}

TEST_F(LogTests, WritesMultipleBlocks)
{
    auto writer = get_writer(SegmentId {1});
    write_string(writer, get_large_payload());
    ASSERT_TRUE(expose_message(writer.flush()));

    Size file_size {};
    ASSERT_TRUE(expose_message(store->file_size("test/wal-000001", file_size)));
    ASSERT_EQ(file_size % writer_tail.size(), 0);
    ASSERT_GT(file_size / writer_tail.size(), 0);
}

TEST_F(LogTests, SingleSmallPayload)
{
    run_basic_test({get_small_payload()});
}

TEST_F(LogTests, MultipleSmallPayloads)
{
    run_basic_test({
        get_small_payload(),
        get_small_payload(),
        get_small_payload(),
        get_small_payload(),
        get_small_payload(),
    });
}

TEST_F(LogTests, SingleLargePayload)
{
    run_basic_test({get_large_payload()});
}

TEST_F(LogTests, MultipleLargePayloads)
{
    run_basic_test({
        get_large_payload(),
        get_large_payload(),
        get_large_payload(),
        get_large_payload(),
        get_large_payload(),
    });
}

TEST_F(LogTests, MultipleMixedPayloads)
{
    run_basic_test({
        get_small_payload(),
        get_large_payload(),
        get_small_payload(),
        get_large_payload(),
        get_small_payload(),
    });
}

TEST_F(LogTests, SanityCheck)
{
    std::vector<std::string> payloads(1'000);
    std::generate(begin(payloads), end(payloads), [this] {
        return random.get(4) ? get_small_payload() : get_large_payload();
    });
    run_basic_test(payloads);
}

TEST_F(LogTests, HandlesEarlyFlushes)
{
    std::vector<std::string> payloads(1'000);
    std::generate(begin(payloads), end(payloads), [this] {
        return random.get(4) ? get_small_payload() : get_large_payload();
    });

    auto writer = get_writer(SegmentId {1});
    auto reader = get_reader(SegmentId {1});
    for (const auto &payload: payloads) {
        ASSERT_LE(payload.size(), wal_scratch_size(PAGE_SIZE));
        write_string(writer, payload);
        if (random.get(10) == 0) {
            ASSERT_TRUE(expose_message(writer.flush()));
        }
    }
    ASSERT_TRUE(expose_message(writer.flush()));

    for (const auto &payload: payloads) {
        ASSERT_EQ(read_string(reader), payload);
    }
}

class WalWriterTests: public TestWithWalSegmentsOnHeap {
public:
    static constexpr Size PAGE_SIZE {0x100};
    static constexpr Size WAL_LIMIT {2};

    WalWriterTests()
        : scratch {wal_scratch_size(PAGE_SIZE)},
          tail(wal_block_size(PAGE_SIZE), '\x00')
    {
        writer.emplace(
            *store,
            collection,
            scratch,
            Bytes {tail},
            flushed_lsn,
            PREFIX,
            WAL_LIMIT
        );
    }

    ~WalWriterTests() override = default;

    auto SetUp() -> void override
    {
         ASSERT_TRUE(expose_message(writer->open()));
    }

    WalCollection collection;
    LogScratchManager scratch;
    std::atomic<SequenceId> flushed_lsn {};
    std::optional<WalWriter> writer;
    std::string tail;
    Random random {internal::random_seed};
};

TEST_F(WalWriterTests, NewWriterStatus)
{
    ASSERT_TRUE(expose_message(writer->status()));
}

TEST_F(WalWriterTests, DoesNotLeaveEmptySegments_NormalClose)
{
    // After the writer closes a segment file, it will either add it to the set of segment files, or it
    // will delete it. Empty segments get deleted, while nonempty segments get added.
    writer->advance();
    writer->advance();
    writer->advance();

    // Blocks until the last segment is deleted.
    ASSERT_TRUE(expose_message(std::move(*writer).destroy()));
    ASSERT_TRUE(collection.segments().empty());

    std::vector<std::string> children;
    ASSERT_TRUE(expose_message(store->get_children(ROOT, children)));
    ASSERT_TRUE(children.empty());
}

TEST_F(WalWriterTests, DoesNotLeaveEmptySegments_WriteFailure)
{
    interceptors::set_write(FailAfter<0> {"test/wal-"});

    while (writer->status().is_ok())
        writer->write(SequenceId {1}, scratch.get());

    // Blocks until the last segment is deleted.
    ASSERT_TRUE(expose_message(std::move(*writer).destroy()));
    ASSERT_TRUE(collection.segments().empty());

    std::vector<std::string> children;
    ASSERT_TRUE(expose_message(store->get_children(ROOT, children)));
    ASSERT_TRUE(children.empty());
}

//TEST_F(WalWriterTests, DoesNotLeaveEmptySegments_WriteFailure)
//{
//    interceptors::set_write(FailOnce<0> {"test/wal-"});
//
//    SequenceId lsn {1};
//    while (writer->status().is_ok()) {
//        auto payload = scratch.get();
//        const auto size = random.get(1UL, payload->size());
//        auto data = random.get<std::string>('a', 'z', size);
//        mem_copy(*payload, data);
//        payload->truncate(data.size());
//        writer->write(lsn, payload);
//    }
//
//    // Blocks until the last segment is deleted.
//    ASSERT_TRUE(expose_message(std::move(*writer).destroy()));
//    ASSERT_TRUE(collection.segments().empty());
//
//    std::vector<std::string> children;
//    ASSERT_TRUE(expose_message(store->get_children(ROOT, children)));
//    ASSERT_TRUE(children.empty());
//}

//
//class BasicWalReaderWriterTests: public TestWithWalSegmentsOnHeap {
//public:
//    static constexpr Size PAGE_SIZE {0x100};
//    static constexpr Size BLOCK_SIZE {PAGE_SIZE * WAL_BLOCK_SCALE};
//
//    BasicWalReaderWriterTests()
//        : scratch {std::make_unique<LogScratchManager>(PAGE_SIZE * WAL_SCRATCH_SCALE)}
//    {}
//
//    auto SetUp() -> void override
//    {
//        reader = std::make_unique<BasicWalReader>(
//            *store,
//            ROOT,
//            PAGE_SIZE
//        );
//
//        writer = std::make_unique<BasicWalWriter>(BasicWalWriter::Parameters {
//            store.get(),
//            &collection,
//            &flushed_lsn,
//            create_logger(create_sink(), "wal"),
//            ROOT,
//            PAGE_SIZE,
//            128,
//        });
//    }
//
//    WalCollection collection;
//    std::atomic<SequenceId> flushed_lsn {};
//    std::unique_ptr<LogScratchManager> scratch;
//    std::unique_ptr<BasicWalReader> reader;
//    std::unique_ptr<BasicWalWriter> writer;
//    Random random {internal::random_seed};
//};
//
//TEST_F(BasicWalReaderWriterTests, NewWriterIsOk)
//{
//    ASSERT_TRUE(writer->status().is_ok());
//    writer->stop();
//}

template<class Test>
auto generate_images(Test &test, Size page_size, Size n)
{
    std::vector<std::string> images;
    std::generate_n(back_inserter(images), n, [&test, page_size] {
        return test.random.template get<std::string>('\x00', '\xFF', page_size);
    });
    return images;
}
//
//auto generate_deltas(std::vector<std::string> &images)
//{
//    WalRecordGenerator generator;
//    std::vector<std::vector<PageDelta>> deltas;
//    for (auto &image: images)
//        deltas.emplace_back(generator.setup_deltas(stob(image)));
//    return deltas;
//}
//
//TEST_F(BasicWalReaderWriterTests, WritesAndReadsDeltasNormally)
//{
//    // NOTE: This test doesn't handle segmentation. If the writer segments, the test will fail!
//    static constexpr Size NUM_RECORDS {100};
//    auto images = generate_images(*this, PAGE_SIZE, NUM_RECORDS);
//    auto deltas = generate_deltas(images);
//    for (Size i {}; i < NUM_RECORDS; ++i)
//        writer->log_deltas(PageId::root(), stob(images[i]), deltas[i]);
//
//    // close() should cause the writer to flush the current block.
//    writer->stop();
//
//    ASSERT_TRUE(expose_message(reader->open(SegmentId {1})));
//
//    Size i {};
//    ASSERT_TRUE(expose_message(reader->redo([deltas, &i, images](const RedoDescriptor &descriptor) {
//        auto lhs = cbegin(descriptor.deltas);
//        auto rhs = cbegin(deltas[i]);
//        for (; rhs != cend(deltas[i]); ++lhs, ++rhs) {
//            EXPECT_NE(lhs, cend(descriptor.deltas));
//            EXPECT_TRUE(lhs->data == stob(images[i]).range(rhs->offset, rhs->size));
//            EXPECT_EQ(lhs->offset, rhs->offset);
//        }
//        i++;
//        return Status::ok();
//    })));
//    ASSERT_EQ(i, images.size());
//    ASSERT_EQ(get_segment_size(0UL) % BLOCK_SIZE, 0);
//}
//
//TEST_F(BasicWalReaderWriterTests, WritesAndReadsFullImagesNormally)
//{
//    // NOTE: This test doesn't handle segmentation. If the writer segments, the test will fail!
//    static constexpr Size NUM_RECORDS {100};
//    auto images = generate_images(*this, PAGE_SIZE, NUM_RECORDS);
//
//    for (Size i {}; i < NUM_RECORDS; ++i)
//        writer->log_full_image(PageId::from_index(i), stob(images[i]));
//    writer->stop();
//
//    ASSERT_TRUE(expose_message(reader->open(SegmentId {1})));
//    ASSERT_TRUE(expose_message(reader->redo([](const auto&) {
//        ADD_FAILURE() << "This should not be called";
//        return Status::logic_error("Logic error!");
//    })));
//
//    Size i {};
//    ASSERT_TRUE(expose_message(reader->undo([&i, images](const UndoDescriptor &descriptor) {
//        EXPECT_EQ(descriptor.page_id, i + 1);
//        EXPECT_TRUE(descriptor.image == stob(images[i]));
//        i++;
//        return Status::ok();
//    })));
//    ASSERT_EQ(i, images.size());
//    ASSERT_EQ(get_segment_size(0UL) % BLOCK_SIZE, 0);
//}
//
//auto test_undo_redo(BasicWalReaderWriterTests &test, Size num_images, Size num_deltas)
//{
//    const auto deltas_per_image = num_deltas / num_images;
//
//    std::vector<std::string> before_images;
//    std::vector<std::string> after_images;
//    WalRecordGenerator generator;
//
//    auto &reader = test.reader;
//    auto &writer = test.writer;
//    auto &random = test.random;
//    auto &collection = test.collection;
//
//    for (Size i {}; i < num_images; ++i) {
//        auto pid = PageId::from_index(i);
//
//        before_images.emplace_back(random.get<std::string>('\x00', '\xFF', BasicWalReaderWriterTests::PAGE_SIZE));
//        writer->log_full_image(pid, stob(before_images.back()));
//
//        after_images.emplace_back(before_images.back());
//        for (Size j {}; j < deltas_per_image; ++j) {
//            const auto deltas = generator.setup_deltas(stob(after_images.back()));
//            writer->log_deltas(pid, stob(after_images.back()), deltas);
//        }
//    }
//    writer->stop();
//
//    // Roll forward some copies of the "before images" to match the "after images".
//    auto images = before_images;
//    for (const auto &[id, meta]: collection.segments()) {
//        ASSERT_TRUE(expose_message(reader->open(id)));
//        ASSERT_TRUE(expose_message(reader->redo([&images](const RedoDescriptor &info) {
//            auto image = stob(images.at(info.page_id - 1));
//            for (const auto &[offset, content]: info.deltas)
//                mem_copy(image.range(offset, content.size()), content);
//            return Status::ok();
//        })));
//        ASSERT_TRUE(expose_message(reader->close()));
//    }
//
//    // Image copies should match the "after images".
//    for (Size i {}; i < images.size(); ++i) {
//        ASSERT_EQ(images.at(i), after_images.at(i));
//    }
//
//    // Now roll them back to match the before images again.
//    for (auto itr = crbegin(collection.segments()); itr != crend(collection.segments()); ++itr) {
//        // Segment ID should be the same for each record position within each group.
//        ASSERT_TRUE(expose_message(reader->open(itr->id)));
//        ASSERT_TRUE(expose_message(reader->undo([&images](const auto &info) {
//            const auto index = info.page_id - 1;
//            mem_copy(stob(images[index]), info.image);
//            return Status::ok();
//        })));
//        ASSERT_TRUE(expose_message(reader->close()));
//    }
//
//    for (Size i {}; i < images.size(); ++i) {
//        ASSERT_EQ(images.at(i), before_images.at(i));
//    }
//}
//
//TEST_F(BasicWalReaderWriterTests, SingleImage)
//{
//    // This situation should not happen in practice, but we technically should be able to handle it.
//    test_undo_redo(*this, 1, 0);
//}
//
//TEST_F(BasicWalReaderWriterTests, SingleImageSingleDelta)
//{
//    test_undo_redo(*this, 1, 1);
//}
//
//TEST_F(BasicWalReaderWriterTests, SingleImageManyDeltas)
//{
//    test_undo_redo(*this, 1, 100);
//}
//
//TEST_F(BasicWalReaderWriterTests, ManyImagesManyDeltas)
//{
//    test_undo_redo(*this, 100, 1'000);
//}
//
////TEST_F(BasicWalReaderWriterTests, ManyManyImagesManyManyDeltas)
////{
////    test_undo_redo(*this, 10'000, 1'000'000);
////}

class BasicWalTests: public TestWithWalSegmentsOnHeap {
public:
    static constexpr Size PAGE_SIZE {0x100};

    ~BasicWalTests() override = default;

    auto SetUp() -> void override
    {
        WriteAheadLog *temp {};

        ASSERT_TRUE(expose_message(BasicWriteAheadLog::open({
            ROOT,
            store.get(),
            create_sink(),
            PAGE_SIZE,
        }, &temp)));

        wal.reset(temp);

        ASSERT_TRUE(expose_message(wal->start_recovery([](const auto &) { return Status::logic_error(""); },
                                                       [](const auto &) { return Status::logic_error(""); })));
    }

    Random random {42};
    std::unique_ptr<WriteAheadLog> wal;
};

TEST_F(BasicWalTests, StartsAndStops)
{
    ASSERT_TRUE(expose_message(wal->start_workers()));
    ASSERT_TRUE(expose_message(wal->stop_workers()));
}

TEST_F(BasicWalTests, NewWalState)
{
    ASSERT_TRUE(expose_message(wal->start_workers()));
    ASSERT_EQ(wal->flushed_lsn(), 0);
    ASSERT_EQ(wal->current_lsn(), 1);
    ASSERT_TRUE(expose_message(wal->stop_workers()));
}

TEST_F(BasicWalTests, WriterDoesNotLeaveEmptySegments)
{
    std::vector<std::string> children;

    for (Size i {}; i < 10; ++i) {
        ASSERT_TRUE(expose_message(wal->start_workers()));

        // File should be deleted before this method returns, if no records were written to it.
        ASSERT_TRUE(expose_message(wal->stop_workers()));
        ASSERT_TRUE(expose_message(store->get_children(ROOT, children)));
        ASSERT_TRUE(children.empty());
    }
}

TEST_F(BasicWalTests, FailureDuringFirstOpen)
{
    interceptors::set_open(FailOnce<0> {"test/wal-"});
    ASSERT_TRUE(expose_message(wal->start_workers()));
    ASSERT_TRUE(expose_message(wal->stop_workers()));
}

TEST_F(BasicWalTests, FailureDuringNthOpen)
{
    auto images = generate_images(*this, PAGE_SIZE, 1'000);
    interceptors::set_open(FailEvery<5> {"test/wal-"});
    ASSERT_TRUE(expose_message(wal->start_workers()));

    Size num_writes {};
    for (Size i {}; i < images.size(); ++i) {
        auto s = wal->log(i, stob(images[i]));
        if (!s.is_ok()) {
            assert_error_42(s);
            break;
        }
        num_writes++;
    }
    ASSERT_GT(num_writes, 5);
    ASSERT_TRUE(expose_message(wal->stop_workers()));
}

} // <anonymous>