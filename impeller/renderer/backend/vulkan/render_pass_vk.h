// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_RENDER_PASS_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_RENDER_PASS_VK_H_

#include "flutter/fml/macros.h"
#include "impeller/renderer/backend/vulkan/binding_helpers_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/pass_bindings_cache.h"
#include "impeller/renderer/backend/vulkan/shared_object_vk.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

class CommandBufferVK;

class RenderPassVK final : public RenderPass {
 public:
  // |RenderPass|
  ~RenderPassVK() override;

 private:
  friend class CommandBufferVK;

  std::weak_ptr<CommandBufferVK> command_buffer_;
  std::string debug_label_;
  bool is_valid_ = false;

  mutable std::array<vk::DescriptorImageInfo, kMaxBindings> image_workspace_;
  mutable std::array<vk::DescriptorBufferInfo, kMaxBindings> buffer_workspace_;
  mutable std::array<vk::WriteDescriptorSet, kMaxBindings + kMaxBindings>
      write_workspace_;

  mutable PassBindingsCache pass_bindings_cache_;

  RenderPassVK(const std::shared_ptr<const Context>& context,
               const RenderTarget& target,
               std::weak_ptr<CommandBufferVK> command_buffer);

  // |RenderPass|
  bool IsValid() const override;

  // |RenderPass|
  void OnSetLabel(std::string label) override;

  // |RenderPass|
  bool OnEncodeCommands(const Context& context) const override;

  SharedHandleVK<vk::RenderPass> CreateVKRenderPass(
      const ContextVK& context,
      const std::shared_ptr<CommandBufferVK>& command_buffer,
      bool has_subpass_dependency) const;

  SharedHandleVK<vk::Framebuffer> CreateVKFramebuffer(
      const ContextVK& context,
      const vk::RenderPass& pass) const;

  RenderPassVK(const RenderPassVK&) = delete;

  RenderPassVK& operator=(const RenderPassVK&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_RENDER_PASS_VK_H_
