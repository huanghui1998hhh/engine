// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/compute_pass.h"
#include <memory>

#include "fml/logging.h"
#include "impeller/base/validation.h"

namespace impeller {

ComputePass::ComputePass(std::weak_ptr<const Context> context)
    : context_(std::move(context)) {}

ComputePass::~ComputePass() = default;

void ComputePass::SetLabel(const std::string& label) {
  if (label.empty()) {
    return;
  }
  OnSetLabel(label);
}

void ComputePass::SetGridSize(const ISize& size) {
  grid_size_ = size;
}

void ComputePass::SetThreadGroupSize(const ISize& size) {
  thread_group_size_ = size;
}

bool ComputePass::AddCommand(ComputeCommand command) {
  if (!command) {
    VALIDATION_LOG
        << "Attempted to add an invalid command to the compute pass.";
    return false;
  }

  commands_.emplace_back(std::move(command));
  return true;
}

bool ComputePass::EncodeCommands() const {
  if (grid_size_.IsEmpty() || thread_group_size_.IsEmpty()) {
    FML_DLOG(WARNING) << "Attempted to encode a compute pass with an empty "
                         "grid or thread group size.";
    return false;
  }
  auto context = context_.lock();
  // The context could have been collected in the meantime.
  if (!context) {
    return false;
  }
  return OnEncodeCommands(*context, grid_size_, thread_group_size_);
}

}  // namespace impeller
