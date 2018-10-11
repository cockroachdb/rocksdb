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
  bool expectAlive;
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

struct AddTombstonesArgs {
  const std::vector<RangeTombstone> tombstones;
  const InternalKey* smallest;
  const InternalKey* largest;
};

static auto bytewise_icmp = InternalKeyComparator(BytewiseComparator());

void AddTombstones(RangeDelAggregator* range_del_agg,
                   const std::vector<RangeTombstone>& range_dels,
                   const InternalKey* smallest = nullptr,
                   const InternalKey* largest = nullptr) {
  std::vector<std::string> keys, values;
  for (const auto& range_del : range_dels) {
    auto key_and_value = range_del.Serialize();
    keys.push_back(key_and_value.first.Encode().ToString());
    values.push_back(key_and_value.second.ToString());
  }
  std::unique_ptr<test::VectorIterator> range_del_iter(
      new test::VectorIterator(keys, values));
  range_del_agg->AddTombstones(std::move(range_del_iter), smallest, largest);
}

void VerifyTombstonesEq(const RangeTombstone& a, const RangeTombstone& b) {
  ASSERT_EQ(a.seq_, b.seq_);
  ASSERT_EQ(a.start_key_, b.start_key_);
  ASSERT_EQ(a.end_key_, b.end_key_);
}

void VerifyPartialTombstonesEq(const PartialRangeTombstone& a,
                               const PartialRangeTombstone& b) {
  ASSERT_EQ(a.seq(), b.seq());
  if (a.start_key() != nullptr) {
    ASSERT_EQ(a.start_key()->user_key, b.start_key()->user_key);
    ASSERT_EQ(a.start_key()->sequence, b.start_key()->sequence);
    ASSERT_EQ(a.start_key()->type, b.start_key()->type);
  } else {
    ASSERT_EQ(b.start_key(), nullptr);
  }
  if (a.end_key() != nullptr) {
    ASSERT_EQ(a.end_key()->user_key, b.end_key()->user_key);
    ASSERT_EQ(a.end_key()->sequence, b.end_key()->sequence);
    ASSERT_EQ(a.end_key()->type, b.end_key()->type);
  } else {
    ASSERT_EQ(b.end_key(), nullptr);
  }
}

void VerifyRangeDelIter(
    RangeDelIterator* range_del_iter,
    const std::vector<RangeTombstone>& expected_range_dels) {
  size_t i = 0;
  for (; range_del_iter->Valid(); range_del_iter->Next(), i++) {
    VerifyTombstonesEq(expected_range_dels[i], range_del_iter->Tombstone());
  }
  ASSERT_EQ(expected_range_dels.size(), i);
  ASSERT_FALSE(range_del_iter->Valid());
}

void VerifyRangeDels(
    const std::vector<AddTombstonesArgs>& all_args,
    const std::vector<ExpectedPoint>& expected_points,
    const std::vector<RangeTombstone>& expected_collapsed_range_dels,
    const InternalKeyComparator& icmp = bytewise_icmp) {
  // Test same result regardless of which order the range deletions are added
  // and regardless of collapsed mode.
  for (bool collapsed : {false, true}) {
    for (Direction dir : {kForward, kReverse}) {
      RangeDelAggregator range_del_agg(icmp, {} /* snapshots */, collapsed);
      std::vector<RangeTombstone> all_range_dels;

      for (const auto& args : all_args) {
        std::vector<RangeTombstone> range_dels = args.tombstones;
        if (dir == kReverse) {
          std::reverse(range_dels.begin(), range_dels.end());
        }
        all_range_dels.insert(all_range_dels.end(), range_dels.begin(),
                              range_dels.end());
        AddTombstones(&range_del_agg, range_dels, args.smallest, args.largest);
      }

      auto mode = RangeDelPositioningMode::kFullScan;
      if (collapsed) {
        mode = RangeDelPositioningMode::kForwardTraversal;
      }

      for (const auto expected_point : expected_points) {
        ParsedInternalKey parsed_key;
        parsed_key.user_key = expected_point.begin;
        parsed_key.sequence = expected_point.seq;
        parsed_key.type = kTypeValue;
        std::string ikey;
        AppendInternalKey(&ikey, parsed_key);
        ASSERT_FALSE(range_del_agg.ShouldDelete(ikey, mode));
        if (parsed_key.sequence > 0) {
          --parsed_key.sequence;
          ikey.clear();
          AppendInternalKey(&ikey, parsed_key);
          if (expected_point.expectAlive) {
            ASSERT_FALSE(range_del_agg.ShouldDelete(ikey, mode));
          } else {
            ASSERT_TRUE(range_del_agg.ShouldDelete(ikey, mode));
          }
        }
      }

      if (collapsed) {
        all_range_dels = expected_collapsed_range_dels;
        VerifyRangeDelIter(range_del_agg.NewIterator().get(), all_range_dels);
      } else if (all_args.size() == 1 && all_args[0].smallest == nullptr &&
                 all_args[0].largest == nullptr) {
        // Tombstones in an uncollapsed map are presented in start key
        // order. Tombstones with the same start key are presented in
        // insertion order. We don't handle tombstone truncation here, so the
        // verification is only performed if no truncation was requested.
        std::stable_sort(all_range_dels.begin(), all_range_dels.end(),
                         [&](const RangeTombstone& a, const RangeTombstone& b) {
                           return icmp.user_comparator()->Compare(
                                      a.start_key_, b.start_key_) < 0;
                         });
        VerifyRangeDelIter(range_del_agg.NewIterator().get(), all_range_dels);
      }
    }
  }

  RangeDelAggregator range_del_agg(icmp, {} /* snapshots */,
                                   false /* collapse_deletions */);
  for (const auto& args : all_args) {
    AddTombstones(&range_del_agg, args.tombstones, args.smallest, args.largest);
  }
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
  RangeDelAggregator range_del_agg(bytewise_icmp, {} /* snapshots */, true);
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
                        const PartialRangeTombstone& expected_tombstone) {
  RangeDelAggregator range_del_agg(bytewise_icmp, {} /* snapshots */, true);
  ASSERT_TRUE(range_del_agg.IsEmpty());
  std::vector<std::string> keys, values;
  for (const auto& range_del : range_dels) {
    auto key_and_value = range_del.Serialize();
    keys.push_back(key_and_value.first.Encode().ToString());
    values.push_back(key_and_value.second.ToString());
  }
  std::unique_ptr<test::VectorIterator> range_del_iter(
      new test::VectorIterator(keys, values));
  range_del_agg.AddTombstones(std::move(range_del_iter));

  auto key = InternalKey(expected_point.begin, kMaxSequenceNumber, kTypeValue);
  auto tombstone = range_del_agg.GetTombstone(key.Encode(), expected_point.seq);
  VerifyPartialTombstonesEq(expected_tombstone, tombstone);
}

}  // anonymous namespace

TEST_F(RangeDelAggregatorTest, Empty) { VerifyRangeDels({}, {{"a", 0}}, {}); }

TEST_F(RangeDelAggregatorTest, SameStartAndEnd) {
  VerifyRangeDels({{{{"a", "a", 5}}}}, {{" ", 0}, {"a", 0}, {"b", 0}}, {});
}

TEST_F(RangeDelAggregatorTest, Single) {
  VerifyRangeDels({{{{"a", "b", 10}}}}, {{" ", 0}, {"a", 10}, {"b", 0}},
                  {{"a", "b", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveLeft) {
  VerifyRangeDels({{{{"a", "c", 10}, {"b", "d", 5}}}},
                  {{" ", 0}, {"a", 10}, {"c", 5}, {"d", 0}},
                  {{"a", "c", 10}, {"c", "d", 5}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveRight) {
  VerifyRangeDels({{{{"a", "c", 5}, {"b", "d", 10}}}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"d", 0}},
                  {{"a", "b", 5}, {"b", "d", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveMiddle) {
  VerifyRangeDels({{{{"a", "d", 5}, {"b", "c", 10}}}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"c", 5}, {"d", 0}},
                  {{"a", "b", 5}, {"b", "c", 10}, {"c", "d", 5}});
}

TEST_F(RangeDelAggregatorTest, OverlapAboveMiddleReverse) {
  VerifyRangeDels({{{{"d", "a", 5}, {"c", "b", 10}}}},
                  {{"z", 0}, {"d", 5}, {"c", 10}, {"b", 5}, {"a", 0}},
                  {{"d", "c", 5}, {"c", "b", 10}, {"b", "a", 5}},
                  InternalKeyComparator(ReverseBytewiseComparator()));
}

TEST_F(RangeDelAggregatorTest, OverlapFully) {
  VerifyRangeDels({{{{"a", "d", 10}, {"b", "c", 5}}}},
                  {{" ", 0}, {"a", 10}, {"d", 0}}, {{"a", "d", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlapPoint) {
  VerifyRangeDels({{{{"a", "b", 5}, {"b", "c", 10}}}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"c", 0}},
                  {{"a", "b", 5}, {"b", "c", 10}});
}

TEST_F(RangeDelAggregatorTest, SameStartKey) {
  VerifyRangeDels({{{{"a", "c", 5}, {"a", "b", 10}}}},
                  {{" ", 0}, {"a", 10}, {"b", 5}, {"c", 0}},
                  {{"a", "b", 10}, {"b", "c", 5}});
}

TEST_F(RangeDelAggregatorTest, SameEndKey) {
  VerifyRangeDels({{{{"a", "d", 5}, {"b", "d", 10}}}},
                  {{" ", 0}, {"a", 5}, {"b", 10}, {"d", 0}},
                  {{"a", "b", 5}, {"b", "d", 10}});
}

TEST_F(RangeDelAggregatorTest, GapsBetweenRanges) {
  VerifyRangeDels({{{{"a", "b", 5}, {"c", "d", 10}, {"e", "f", 15}}}},
                  {{" ", 0},
                   {"a", 5},
                   {"b", 0},
                   {"c", 10},
                   {"d", 0},
                   {"da", 0},
                   {"e", 15},
                   {"f", 0}},
                  {{"a", "b", 5}, {"c", "d", 10}, {"e", "f", 15}});
}

TEST_F(RangeDelAggregatorTest, IdenticalSameSeqNo) {
  VerifyRangeDels({{{{"a", "b", 5}, {"a", "b", 5}}}},
                  {{" ", 0}, {"a", 5}, {"b", 0}},
                  {{"a", "b", 5}});
}

TEST_F(RangeDelAggregatorTest, ContiguousSameSeqNo) {
  VerifyRangeDels({{{{"a", "b", 5}, {"b", "c", 5}}}},
                  {{" ", 0}, {"a", 5}, {"b", 5}, {"c", 0}},
                  {{"a", "c", 5}});
}

TEST_F(RangeDelAggregatorTest, OverlappingSameSeqNo) {
  VerifyRangeDels({{{{"a", "c", 5}, {"b", "d", 5}}}},
                  {{" ", 0}, {"a", 5}, {"b", 5}, {"c", 5}, {"d", 0}},
                  {{"a", "d", 5}});
}

TEST_F(RangeDelAggregatorTest, CoverSameSeqNo) {
  VerifyRangeDels({{{{"a", "d", 5}, {"b", "c", 5}}}},
                  {{" ", 0}, {"a", 5}, {"b", 5}, {"c", 5}, {"d", 0}},
                  {{"a", "d", 5}});
}

// Note the Cover* tests also test cases where tombstones are inserted under a
// larger one when VerifyRangeDels() runs them in reverse
TEST_F(RangeDelAggregatorTest, CoverMultipleFromLeft) {
  VerifyRangeDels(
      {{{{"b", "d", 5}, {"c", "f", 10}, {"e", "g", 15}, {"a", "f", 20}}}},
      {{" ", 0}, {"a", 20}, {"f", 15}, {"g", 0}},
      {{"a", "f", 20}, {"f", "g", 15}});
}

TEST_F(RangeDelAggregatorTest, CoverMultipleFromRight) {
  VerifyRangeDels(
      {{{{"b", "d", 5}, {"c", "f", 10}, {"e", "g", 15}, {"c", "h", 20}}}},
      {{" ", 0}, {"b", 5}, {"c", 20}, {"h", 0}},
      {{"b", "c", 5}, {"c", "h", 20}});
}

TEST_F(RangeDelAggregatorTest, CoverMultipleFully) {
  VerifyRangeDels(
      {{{{"b", "d", 5}, {"c", "f", 10}, {"e", "g", 15}, {"a", "h", 20}}}},
      {{" ", 0}, {"a", 20}, {"h", 0}}, {{"a", "h", 20}});
}

TEST_F(RangeDelAggregatorTest, AlternateMultipleAboveBelow) {
  VerifyRangeDels(
      {{{{"b", "d", 15}, {"c", "f", 10}, {"e", "g", 20}, {"a", "h", 5}}}},
      {{" ", 0}, {"a", 5}, {"b", 15}, {"d", 10}, {"e", 20}, {"g", 5}, {"h", 0}},
      {{"a", "b", 5},
       {"b", "d", 15},
       {"d", "e", 10},
       {"e", "g", 20},
       {"g", "h", 5}});
}

TEST_F(RangeDelAggregatorTest, MergingIteratorAllEmptyStripes) {
  for (bool collapsed : {true, false}) {
    RangeDelAggregator range_del_agg(bytewise_icmp, {1, 2}, collapsed);
    VerifyRangeDelIter(range_del_agg.NewIterator().get(), {});
  }
}

TEST_F(RangeDelAggregatorTest, MergingIteratorOverlappingStripes) {
  for (bool collapsed : {true, false}) {
    RangeDelAggregator range_del_agg(bytewise_icmp, {5, 15, 25, 35}, collapsed);
    AddTombstones(
        &range_del_agg,
        {{"d", "e", 10}, {"aa", "b", 20}, {"c", "d", 30}, {"a", "b", 10}});
    VerifyRangeDelIter(
        range_del_agg.NewIterator().get(),
        {{"a", "b", 10}, {"aa", "b", 20}, {"c", "d", 30}, {"d", "e", 10}});
  }
}

TEST_F(RangeDelAggregatorTest, MergingIteratorSeek) {
  RangeDelAggregator range_del_agg(bytewise_icmp, {5, 15},
                                   true /* collapsed */);
  AddTombstones(&range_del_agg, {{"a", "c", 10},
                                 {"b", "c", 11},
                                 {"f", "g", 10},
                                 {"c", "d", 20},
                                 {"e", "f", 20}});
  auto it = range_del_agg.NewIterator();

  // Verify seek positioning.
  it->Seek("");
  VerifyTombstonesEq(it->Tombstone(), {"a", "b", 10});
  it->Seek("a");
  VerifyTombstonesEq(it->Tombstone(), {"a", "b", 10});
  it->Seek("aa");
  VerifyTombstonesEq(it->Tombstone(), {"a", "b", 10});
  it->Seek("b");
  VerifyTombstonesEq(it->Tombstone(), {"b", "c", 11});
  it->Seek("c");
  VerifyTombstonesEq(it->Tombstone(), {"c", "d", 20});
  it->Seek("dd");
  VerifyTombstonesEq(it->Tombstone(), {"e", "f", 20});
  it->Seek("f");
  VerifyTombstonesEq(it->Tombstone(), {"f", "g", 10});
  it->Seek("g");
  ASSERT_EQ(it->Valid(), false);
  it->Seek("h");
  ASSERT_EQ(it->Valid(), false);

  // Verify iteration after seek.
  it->Seek("c");
  VerifyRangeDelIter(it.get(),
                     {{"c", "d", 20}, {"e", "f", 20}, {"f", "g", 10}});
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
  ParsedInternalKey a = {"a", kMaxSequenceNumber, kMaxValue};
  ParsedInternalKey b = {"b", kMaxSequenceNumber, kMaxValue};
  ParsedInternalKey c = {"c", kMaxSequenceNumber, kMaxValue};
  ParsedInternalKey d = {"d", kMaxSequenceNumber, kMaxValue};
  ParsedInternalKey e = {"e", kMaxSequenceNumber, kMaxValue};
  ParsedInternalKey h = {"h", kMaxSequenceNumber, kMaxValue};
  VerifyGetTombstone({{"b", "d", 10}}, {"b", 9},
                     PartialRangeTombstone(&b, &d, 10));
  VerifyGetTombstone({{"b", "d", 10}}, {"b", 10},
                     PartialRangeTombstone(&b, &d, 0));
  VerifyGetTombstone({{"b", "d", 10}}, {"b", 20},
                     PartialRangeTombstone(&b, &d, 0));
  VerifyGetTombstone({{"b", "d", 10}}, {"a", 9},
                     PartialRangeTombstone(nullptr, &b, 0));
  VerifyGetTombstone({{"b", "d", 10}}, {"d", 9},
                     PartialRangeTombstone(&d, nullptr, 0));
  VerifyGetTombstone({{"a", "c", 10}, {"e", "h", 20}}, {"d", 9},
                     PartialRangeTombstone(&c, &e, 0));
  VerifyGetTombstone({{"a", "c", 10}, {"e", "h", 20}}, {"b", 9},
                     PartialRangeTombstone(&a, &c, 10));
  VerifyGetTombstone({{"a", "c", 10}, {"e", "h", 20}}, {"b", 10},
                     PartialRangeTombstone(&a, &c, 0));
  VerifyGetTombstone({{"a", "c", 10}, {"e", "h", 20}}, {"e", 19},
                     PartialRangeTombstone(&e, &h, 20));
  VerifyGetTombstone({{"a", "c", 10}, {"e", "h", 20}}, {"e", 20},
                     PartialRangeTombstone(&e, &h, 0));
}

TEST_F(RangeDelAggregatorTest, AddGetTombstoneInterleaved) {
  RangeDelAggregator range_del_agg(bytewise_icmp, {} /* snapshots */,
                                   true /* collapsed */);
  AddTombstones(&range_del_agg, {{"b", "c", 10}});
  auto key = InternalKey("b", kMaxSequenceNumber, kTypeValue);
  auto tombstone = range_del_agg.GetTombstone(key.Encode(), 5);
  AddTombstones(&range_del_agg, {{"a", "d", 20}});
  ParsedInternalKey b = {"b", kMaxSequenceNumber, kMaxValue};
  ParsedInternalKey c {"c", kMaxSequenceNumber, kMaxValue};
  VerifyPartialTombstonesEq(PartialRangeTombstone(&b, &c, 10), tombstone);
}

TEST_F(RangeDelAggregatorTest, TruncateTombstones) {
  const InternalKey smallest("b", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest("e", kMaxSequenceNumber, kTypeRangeDeletion);
  VerifyRangeDels(
      {{{{"a", "c", 10}, {"d", "f", 10}}, &smallest, &largest}},
      {{"a", 10, true},  // truncated
       {"b", 10, false}, // not truncated
       {"d", 10, false}, // not truncated
       {"e", 10, true}}, // truncated
      {{"b", "c", 10}, {"d", "e", 10}});
}

TEST_F(RangeDelAggregatorTest, IsEmpty) {
  const std::vector<SequenceNumber> snapshots;
  RangeDelAggregator range_del_agg1(
      bytewise_icmp, snapshots, false /* collapse_deletions */);
  ASSERT_TRUE(range_del_agg1.IsEmpty());

  RangeDelAggregator range_del_agg2(
      bytewise_icmp, snapshots, true /* collapse_deletions */);
  ASSERT_TRUE(range_del_agg2.IsEmpty());

  RangeDelAggregator range_del_agg3(
      bytewise_icmp, kMaxSequenceNumber, false /* collapse_deletions */);
  ASSERT_TRUE(range_del_agg3.IsEmpty());

  RangeDelAggregator range_del_agg4(
      bytewise_icmp, kMaxSequenceNumber, true /* collapse_deletions */);
  ASSERT_TRUE(range_del_agg4.IsEmpty());
}

TEST_F(RangeDelAggregatorTest, OverlappingLargestKeyTruncateBelowTombstone) {
  const InternalKey smallest("b", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest(
      "e", 3,  // could happen if "e" is in consecutive sstables
      kTypeValue);
  VerifyRangeDels(
      {{{{"a", "c", 10}, {"d", "f", 10}}, &smallest, &largest}},
      {{"a", 10, true},  // truncated
       {"b", 10, false}, // not truncated
       {"d", 10, false}, // not truncated
       {"e", 10, false}, // not truncated
       {"e", 2, true}},  // truncated here
      {{"b", "c", 10}, {"d", "e", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlappingLargestKeyTruncateAboveTombstone) {
  const InternalKey smallest("b", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest(
      "e", 15,  // could happen if "e" is in consecutive sstables
      kTypeValue);
  VerifyRangeDels(
      {{{{"a", "c", 10}, {"d", "f", 10}}, &smallest, &largest}},
      {{"a", 10, true},  // truncated
       {"b", 10, false}, // not truncated
       {"d", 10, false}, // not truncated
       {"e", kMaxSequenceNumber, true}},  // truncated
      {{"b", "c", 10}, {"d", "e", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlappingSmallestKeyTruncateBelowTombstone) {
  const InternalKey smallest("b", 5, kTypeValue);
  const InternalKey largest("e", kMaxSequenceNumber, kTypeRangeDeletion);
  VerifyRangeDels(
      {{{{"a", "c", 10}, {"d", "f", 10}}, &smallest, &largest}},
      {{"a", 10, true},  // truncated
       {"b", 10, true}, // truncated
       {"b", 6, false}, // not truncated; start boundary moved
       {"d", 10, false}, // not truncated
       {"e", kMaxSequenceNumber, true}}, // truncated
      {{"b", "c", 10}, {"d", "e", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlappingSmallestKeyTruncateAboveTombstone) {
  const InternalKey smallest("b", 15, kTypeValue);
  const InternalKey largest("e", kMaxSequenceNumber, kTypeRangeDeletion);
  VerifyRangeDels(
      {{{{"a", "c", 10}, {"d", "f", 10}}, &smallest, &largest}},
      {{"a", 10, true},  // truncated
       {"b", 15, true}, // truncated
       {"b", 10, false}, // not truncated
       {"d", 10, false}, // not truncated
       {"e", kMaxSequenceNumber, true}}, // truncated
      {{"b", "c", 10}, {"d", "e", 10}});
}

TEST_F(RangeDelAggregatorTest, OverlappingBoundaryGapAboveTombstone) {
  const InternalKey smallest1("b", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest1("c", 20, kTypeValue);
  const InternalKey smallest2("c", 10, kTypeValue);
  const InternalKey largest2("e", kMaxSequenceNumber, kTypeRangeDeletion);
  VerifyRangeDels(
      {{{{"b", "d", 5}}, &smallest1, &largest1},
       {{{"b", "d", 5}}, &smallest2, &largest2}},
      {{"b", 5, false}, // not truncated
       {"c", 5, false}}, // not truncated
      {{"b", "c", 5}, {"c", "d", 5}}); // not collapsed due to boundaries
}

TEST_F(RangeDelAggregatorTest, OverlappingBoundaryGapBelowTombstone) {
  const InternalKey smallest1("b", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest1("c", 20, kTypeValue);
  const InternalKey smallest2("c", 10, kTypeValue);
  const InternalKey largest2("e", kMaxSequenceNumber, kTypeRangeDeletion);
  VerifyRangeDels(
      {{{{"b", "d", 30}}, &smallest1, &largest1},
       {{{"b", "d", 30}}, &smallest2, &largest2}},
      {{"b", 30, false}, // not truncated
       {"c", 30, false}, // not truncated
       {"c", 19, true}, // truncated here (keys in this range should not exist)
       {"c", 11, false}}, // not truncated again
      {{"b", "c", 30}, {"c", "d", 30}}); // not collapsed due to boundaries
}

TEST_F(RangeDelAggregatorTest, OverlappingBoundaryGapContainsTombstone) {
  const InternalKey smallest1("b", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest1("c", 20, kTypeValue);
  const InternalKey smallest2("c", 10, kTypeValue);
  const InternalKey largest2("e", kMaxSequenceNumber, kTypeRangeDeletion);
  VerifyRangeDels(
      {{{{"b", "d", 15}}, &smallest1, &largest1},
       {{{"b", "d", 15}}, &smallest2, &largest2}},
      {{"b", 15, false}, // not truncated
       {"c", 15, true}, // truncated (keys in this range should not exist)
       {"c", 11, false}}, // not truncated here
      {{"b", "c", 15}, {"c", "d", 15}}); // not collapsed due to boundaries
}

TEST_F(RangeDelAggregatorTest, FileCoversOneKeyAndTombstoneAbove) {
  const InternalKey smallest("a", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest("a", 20, kTypeValue);
  VerifyRangeDels(
      {{{{"a", "b", 35}}, &smallest, &largest}},
      {{"a", 40, true}, // not truncated
       {"a", 35, false}}, // not truncated
      {{"a", "a", 35}}); // empty tombstone but can't occur during a compaction
}

TEST_F(RangeDelAggregatorTest, FileCoversOneKeyAndTombstoneBelow) {
  const InternalKey smallest("a", kMaxSequenceNumber, kTypeRangeDeletion);
  const InternalKey largest("a", 20, kTypeValue);
  VerifyRangeDels(
      {{{{"a", "b", 15}}, &smallest, &largest}},
      {{"a", 20, true}, // truncated here
       {"a", 15, true}}, // truncated
      {{"a", "a", 15}}); // empty tombstone but can't occur during a compaction
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
