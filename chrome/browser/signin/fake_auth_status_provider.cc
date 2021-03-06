// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/signin/fake_auth_status_provider.h"

FakeAuthStatusProvider::FakeAuthStatusProvider(SigninGlobalError* error)
  : global_error_(error),
    auth_error_(GoogleServiceAuthError::AuthErrorNone()) {
  global_error_->AddProvider(this);
}

FakeAuthStatusProvider::~FakeAuthStatusProvider() {
  global_error_->RemoveProvider(this);
}

std::string FakeAuthStatusProvider::GetAccountId() const {
  return account_id_;
}

GoogleServiceAuthError FakeAuthStatusProvider::GetAuthStatus() const {
  return auth_error_;
}

void FakeAuthStatusProvider::SetAuthError(const std::string& account_id,
                                          const GoogleServiceAuthError& error) {
  account_id_ = account_id;
  auth_error_ = error;
  global_error_->AuthStatusChanged();
}
