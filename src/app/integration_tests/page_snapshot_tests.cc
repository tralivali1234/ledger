// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>
#include <vector>

#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/app/integration_tests/test_utils.h"
#include "apps/ledger/src/convert/convert.h"
#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/vmo/strings.h"

namespace ledger {
namespace integration_tests {
namespace {

class PageSnapshotIntegrationTest : public LedgerApplicationBaseTest {
 public:
  PageSnapshotIntegrationTest() {}
  ~PageSnapshotIntegrationTest() override {}

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(PageSnapshotIntegrationTest);
};

TEST_F(PageSnapshotIntegrationTest, PageSnapshotGet) {
  PagePtr page = GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  ValuePtr value;
  snapshot->Get(convert::ToArray("name"), [&value](Status status, ValuePtr v) {
    EXPECT_EQ(status, Status::OK);
    value = std::move(v);
  });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
  EXPECT_TRUE(value->is_bytes());
  EXPECT_EQ("Alice", convert::ToString(value->get_bytes()));

  // Attempt to get an entry that is not in the page.
  snapshot->Get(convert::ToArray("favorite book"),
                [](Status status, ValuePtr v) {
                  // People don't read much these days.
                  EXPECT_EQ(status, Status::KEY_NOT_FOUND);
                });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
}

TEST_F(PageSnapshotIntegrationTest, PageSnapshotGetPartial) {
  PagePtr page = GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  EXPECT_EQ("Alice",
            SnapshotGetPartial(&snapshot, convert::ToArray("name"), 0, -1));
  EXPECT_EQ("e",
            SnapshotGetPartial(&snapshot, convert::ToArray("name"), 4, -1));
  EXPECT_EQ("", SnapshotGetPartial(&snapshot, convert::ToArray("name"), 5, -1));
  EXPECT_EQ("", SnapshotGetPartial(&snapshot, convert::ToArray("name"), 6, -1));
  EXPECT_EQ("i", SnapshotGetPartial(&snapshot, convert::ToArray("name"), 2, 1));
  EXPECT_EQ("", SnapshotGetPartial(&snapshot, convert::ToArray("name"), 2, 0));

  // Negative offsets.
  EXPECT_EQ("Alice",
            SnapshotGetPartial(&snapshot, convert::ToArray("name"), -5, -1));
  EXPECT_EQ("e",
            SnapshotGetPartial(&snapshot, convert::ToArray("name"), -1, -1));
  EXPECT_EQ("", SnapshotGetPartial(&snapshot, convert::ToArray("name"), -5, 0));
  EXPECT_EQ("i",
            SnapshotGetPartial(&snapshot, convert::ToArray("name"), -3, 1));

  // Attempt to get an entry that is not in the page.
  snapshot->GetPartial(convert::ToArray("favorite book"), 0, -1,
                       [](Status status, mx::vmo received_buffer) {
                         // People don't read much these days.
                         EXPECT_EQ(Status::KEY_NOT_FOUND, status);
                       });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
}

TEST_F(PageSnapshotIntegrationTest, PageSnapshotGetKeys) {
  PagePtr page = GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetKeys()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fidl::Array<fidl::Array<uint8_t>> result =
      SnapshotGetKeys(&snapshot, fidl::Array<uint8_t>());
  EXPECT_EQ(0u, result.size());

  // Add entries and grab a new snapshot.
  const size_t N = 4;
  fidl::Array<uint8_t> keys[N] = {
      RandomArray(20, {0, 0, 0}), RandomArray(20, {0, 0, 1}),
      RandomArray(20, {0, 1, 0}), RandomArray(20, {0, 1, 1}),
  };
  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i].Clone(), RandomArray(50),
              [](Status status) { EXPECT_EQ(status, Status::OK); });
    EXPECT_TRUE(page.WaitForIncomingResponse());
  }
  snapshot = PageGetSnapshot(&page);

  // Get all keys.
  result = SnapshotGetKeys(&snapshot, fidl::Array<uint8_t>());
  EXPECT_EQ(N, result.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_TRUE(keys[i].Equals(result[i]));
  }

  // Get keys matching the prefix "0".
  result = SnapshotGetKeys(&snapshot,
                           fidl::Array<uint8_t>::From(std::vector<uint8_t>{0}));
  EXPECT_EQ(N, result.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_TRUE(keys[i].Equals(result[i]));
  }

  // Get keys matching the prefix "00".
  result = SnapshotGetKeys(
      &snapshot, fidl::Array<uint8_t>::From(std::vector<uint8_t>{0, 0}));
  EXPECT_EQ(2u, result.size());
  for (size_t i = 0; i < 2u; ++i) {
    EXPECT_TRUE(keys[i].Equals(result[i]));
  }

  // Get keys matching the prefix "010".
  result = SnapshotGetKeys(
      &snapshot, fidl::Array<uint8_t>::From(std::vector<uint8_t>{0, 1, 0}));
  EXPECT_EQ(1u, result.size());
  EXPECT_TRUE(keys[2].Equals(result[0]));

  // Get keys matching the prefix "5".
  result = SnapshotGetKeys(&snapshot,
                           fidl::Array<uint8_t>::From(std::vector<uint8_t>{5}));
  EXPECT_EQ(0u, result.size());
}

TEST_F(PageSnapshotIntegrationTest, PageSnapshotGetEntries) {
  PagePtr page = GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetEntries()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fidl::Array<EntryPtr> entries =
      SnapshotGetEntries(&snapshot, fidl::Array<uint8_t>());
  EXPECT_EQ(0u, entries.size());

  // Add entries and grab a new snapshot.
  const size_t N = 4;
  fidl::Array<uint8_t> keys[N] = {
      RandomArray(20, {0, 0, 0}), RandomArray(20, {0, 0, 1}),
      RandomArray(20, {0, 1, 0}), RandomArray(20, {0, 1, 1}),
  };
  fidl::Array<uint8_t> values[N] = {
      RandomArray(50), RandomArray(50), RandomArray(50), RandomArray(50),
  };
  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i].Clone(), values[i].Clone(),
              [](Status status) { EXPECT_EQ(status, Status::OK); });
    EXPECT_TRUE(page.WaitForIncomingResponse());
  }
  snapshot = PageGetSnapshot(&page);

  // Get all entries.
  entries = SnapshotGetEntries(&snapshot, fidl::Array<uint8_t>());
  EXPECT_EQ(N, entries.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_TRUE(keys[i].Equals(entries[i]->key));
    EXPECT_TRUE(values[i].Equals(entries[i]->value->get_bytes()));
  }

  // Get entries matching the prefix "0".
  entries = SnapshotGetEntries(
      &snapshot, fidl::Array<uint8_t>::From(std::vector<uint8_t>{0}));
  EXPECT_EQ(N, entries.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_TRUE(keys[i].Equals(entries[i]->key));
    EXPECT_TRUE(values[i].Equals(entries[i]->value->get_bytes()));
  }

  // Get entries matching the prefix "00".
  entries = SnapshotGetEntries(
      &snapshot, fidl::Array<uint8_t>::From(std::vector<uint8_t>{0, 0}));
  EXPECT_EQ(2u, entries.size());
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_TRUE(keys[i].Equals(entries[i]->key));
    EXPECT_TRUE(values[i].Equals(entries[i]->value->get_bytes()));
  }

  // Get keys matching the prefix "010".
  entries = SnapshotGetEntries(
      &snapshot, fidl::Array<uint8_t>::From(std::vector<uint8_t>{0, 1, 0}));
  EXPECT_EQ(1u, entries.size());
  EXPECT_TRUE(keys[2].Equals(entries[0]->key));
  EXPECT_TRUE(values[2].Equals(entries[0]->value->get_bytes()));

  // Get keys matching the prefix "5".
  snapshot->GetEntries(fidl::Array<uint8_t>::From(std::vector<uint8_t>{5}),
                       nullptr,
                       [&entries](Status status, fidl::Array<EntryPtr> e,
                                  fidl::Array<uint8_t> next_token) {
                         EXPECT_EQ(Status::OK, status);
                         EXPECT_TRUE(next_token.is_null());
                         entries = std::move(e);
                       });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
  EXPECT_EQ(0u, entries.size());
}

TEST_F(PageSnapshotIntegrationTest, PageSnapshotGettersReturnSortedEntries) {
  PagePtr page = GetTestPage();

  const size_t N = 4;
  fidl::Array<uint8_t> keys[N] = {
      RandomArray(20, {2}), RandomArray(20, {5}), RandomArray(20, {3}),
      RandomArray(20, {0}),
  };
  fidl::Array<uint8_t> values[N] = {
      RandomArray(20), RandomArray(20), RandomArray(20), RandomArray(20),
  };
  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i].Clone(), values[i].Clone(),
              [](Status status) { EXPECT_EQ(status, Status::OK); });
    EXPECT_TRUE(page.WaitForIncomingResponse());
  }

  // Get a snapshot.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);

  // Verify that GetKeys() results are sorted.
  fidl::Array<fidl::Array<uint8_t>> result =
      SnapshotGetKeys(&snapshot, fidl::Array<uint8_t>());
  EXPECT_TRUE(keys[3].Equals(result[0]));
  EXPECT_TRUE(keys[0].Equals(result[1]));
  EXPECT_TRUE(keys[2].Equals(result[2]));
  EXPECT_TRUE(keys[1].Equals(result[3]));

  // Verify that GetEntries() results are sorted.
  fidl::Array<EntryPtr> entries =
      SnapshotGetEntries(&snapshot, fidl::Array<uint8_t>());
  EXPECT_TRUE(keys[3].Equals(entries[0]->key));
  EXPECT_TRUE(values[3].Equals(entries[0]->value->get_bytes()));
  EXPECT_TRUE(keys[0].Equals(entries[1]->key));
  EXPECT_TRUE(values[0].Equals(entries[1]->value->get_bytes()));
  EXPECT_TRUE(keys[2].Equals(entries[2]->key));
  EXPECT_TRUE(values[2].Equals(entries[2]->value->get_bytes()));
  EXPECT_TRUE(keys[1].Equals(entries[3]->key));
  EXPECT_TRUE(values[1].Equals(entries[3]->value->get_bytes()));
}

TEST_F(PageSnapshotIntegrationTest, PageCreateReferenceNegativeSize) {
  const std::string big_data(1'000'000, 'a');

  PagePtr page = GetTestPage();

  page->CreateReference(-1, StreamDataToSocket(big_data),
                        [this](Status status, ReferencePtr ref) {
                          EXPECT_EQ(Status::OK, status);
                        });
  ASSERT_TRUE(page.WaitForIncomingResponse());
}

TEST_F(PageSnapshotIntegrationTest, PageCreateReferenceWrongSize) {
  const std::string big_data(1'000'000, 'a');

  PagePtr page = GetTestPage();

  page->CreateReference(123, StreamDataToSocket(big_data),
                        [this](Status status, ReferencePtr ref) {
                          EXPECT_EQ(Status::IO_ERROR, status);
                        });
  ASSERT_TRUE(page.WaitForIncomingResponse());
}

TEST_F(PageSnapshotIntegrationTest, PageCreatePutLargeReference) {
  const std::string big_data(1'000'000, 'a');

  PagePtr page = GetTestPage();

  // Stream the data into the reference.
  ReferencePtr reference;
  page->CreateReference(big_data.size(), StreamDataToSocket(big_data),
                        [this, &reference](Status status, ReferencePtr ref) {
                          EXPECT_EQ(Status::OK, status);
                          reference = std::move(ref);
                        });
  ASSERT_TRUE(page.WaitForIncomingResponse());

  // Set the reference uder a key.
  page->PutReference(convert::ToArray("big data"), std::move(reference),
                     Priority::EAGER,
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  ASSERT_TRUE(page.WaitForIncomingResponse());

  // Get a snapshot and read the value.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  ValuePtr value;
  snapshot->Get(convert::ToArray("big data"),
                [&value](Status status, ValuePtr v) {
                  EXPECT_EQ(status, Status::OK);
                  value = std::move(v);
                });
  ASSERT_TRUE(snapshot.WaitForIncomingResponse());

  EXPECT_FALSE(value->is_bytes());
  EXPECT_TRUE(value->is_buffer());
  std::string retrieved_data;
  EXPECT_TRUE(mtl::StringFromVmo(value->get_buffer(), &retrieved_data));
  EXPECT_EQ(big_data, retrieved_data);
}

TEST_F(PageSnapshotIntegrationTest, PageSnapshotClosePageGet) {
  PagePtr page = GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);

  // Close the channel. PageSnapshotPtr should remain valid.
  page.reset();

  ValuePtr value;
  snapshot->Get(convert::ToArray("name"), [&value](Status status, ValuePtr v) {
    EXPECT_EQ(status, Status::OK);
    value = std::move(v);
  });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
  EXPECT_TRUE(value->is_bytes());
  EXPECT_EQ("Alice", convert::ToString(value->get_bytes()));

  // Attempt to get an entry that is not in the page.
  snapshot->Get(convert::ToArray("favorite book"),
                [](Status status, ValuePtr v) {
                  // People don't read much these days.
                  EXPECT_EQ(status, Status::KEY_NOT_FOUND);
                });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
}

TEST_F(PageSnapshotIntegrationTest, PageGetById) {
  PagePtr page = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page.reset();

  page = GetPage(test_page_id, Status::OK);
  page->GetId([&test_page_id, this](fidl::Array<uint8_t> page_id) {
    EXPECT_EQ(convert::ToString(test_page_id), convert::ToString(page_id));
  });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  ValuePtr value;
  snapshot->Get(convert::ToArray("name"),
                [&value, this](Status status, ValuePtr v) {
                  EXPECT_EQ(status, Status::OK);
                  value = std::move(v);
                });
  EXPECT_TRUE(snapshot.WaitForIncomingResponse());
  EXPECT_TRUE(value->is_bytes());
  EXPECT_EQ("Alice", convert::ToString(value->get_bytes()));
}

}  // namespace
}  // namespace integration_tests
}  // namespace ledger
