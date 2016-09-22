// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/fake_network_service/fake_url_loader.h"

#include <utility>

#include "lib/ftl/logging.h"

namespace fake_network_service {

FakeURLLoader::FakeURLLoader(
    mojo::InterfaceRequest<mojo::URLLoader> message_pipe,
    mojo::URLResponsePtr response_to_return,
    mojo::URLRequestPtr* request_received)
    : binding_(this, std::move(message_pipe)),
      response_to_return_(std::move(response_to_return)),
      request_received_(request_received) {
  FTL_DCHECK(response_to_return_);
}

FakeURLLoader::~FakeURLLoader() {}

void FakeURLLoader::Start(mojo::URLRequestPtr request,
                          const StartCallback& callback) {
  FTL_DCHECK(response_to_return_);
  *request_received_ = std::move(request);
  callback.Run(std::move(response_to_return_));
}

void FakeURLLoader::FollowRedirect(const FollowRedirectCallback& callback) {}

void FakeURLLoader::QueryStatus(const QueryStatusCallback& callback) {}

}  // namespace fake_network_service
