// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/journal_db_impl.h"

#include <functional>
#include <string>

#include "apps/ledger/src/callback/waiter.h"
#include "apps/ledger/src/storage/impl/btree/builder.h"
#include "apps/ledger/src/storage/impl/commit_impl.h"
#include "apps/ledger/src/storage/impl/db.h"
#include "apps/ledger/src/storage/public/commit.h"
#include "lib/ftl/functional/make_copyable.h"

namespace storage {

JournalDBImpl::JournalDBImpl(JournalType type,
                             coroutine::CoroutineService* coroutine_service,
                             PageStorageImpl* page_storage,
                             DB* db,
                             const JournalId& id,
                             const CommitId& base)
    : type_(type),
      coroutine_service_(coroutine_service),
      page_storage_(page_storage),
      db_(db),
      id_(id),
      base_(base),
      valid_(true),
      failed_operation_(false) {}

JournalDBImpl::~JournalDBImpl() {
  // Log a warning if the journal was not committed or rolled back.
  if (valid_) {
    FTL_LOG(WARNING) << "Journal not committed or rolled back.";
  }
}

std::unique_ptr<Journal> JournalDBImpl::Simple(
    JournalType type,
    coroutine::CoroutineService* coroutine_service,
    PageStorageImpl* page_storage,
    DB* db,
    const JournalId& id,
    const CommitId& base) {
  return std::unique_ptr<Journal>(
      new JournalDBImpl(type, coroutine_service, page_storage, db, id, base));
}

std::unique_ptr<Journal> JournalDBImpl::Merge(
    coroutine::CoroutineService* coroutine_service,
    PageStorageImpl* page_storage,
    DB* db,
    const JournalId& id,
    const CommitId& base,
    const CommitId& other) {
  JournalDBImpl* db_journal = new JournalDBImpl(
      JournalType::EXPLICIT, coroutine_service, page_storage, db, id, base);
  db_journal->other_ = std::make_unique<CommitId>(other);
  std::unique_ptr<Journal> journal(db_journal);
  return journal;
}

const JournalId& JournalDBImpl::GetId() const {
  return id_;
}

Status JournalDBImpl::UpdateValueCounter(
    ObjectIdView object_id,
    const std::function<int64_t(int64_t)>& operation) {
  // Update the counter fo untracked objects only.
  if (!page_storage_->ObjectIsUntracked(object_id)) {
    return Status::OK;
  }
  int64_t counter;
  Status s = db_->GetJournalValueCounter(id_, object_id, &counter);
  if (s != Status::OK) {
    return s;
  }
  int64_t next_counter = operation(counter);
  FTL_DCHECK(next_counter >= 0);
  s = db_->SetJournalValueCounter(id_, object_id, next_counter);
  return s;
}

Status JournalDBImpl::Put(convert::ExtendedStringView key,
                          ObjectIdView object_id,
                          KeyPriority priority) {
  if (!valid_ || (type_ == JournalType::EXPLICIT && failed_operation_)) {
    return Status::ILLEGAL_STATE;
  }
  std::string prev_id;
  Status prev_entry_status = db_->GetJournalValue(id_, key, &prev_id);

  std::unique_ptr<DB::Batch> batch = db_->StartBatch();
  Status s = db_->AddJournalEntry(id_, key, object_id, priority);
  if (s != Status::OK) {
    failed_operation_ = true;
    return s;
  }
  if (object_id != prev_id) {
    UpdateValueCounter(object_id, [](int64_t counter) { return counter + 1; });
    if (prev_entry_status == Status::OK) {
      UpdateValueCounter(prev_id, [](int64_t counter) { return counter - 1; });
    }
  }
  return batch->Execute();
}

Status JournalDBImpl::Delete(convert::ExtendedStringView key) {
  if (!valid_ || (type_ == JournalType::EXPLICIT && failed_operation_)) {
    return Status::ILLEGAL_STATE;
  }
  std::string prev_id;
  Status prev_entry_status = db_->GetJournalValue(id_, key, &prev_id);

  std::unique_ptr<DB::Batch> batch = db_->StartBatch();
  Status s = db_->RemoveJournalEntry(id_, key);
  if (s != Status::OK) {
    failed_operation_ = true;
    return s;
  }

  if (prev_entry_status == Status::OK) {
    UpdateValueCounter(prev_id, [](int64_t counter) { return counter - 1; });
  }
  return batch->Execute();
}

void JournalDBImpl::GetParents(
    std::function<void(Status,
                       std::vector<std::unique_ptr<const storage::Commit>>)>
        callback) {
  auto waiter =
      callback::Waiter<Status, std::unique_ptr<const storage::Commit>>::Create(
          Status::OK);
  page_storage_->GetCommit(base_, waiter->NewCallback());
  if (other_) {
    page_storage_->GetCommit(*other_, waiter->NewCallback());
  }
  waiter->Finalize(std::move(callback));
}

Status JournalDBImpl::ClearCommittedJournal(
    std::unordered_set<ObjectId> new_nodes) {
  // Mark objects as unsynced in a single batch.
  std::vector<ObjectId> objects_to_sync;
  Status status = db_->GetJournalValues(id_, &objects_to_sync);
  if (status != Status::OK) {
    return status;
  }
  std::unique_ptr<DB::Batch> batch = db_->StartBatch();
  for (const ObjectId& tree_node_id : new_nodes) {
    status = db_->MarkObjectIdUnsynced(tree_node_id);
    if (status != Status::OK) {
      return status;
    }
  }
  for (const ObjectId& object_id : objects_to_sync) {
    status = db_->MarkObjectIdUnsynced(object_id);
    if (status != Status::OK) {
      return status;
    }
  }
  status = batch->Execute();
  if (status != Status::OK) {
    return status;
  }
  // Notify PageStorage that the objects are now tracked.
  for (const ObjectId& object_id : objects_to_sync) {
    page_storage_->MarkObjectTracked(object_id);
  }
  db_->RemoveJournal(id_);
  return Status::OK;
}

void JournalDBImpl::Commit(
    std::function<void(Status, std::unique_ptr<const storage::Commit>)>
        callback) {
  if (!valid_ || (type_ == JournalType::EXPLICIT && failed_operation_)) {
    callback(Status::ILLEGAL_STATE, nullptr);
    return;
  }

  GetParents([ this, callback = std::move(callback) ](
      Status status,
      std::vector<std::unique_ptr<const storage::Commit>> parents) {
    if (status != Status::OK) {
      callback(status, nullptr);
      return;
    }
    std::unique_ptr<Iterator<const EntryChange>> entries;
    status = db_->GetJournalEntries(id_, &entries);
    if (status != Status::OK) {
      callback(status, nullptr);
      return;
    }
    btree::ApplyChanges(
        coroutine_service_, page_storage_, parents[0]->GetRootId(),
        std::move(entries),
        ftl::MakeCopyable([
          this, parents = std::move(parents), callback = std::move(callback)
        ](Status status, ObjectId object_id,
          std::unordered_set<ObjectId> new_nodes) mutable {
          if (status != Status::OK) {
            callback(status, nullptr);
            return;
          }
          // If the commit is a no-op, returns early.
          if (parents.size() == 1 &&
              parents.front()->GetRootId() == object_id) {
            FTL_DCHECK(new_nodes.empty());
            callback(Rollback(), std::move(parents.front()));
            return;
          }
          std::unique_ptr<storage::Commit> commit =
              CommitImpl::FromContentAndParents(page_storage_, object_id,
                                                std::move(parents));
          page_storage_->AddCommitFromLocal(
              commit->Clone(), ftl::MakeCopyable([
                this, commit = std::move(commit),
                new_nodes = std::move(new_nodes), callback
              ](Status status) mutable {
                valid_ = false;
                if (status != Status::OK) {
                  callback(status, nullptr);
                  return;
                }
                status = ClearCommittedJournal(std::move(new_nodes));
                if (status != Status::OK) {
                  callback(status, nullptr);
                } else {
                  callback(Status::OK, std::move(commit));
                }
              }));
        }));
  });
}

Status JournalDBImpl::Rollback() {
  if (!valid_) {
    return Status::ILLEGAL_STATE;
  }
  Status s = db_->RemoveJournal(id_);
  if (s == Status::OK) {
    valid_ = false;
  }
  return s;
}

}  // namespace storage
