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
#include "lib/ftl/time/time_delta.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {
namespace integration_tests {
namespace {

class PageWatcherIntegrationTest : public LedgerApplicationBaseTest {
 public:
  PageWatcherIntegrationTest() {}
  ~PageWatcherIntegrationTest() override {}

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(PageWatcherIntegrationTest);
};

class Watcher : public PageWatcher {
 public:
  Watcher(fidl::InterfaceRequest<PageWatcher> request,
          ftl::Closure change_callback)
      : binding_(this, std::move(request)), change_callback_(change_callback) {}

  uint changes_seen = 0;
  PageSnapshotPtr last_snapshot_;
  PageChangePtr last_page_change_;

 private:
  // PageWatcher:
  void OnInitialState(fidl::InterfaceHandle<PageSnapshot> snapshot,
                      const OnInitialStateCallback& callback) override {
    callback();
  }

  void OnChange(PageChangePtr page_change,
                const OnChangeCallback& callback) override {
    FTL_DCHECK(page_change);
    changes_seen++;
    last_page_change_ = std::move(page_change);
    last_snapshot_.reset();
    callback(last_snapshot_.NewRequest());
    change_callback_();
  }

  fidl::Binding<PageWatcher> binding_;
  ftl::Closure change_callback_;
};

TEST_F(PageWatcherIntegrationTest, PageWatcherSimple) {
  PagePtr page = GetTestPage();
  PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });

  page->Watch(std::move(watcher_ptr),
              [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  PageChangePtr change = std::move(watcher.last_page_change_);
  EXPECT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", convert::ToString(change->changes[0]->value->get_bytes()));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherSnapshot) {
  PagePtr page = GetTestPage();
  PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });

  page->Watch(std::move(watcher_ptr),
              [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  fidl::Array<EntryPtr> entries =
      SnapshotGetEntries(&(watcher.last_snapshot_), convert::ToArray(""));
  EXPECT_EQ(1u, entries.size());
  EXPECT_EQ("name", convert::ToString(entries[0]->key));
  EXPECT_EQ("Alice", convert::ToString(entries[0]->value->get_bytes()));
  EXPECT_EQ(Priority::EAGER, entries[0]->priority);
}

TEST_F(PageWatcherIntegrationTest, PageWatcherTransaction) {
  PagePtr page = GetTestPage();
  PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });

  page->Watch(std::move(watcher_ptr),
              [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"),
            [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [] { mtl::MessageLoop::GetCurrent()->QuitNow(); },
      ftl::TimeDelta::FromSeconds(1));
  mtl::MessageLoop::GetCurrent()->Run();
  EXPECT_EQ(0u, watcher.changes_seen);

  page->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher.changes_seen);
  PageChangePtr change = std::move(watcher.last_page_change_);
  EXPECT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", convert::ToString(change->changes[0]->value->get_bytes()));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherParallel) {
  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PagePtr page2 = GetPage(test_page_id, Status::OK);

  PageWatcherPtr watcher1_ptr;
  Watcher watcher1(watcher1_ptr.NewRequest(),
                   [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });
  page1->Watch(std::move(watcher1_ptr),
               [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PageWatcherPtr watcher2_ptr;
  Watcher watcher2(watcher2_ptr.NewRequest(),
                   [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });
  page2->Watch(std::move(watcher2_ptr),
               [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("name"), convert::ToArray("Bob"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  // Verify that each change is seen by the right watcher.
  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  mtl::MessageLoop::GetCurrent()->Run();
  EXPECT_EQ(1u, watcher1.changes_seen);
  PageChangePtr change = std::move(watcher1.last_page_change_);
  EXPECT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", convert::ToString(change->changes[0]->value->get_bytes()));

  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  mtl::MessageLoop::GetCurrent()->Run();

  EXPECT_EQ(1u, watcher2.changes_seen);
  change = std::move(watcher2.last_page_change_);
  EXPECT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", convert::ToString(change->changes[0]->value->get_bytes()));

  mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [] { mtl::MessageLoop::GetCurrent()->QuitNow(); },
      ftl::TimeDelta::FromSeconds(1));
  mtl::MessageLoop::GetCurrent()->Run();
  // A merge happens now. Only the first watcher should see a change.
  EXPECT_EQ(2u, watcher1.changes_seen);
  EXPECT_EQ(1u, watcher2.changes_seen);

  change = std::move(watcher1.last_page_change_);
  EXPECT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", convert::ToString(change->changes[0]->value->get_bytes()));
}

TEST_F(PageWatcherIntegrationTest, PageWatcherEmptyTransaction) {
  PagePtr page = GetTestPage();
  PageWatcherPtr watcher_ptr;
  Watcher watcher(watcher_ptr.NewRequest(),
                  [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });

  page->Watch(std::move(watcher_ptr),
              [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());

  page->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page.WaitForIncomingResponse());
  mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [] { mtl::MessageLoop::GetCurrent()->QuitNow(); },
      ftl::TimeDelta::FromSeconds(1));
  mtl::MessageLoop::GetCurrent()->Run();
  EXPECT_EQ(0u, watcher.changes_seen);
}

TEST_F(PageWatcherIntegrationTest, PageWatcher1Change2Pages) {
  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PagePtr page2 = GetPage(test_page_id, Status::OK);

  PageWatcherPtr watcher1_ptr;
  Watcher watcher1(watcher1_ptr.NewRequest(),
                   [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });
  page1->Watch(std::move(watcher1_ptr),
               [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PageWatcherPtr watcher2_ptr;
  Watcher watcher2(watcher2_ptr.NewRequest(),
                   [this]() { mtl::MessageLoop::GetCurrent()->QuitNow(); });
  page2->Watch(std::move(watcher2_ptr),
               [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(1u, watcher1.changes_seen);
  PageChangePtr change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", convert::ToString(change->changes[0]->value->get_bytes()));

  ASSERT_EQ(1u, watcher2.changes_seen);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Alice", convert::ToString(change->changes[0]->value->get_bytes()));
}

}  // namespace
}  // namespace integration_tests
}  // namespace ledger
