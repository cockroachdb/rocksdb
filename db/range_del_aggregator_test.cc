//  Copyright (c) 2016-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <algorithm>

#include "db/db_test_util.h"
#include "db/range_del_aggregator.h"
#include "rocksdb/comparator.h"
#include "util/testutil.h"

namespace rocksdb {

class RangeDelAggregatorTest : public testing::Test {};

namespace {

struct ExpectedPoint {
  Slice begin;
  SequenceNumber seq;
};

struct ExpectedRange {
  Slice begin;
  Slice end;
  SequenceNumber seq;
};

enum Direction {
  kForward,
  kReverse,
};

void VerifyRangeDels(const std::vector<RangeTombstone>& range_dels,
                     const std::vector<ExpectedPoint>& expected_points) {
  auto icmp = InternalKeyComparator(BytewiseComparator());
  // Test same result regardless of which order the range deletions are added.
  for (Direction dir : {kForward, kReverse}) {
    RangeDelAggregator range_del_agg(icmp, {} /* snapshots */, true);
    std::vector<std::string> keys, values;
    for (const auto& range_del : range_dels) {
      auto key_and_value = range_del.Serialize();
      keys.push_back(key_and_value.first.Encode().ToString());
      values.push_back(key_and_value.second.ToString());
    }
    if (dir == kReverse) {
      std::reverse(keys.begin(), keys.end());
      std::reverse(values.begin(), values.end());
    }
    std::unique_ptr<test::VectorIterator> range_del_iter(
        new test::VectorIterator(keys, values));
    range_del_agg.AddTombstones(std::move(range_del_iter));

    for (const auto expected_point : expected_points) {
      ParsedInternalKey parsed_key;
      parsed_key.user_key = expected_point.begin;
      parsed_key.sequence = expected_point.seq;
      parsed_key.type = kTypeValue;
      ASSERT_FALSE(range_del_agg.ShouldDelete(
          parsed_key,
          RangeDelAggregator::RangePositioningMode::kForwardTraversal));
      if (parsed_key.sequence > 0) {
        --parsed_key.sequence;
        ASSERT_TRUE(range_del_agg.ShouldDelete(
            parsed_key,
            RangeDelAggregator::RangePositioningMode::kForwardTraversal));
      }
    }
  }

  RangeDelAggregator range_del_agg(icmp, {} /* snapshots */,
                                   false /* collapse_deletions */);
  std::vector<std::string> keys, values;
  for (const auto& range_del : range_dels) {
    auto key_and_value = range_del.Serialize();
    keys.push_back(key_and_value.first.Encode().ToString());
    values.push_back(key_and_value.second.ToString());
  }
  std::unique_ptr<test::VectorIterator> range_del_iter(
      new test::VectorIterator(keys, values));
  range_del_agg.AddTombstones(std::move(range_del_iter));
  for (size_t i = 1; i < expected_points.size(); ++i) {
    bool overlapped = range_del_agg.IsRangeOverlapped(
        expected_points[i - 1].begin, expected_points[i].begin);
    if (expected_points[i - 1].seq > 0 || expected_points[i].seq > 0) {
      ASSERT_TRUE(overlapped);
    } else {
      ASSERT_FALSE(overlapped);
    }
  }
}

bool ShouldDeleteRange(const std::vector<RangeTombstone>& range_dels,
                       const ExpectedRange& expected_range) {
  auto icmp = InternalKeyComparator(BytewiseComparator());
  RangeDelAggregator range_del_agg(icmp, {} /* snapshots */, true);
  std::vector<std::string> keys, values;
  for (const auto& range_del : range_dels) {
    auto key_and_value = range_del.Serialize();
    keys.push_back(key_and_value.first.Encode().ToString());
    values.push_back(key_and_value.second.ToString());
  }
  std::unique_ptr<test::VectorIterator> range_del_iter(
      new test::VectorIterator(keys, values));
  range_del_agg.AddTombstones(std::move(range_del_iter));

  std::string begin, end;
  AppendInternalKey(&begin, {expected_range.begin, expected_range.seq, kTypeValue});
  AppendInternalKey(&end, {expected_range.end, expected_range.seq, kTypeValue});
  return range_del_agg.ShouldDeleteRange(begin, end, expected_range.seq);
}

void VerifyGetTombstone(const std::vector<RangeTombstone>& range_dels,
                        const ExpectedPoint& expected_point,
                        const RangeTombstone& expected_tombstone) {
  auto icmp = InternalKeyComparator(BytewiseComparator());
  RangeDelAggregator range_del_agg(icmp, {} /* snapshots */, true);
  std::vector<std::string> keys, values;
  for (const auto& range_del : range_dels) {
    auto key_and_value = range_del.Serialize();
    keys.push_back(key_and_value.first.Encode().ToString());
    values.push_back(key_and_value.second.ToString());
  }
  std::unique_ptr<test::VectorIterator> range_del_iter(
      new test::VectorIterator(keys, values));
  range_del_agg.AddTombstones(std::move(range_del_iter));

  auto tombstone = range_del_agg.GetTombstone(expected_point.begin, expected_point.seq);
  ASSERT_EQ(expected_tombstone.start_key_.ToString(), tombstone.start_key_.ToString());
  ASSERT_EQ(expected_tombstone.end_key_.ToString(), tombstone.end_key_.ToString());
  ASSERT_EQ(expected_tombstone.seq_, tombstone.seq_);
}

}  // anonymous namespace

TEST_F(RangeDelAggregatorTest, Empty) { VerifyRangeDels({}, {{"a", 0}}); }

TEST_F(RangeDelAggregatorTest, SameStartAndEnd) {
  VerifyRangeDels({{"a", "a", 5}}, {{" ", 0}, {"a", 0}, {"b", 0}});
}

TEST_F(RangeDelAggregatorTest, Single) {
  VerifyRangeDels({{"a", "b", 10}}, {{" ", 0}, {"a", 10}, {"b", 0}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveLeft) {
  VerifyRangeDels({{"a", "c", 10}, {"b", "d", 5}},
                  {{" ", 0}, {"a", 10}, {"c", 5}, {"d", 0}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveRight) {
  VerifyRangeDels({{"a", "c", 5}, {"b", "d", 10}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"d", 0}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveMiddle) {
  VerifyRangeDels({{"a", "d", 5}, {"b", "c", 10}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"c", 5}, {"d", 0}});
}

TEST_F(RangeDelAggregatorTest, OverlapFully) {
  VerifyRangeDels({{"a", "d", 10}, {"b", "c", 5}},
                  {{" ", 0}, {"a", 10}, {"d", 0}});
}

TEST_F(RangeDelAggregatorTest, OverlapPoint) {
  VerifyRangeDels({{"a", "b", 5}, {"b", "c", 10}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"c", 0}});
}

TEST_F(RangeDelAggregatorTest, SameStartKey) {
  VerifyRangeDels({{"a", "c", 5}, {"a", "b", 10}},
                  {{" ", 0}, {"a", 10}, {"b", 5}, {"c", 0}});
}

TEST_F(RangeDelAggregatorTest, SameEndKey) {
  VerifyRangeDels({{"a", "d", 5}, {"b", "d", 10}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"d", 0}});
}

TEST_F(RangeDelAggregatorTest, GapsBetweenRanges) {
  VerifyRangeDels({{"a", "b", 5}, {"c", "d", 10}, {"e", "f", 15}}, {{" ", 0},
                                                                    {"a", 5},
                                                                    {"b", 0},
                                                                    {"c", 10},
                                                                    {"d", 0},
                                                                    {"da", 0},
                                                                    {"e", 15},
                                                                    {"f", 0}});
}

// Note the Cover* tests also test cases where tombstones are inserted under a
// larger one when VerifyRangeDels() runs them in reverse
TEST_F(RangeDelAggregatorTest, CoverMultipleFromLeft) {
  VerifyRangeDels(
      {{"b", "d", 5}, {"c", "f", 10}, {"e", "g", 15}, {"a", "f", 20}},
      {{" ", 0}, {"a", 20}, {"f", 15}, {"g", 0}});
}

TEST_F(RangeDelAggregatorTest, CoverMultipleFromRight) {
  VerifyRangeDels(
      {{"b", "d", 5}, {"c", "f", 10}, {"e", "g", 15}, {"c", "h", 20}},
      {{" ", 0}, {"b", 5}, {"c", 20}, {"h", 0}});
}

TEST_F(RangeDelAggregatorTest, CoverMultipleFully) {
  VerifyRangeDels(
      {{"b", "d", 5}, {"c", "f", 10}, {"e", "g", 15}, {"a", "h", 20}},
      {{" ", 0}, {"a", 20}, {"h", 0}});
}

TEST_F(RangeDelAggregatorTest, AlternateMultipleAboveBelow) {
  VerifyRangeDels(
      {{"b", "d", 15}, {"c", "f", 10}, {"e", "g", 20}, {"a", "h", 5}},
      {{" ", 0},
       {"a", 5},
       {"b", 15},
       {"d", 10},
       {"e", 20},
       {"g", 5},
       {"h", 0}});
}

TEST_F(RangeDelAggregatorTest, ShouldDeleteRange) {
  ASSERT_TRUE(ShouldDeleteRange(
      {{"a", "c", 10}},
      {"a", "b", 9}));
  ASSERT_TRUE(ShouldDeleteRange(
      {{"a", "c", 10}},
      {"a", "a", 9}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"a", "c", 10}},
      {"b", "a", 9}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"a", "c", 10}},
      {"a", "b", 10}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"a", "c", 10}},
      {"a", "c", 9}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"b", "c", 10}},
      {"a", "b", 9}));
  ASSERT_TRUE(ShouldDeleteRange(
      {{"a", "b", 10}, {"b", "d", 20}},
      {"a", "c", 9}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"a", "b", 10}, {"b", "d", 20}},
      {"a", "c", 15}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"a", "b", 10}, {"c", "e", 20}},
      {"a", "d", 9}));
  ASSERT_TRUE(ShouldDeleteRange(
      {{"a", "b", 10}, {"c", "e", 20}},
      {"c", "d", 15}));
  ASSERT_FALSE(ShouldDeleteRange(
      {{"a", "b", 10}, {"c", "e", 20}},
      {"c", "d", 20}));
}

TEST_F(RangeDelAggregatorTest, GetTombstone) {
  VerifyGetTombstone(
      {{"b", "d", 10}},
      {"b", 9},
      {"b", "d", 10});
  VerifyGetTombstone(
      {{"b", "d", 10}},
      {"b", 20},
      {"b", "d", 0});
  VerifyGetTombstone(
      {{"b", "d", 10}},
      {"a", 9},
      {"", "b", 0});
  VerifyGetTombstone(
      {{"b", "d", 10}},
      {"d", 9},
      {"d", "", 0});
  VerifyGetTombstone(
      {{"a", "c", 10}, {"e", "h", 20}},
      {"d", 9},
      {"c", "e", 0});
  VerifyGetTombstone(
      {{"a", "c", 10}, {"e", "h", 20}},
      {"b", 9},
      {"a", "c", 10});
  VerifyGetTombstone(
      {{"a", "c", 10}, {"e", "h", 20}},
      {"e", 9},
      {"e", "h", 20});
  VerifyGetTombstone(
      {{"a", "c", 10}, {"e", "h", 20}},
      {"e", 9},
      {"e", "h", 20});
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
