// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/metal/render_pass_mtl.h"

#include "flutter/fml/closure.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/trace_event.h"
#include "impeller/base/backend_cast.h"
#include "impeller/core/formats.h"
#include "impeller/core/host_buffer.h"
#include "impeller/core/shader_types.h"
#include "impeller/renderer/backend/metal/context_mtl.h"
#include "impeller/renderer/backend/metal/device_buffer_mtl.h"
#include "impeller/renderer/backend/metal/formats_mtl.h"
#include "impeller/renderer/backend/metal/pipeline_mtl.h"
#include "impeller/renderer/backend/metal/sampler_mtl.h"
#include "impeller/renderer/backend/metal/texture_mtl.h"
#include "impeller/renderer/command.h"
#include "impeller/renderer/vertex_descriptor.h"

namespace impeller {

static bool ConfigureResolveTextureAttachment(
    const Attachment& desc,
    MTLRenderPassAttachmentDescriptor* attachment) {
  bool needs_resolve =
      desc.store_action == StoreAction::kMultisampleResolve ||
      desc.store_action == StoreAction::kStoreAndMultisampleResolve;

  if (needs_resolve && !desc.resolve_texture) {
    VALIDATION_LOG << "Resolve store action specified on attachment but no "
                      "resolve texture was specified.";
    return false;
  }

  if (desc.resolve_texture && !needs_resolve) {
    VALIDATION_LOG << "A resolve texture was specified even though the store "
                      "action doesn't require it.";
    return false;
  }

  if (!desc.resolve_texture) {
    return true;
  }

  attachment.resolveTexture =
      TextureMTL::Cast(*desc.resolve_texture).GetMTLTexture();

  return true;
}

static bool ConfigureAttachment(const Attachment& desc,
                                MTLRenderPassAttachmentDescriptor* attachment) {
  if (!desc.texture) {
    return false;
  }

  attachment.texture = TextureMTL::Cast(*desc.texture).GetMTLTexture();
  attachment.loadAction = ToMTLLoadAction(desc.load_action);
  attachment.storeAction = ToMTLStoreAction(desc.store_action);

  if (!ConfigureResolveTextureAttachment(desc, attachment)) {
    return false;
  }

  return true;
}

static bool ConfigureColorAttachment(
    const ColorAttachment& desc,
    MTLRenderPassColorAttachmentDescriptor* attachment) {
  if (!ConfigureAttachment(desc, attachment)) {
    return false;
  }
  attachment.clearColor = ToMTLClearColor(desc.clear_color);
  return true;
}

static bool ConfigureDepthAttachment(
    const DepthAttachment& desc,
    MTLRenderPassDepthAttachmentDescriptor* attachment) {
  if (!ConfigureAttachment(desc, attachment)) {
    return false;
  }
  attachment.clearDepth = desc.clear_depth;
  return true;
}

static bool ConfigureStencilAttachment(
    const StencilAttachment& desc,
    MTLRenderPassStencilAttachmentDescriptor* attachment) {
  if (!ConfigureAttachment(desc, attachment)) {
    return false;
  }
  attachment.clearStencil = desc.clear_stencil;
  return true;
}

// TODO(csg): Move this to formats_mtl.h
static MTLRenderPassDescriptor* ToMTLRenderPassDescriptor(
    const RenderTarget& desc) {
  auto result = [MTLRenderPassDescriptor renderPassDescriptor];

  const auto& colors = desc.GetColorAttachments();

  for (const auto& color : colors) {
    if (!ConfigureColorAttachment(color.second,
                                  result.colorAttachments[color.first])) {
      VALIDATION_LOG << "Could not configure color attachment at index "
                     << color.first;
      return nil;
    }
  }

  const auto& depth = desc.GetDepthAttachment();

  if (depth.has_value() &&
      !ConfigureDepthAttachment(depth.value(), result.depthAttachment)) {
    VALIDATION_LOG << "Could not configure depth attachment.";
    return nil;
  }

  const auto& stencil = desc.GetStencilAttachment();

  if (stencil.has_value() &&
      !ConfigureStencilAttachment(stencil.value(), result.stencilAttachment)) {
    VALIDATION_LOG << "Could not configure stencil attachment.";
    return nil;
  }

  return result;
}

RenderPassMTL::RenderPassMTL(std::weak_ptr<const Context> context,
                             const RenderTarget& target,
                             id<MTLCommandBuffer> buffer)
    : RenderPass(std::move(context), target),
      buffer_(buffer),
      desc_(ToMTLRenderPassDescriptor(GetRenderTarget())) {
  if (!buffer_ || !desc_ || !render_target_.IsValid()) {
    return;
  }
  is_valid_ = true;
}

RenderPassMTL::~RenderPassMTL() = default;

bool RenderPassMTL::IsValid() const {
  return is_valid_;
}

void RenderPassMTL::OnSetLabel(std::string label) {
  if (label.empty()) {
    return;
  }
  label_ = std::move(label);
}

bool RenderPassMTL::OnEncodeCommands(const Context& context) const {
  TRACE_EVENT0("impeller", "RenderPassMTL::EncodeCommands");
  if (!IsValid()) {
    return false;
  }
  auto render_command_encoder =
      [buffer_ renderCommandEncoderWithDescriptor:desc_];

  if (!render_command_encoder) {
    return false;
  }

  if (!label_.empty()) {
    [render_command_encoder setLabel:@(label_.c_str())];
  }

  // Success or failure, the pass must end. The buffer can only process one pass
  // at a time.
  fml::ScopedCleanupClosure auto_end(
      [render_command_encoder]() { [render_command_encoder endEncoding]; });

  return EncodeCommands(context.GetResourceAllocator(), render_command_encoder);
}

//-----------------------------------------------------------------------------
/// @brief      Ensures that bindings on the pass are not redundantly set or
///             updated. Avoids making the driver do additional checks and makes
///             the frame insights during profiling and instrumentation not
///             complain about the same.
///
///             There should be no change to rendering if this caching was
///             absent.
///
struct PassBindingsCache {
  explicit PassBindingsCache(id<MTLRenderCommandEncoder> encoder)
      : encoder_(encoder) {}

  PassBindingsCache(const PassBindingsCache&) = delete;

  PassBindingsCache(PassBindingsCache&&) = delete;

  void SetRenderPipelineState(id<MTLRenderPipelineState> pipeline) {
    if (pipeline == pipeline_) {
      return;
    }
    pipeline_ = pipeline;
    [encoder_ setRenderPipelineState:pipeline_];
  }

  void SetDepthStencilState(id<MTLDepthStencilState> depth_stencil) {
    if (depth_stencil_ == depth_stencil) {
      return;
    }
    depth_stencil_ = depth_stencil;
    [encoder_ setDepthStencilState:depth_stencil_];
  }

  bool SetBuffer(ShaderStage stage,
                 uint64_t index,
                 uint64_t offset,
                 id<MTLBuffer> buffer) {
    auto& buffers_map = buffers_[stage];
    auto found = buffers_map.find(index);
    if (found != buffers_map.end() && found->second.buffer == buffer) {
      // The right buffer is bound. Check if its offset needs to be updated.
      if (found->second.offset == offset) {
        // Buffer and its offset is identical. Nothing to do.
        return true;
      }

      // Only the offset needs to be updated.
      found->second.offset = offset;

      switch (stage) {
        case ShaderStage::kVertex:
          [encoder_ setVertexBufferOffset:offset atIndex:index];
          return true;
        case ShaderStage::kFragment:
          [encoder_ setFragmentBufferOffset:offset atIndex:index];
          return true;
        default:
          VALIDATION_LOG << "Cannot update buffer offset of an unknown stage.";
          return false;
      }
      return true;
    }
    buffers_map[index] = {buffer, static_cast<size_t>(offset)};
    switch (stage) {
      case ShaderStage::kVertex:
        [encoder_ setVertexBuffer:buffer offset:offset atIndex:index];
        return true;
      case ShaderStage::kFragment:
        [encoder_ setFragmentBuffer:buffer offset:offset atIndex:index];
        return true;
      default:
        VALIDATION_LOG << "Cannot bind buffer to unknown shader stage.";
        return false;
    }
    return false;
  }

  bool SetTexture(ShaderStage stage, uint64_t index, id<MTLTexture> texture) {
    auto& texture_map = textures_[stage];
    auto found = texture_map.find(index);
    if (found != texture_map.end() && found->second == texture) {
      // Already bound.
      return true;
    }
    texture_map[index] = texture;
    switch (stage) {
      case ShaderStage::kVertex:
        [encoder_ setVertexTexture:texture atIndex:index];
        return true;
      case ShaderStage::kFragment:
        [encoder_ setFragmentTexture:texture atIndex:index];
        return true;
      default:
        VALIDATION_LOG << "Cannot bind buffer to unknown shader stage.";
        return false;
    }
    return false;
  }

  bool SetSampler(ShaderStage stage,
                  uint64_t index,
                  id<MTLSamplerState> sampler) {
    auto& sampler_map = samplers_[stage];
    auto found = sampler_map.find(index);
    if (found != sampler_map.end() && found->second == sampler) {
      // Already bound.
      return true;
    }
    sampler_map[index] = sampler;
    switch (stage) {
      case ShaderStage::kVertex:
        [encoder_ setVertexSamplerState:sampler atIndex:index];
        return true;
      case ShaderStage::kFragment:
        [encoder_ setFragmentSamplerState:sampler atIndex:index];
        return true;
      default:
        VALIDATION_LOG << "Cannot bind buffer to unknown shader stage.";
        return false;
    }
    return false;
  }

  void SetViewport(const Viewport& viewport) {
    if (viewport_.has_value() && viewport_.value() == viewport) {
      return;
    }
    [encoder_ setViewport:MTLViewport{
                              .originX = viewport.rect.GetX(),
                              .originY = viewport.rect.GetY(),
                              .width = viewport.rect.GetWidth(),
                              .height = viewport.rect.GetHeight(),
                              .znear = viewport.depth_range.z_near,
                              .zfar = viewport.depth_range.z_far,
                          }];
    viewport_ = viewport;
  }

  void SetScissor(const IRect& scissor) {
    if (scissor_.has_value() && scissor_.value() == scissor) {
      return;
    }
    [encoder_
        setScissorRect:MTLScissorRect{
                           .x = static_cast<NSUInteger>(scissor.GetX()),
                           .y = static_cast<NSUInteger>(scissor.GetY()),
                           .width = static_cast<NSUInteger>(scissor.GetWidth()),
                           .height =
                               static_cast<NSUInteger>(scissor.GetHeight()),
                       }];
    scissor_ = scissor;
  }

 private:
  struct BufferOffsetPair {
    id<MTLBuffer> buffer = nullptr;
    size_t offset = 0u;
  };
  using BufferMap = std::map<uint64_t, BufferOffsetPair>;
  using TextureMap = std::map<uint64_t, id<MTLTexture>>;
  using SamplerMap = std::map<uint64_t, id<MTLSamplerState>>;

  const id<MTLRenderCommandEncoder> encoder_;
  id<MTLRenderPipelineState> pipeline_ = nullptr;
  id<MTLDepthStencilState> depth_stencil_ = nullptr;
  std::map<ShaderStage, BufferMap> buffers_;
  std::map<ShaderStage, TextureMap> textures_;
  std::map<ShaderStage, SamplerMap> samplers_;
  std::optional<Viewport> viewport_;
  std::optional<IRect> scissor_;
};

static bool Bind(PassBindingsCache& pass,
                 Allocator& allocator,
                 ShaderStage stage,
                 size_t bind_index,
                 const BufferView& view) {
  if (!view.buffer) {
    return false;
  }

  auto device_buffer = view.buffer;
  if (!device_buffer) {
    return false;
  }

  auto buffer = DeviceBufferMTL::Cast(*device_buffer).GetMTLBuffer();
  // The Metal call is a void return and we don't want to make it on nil.
  if (!buffer) {
    return false;
  }

  return pass.SetBuffer(stage, bind_index, view.range.offset, buffer);
}

static bool Bind(PassBindingsCache& pass,
                 ShaderStage stage,
                 size_t bind_index,
                 const Sampler& sampler,
                 const Texture& texture) {
  if (!sampler.IsValid() || !texture.IsValid()) {
    return false;
  }

  if (texture.NeedsMipmapGeneration()) {
    // TODO(127697): generate mips when the GPU is available on iOS.
#if !FML_OS_IOS
    VALIDATION_LOG
        << "Texture at binding index " << bind_index
        << " has a mip count > 1, but the mipmap has not been generated.";
    return false;
#endif  // !FML_OS_IOS
  }

  return pass.SetTexture(stage, bind_index,
                         TextureMTL::Cast(texture).GetMTLTexture()) &&
         pass.SetSampler(stage, bind_index,
                         SamplerMTL::Cast(sampler).GetMTLSamplerState());
}

bool RenderPassMTL::EncodeCommands(const std::shared_ptr<Allocator>& allocator,
                                   id<MTLRenderCommandEncoder> encoder) const {
  PassBindingsCache pass_bindings(encoder);
  auto bind_stage_resources = [&allocator, &pass_bindings](
                                  const Bindings& bindings,
                                  ShaderStage stage) -> bool {
    for (const BufferAndUniformSlot& buffer : bindings.buffers) {
      if (!Bind(pass_bindings, *allocator, stage, buffer.slot.ext_res_0,
                buffer.view.resource)) {
        return false;
      }
    }
    for (const TextureAndSampler& data : bindings.sampled_images) {
      if (!Bind(pass_bindings, stage, data.slot.texture_index, *data.sampler,
                *data.texture.resource)) {
        return false;
      }
    }
    return true;
  };

  const auto target_sample_count = render_target_.GetSampleCount();

  fml::closure pop_debug_marker = [encoder]() { [encoder popDebugGroup]; };
  for (const auto& command : commands_) {
#ifdef IMPELLER_DEBUG
    fml::ScopedCleanupClosure auto_pop_debug_marker(pop_debug_marker);
    if (!command.label.empty()) {
      [encoder pushDebugGroup:@(command.label.c_str())];
    } else {
      auto_pop_debug_marker.Release();
    }
#endif  // IMPELLER_DEBUG

    const auto& pipeline_desc = command.pipeline->GetDescriptor();
    if (target_sample_count != pipeline_desc.GetSampleCount()) {
      VALIDATION_LOG << "Pipeline for command and the render target disagree "
                        "on sample counts (target was "
                     << static_cast<uint64_t>(target_sample_count)
                     << " but pipeline wanted "
                     << static_cast<uint64_t>(pipeline_desc.GetSampleCount())
                     << ").";
      return false;
    }

    pass_bindings.SetRenderPipelineState(
        PipelineMTL::Cast(*command.pipeline).GetMTLRenderPipelineState());
    pass_bindings.SetDepthStencilState(
        PipelineMTL::Cast(*command.pipeline).GetMTLDepthStencilState());
    pass_bindings.SetViewport(command.viewport.value_or<Viewport>(
        {.rect = Rect::MakeSize(GetRenderTargetSize())}));
    pass_bindings.SetScissor(
        command.scissor.value_or(IRect::MakeSize(GetRenderTargetSize())));

    [encoder setFrontFacingWinding:pipeline_desc.GetWindingOrder() ==
                                           WindingOrder::kClockwise
                                       ? MTLWindingClockwise
                                       : MTLWindingCounterClockwise];
    [encoder setCullMode:ToMTLCullMode(pipeline_desc.GetCullMode())];
    [encoder setTriangleFillMode:ToMTLTriangleFillMode(
                                     pipeline_desc.GetPolygonMode())];
    [encoder setStencilReferenceValue:command.stencil_reference];

    if (!Bind(pass_bindings, *allocator, ShaderStage::kVertex,
              VertexDescriptor::kReservedVertexBufferIndex,
              command.vertex_buffer.vertex_buffer)) {
      return false;
    }

    if (!bind_stage_resources(command.vertex_bindings, ShaderStage::kVertex)) {
      return false;
    }
    if (!bind_stage_resources(command.fragment_bindings,
                              ShaderStage::kFragment)) {
      return false;
    }

    const PrimitiveType primitive_type = pipeline_desc.GetPrimitiveType();
    if (command.vertex_buffer.index_type == IndexType::kNone) {
      if (command.instance_count != 1u) {
#if TARGET_OS_SIMULATOR
        VALIDATION_LOG << "iOS Simulator does not support instanced rendering.";
        return false;
#else   // TARGET_OS_SIMULATOR
        [encoder drawPrimitives:ToMTLPrimitiveType(primitive_type)
                    vertexStart:command.base_vertex
                    vertexCount:command.vertex_buffer.vertex_count
                  instanceCount:command.instance_count
                   baseInstance:0u];
#endif  // TARGET_OS_SIMULATOR
      } else {
        [encoder drawPrimitives:ToMTLPrimitiveType(primitive_type)
                    vertexStart:command.base_vertex
                    vertexCount:command.vertex_buffer.vertex_count];
      }
      continue;
    }

    if (command.vertex_buffer.index_type == IndexType::kUnknown) {
      return false;
    }
    auto index_buffer = command.vertex_buffer.index_buffer.buffer;
    if (!index_buffer) {
      return false;
    }
    auto mtl_index_buffer = DeviceBufferMTL::Cast(*index_buffer).GetMTLBuffer();
    if (!mtl_index_buffer) {
      return false;
    }

    FML_DCHECK(
        command.vertex_buffer.vertex_count *
            (command.vertex_buffer.index_type == IndexType::k16bit ? 2 : 4) ==
        command.vertex_buffer.index_buffer.range.length);

    if (command.instance_count != 1u) {
#if TARGET_OS_SIMULATOR
      VALIDATION_LOG << "iOS Simulator does not support instanced rendering.";
      return false;
#else   // TARGET_OS_SIMULATOR
      [encoder
          drawIndexedPrimitives:ToMTLPrimitiveType(primitive_type)
                     indexCount:command.vertex_buffer.vertex_count
                      indexType:ToMTLIndexType(command.vertex_buffer.index_type)
                    indexBuffer:mtl_index_buffer
              indexBufferOffset:command.vertex_buffer.index_buffer.range.offset
                  instanceCount:command.instance_count
                     baseVertex:command.base_vertex
                   baseInstance:0u];
#endif  // TARGET_OS_SIMULATOR
    } else {
      [encoder
          drawIndexedPrimitives:ToMTLPrimitiveType(primitive_type)
                     indexCount:command.vertex_buffer.vertex_count
                      indexType:ToMTLIndexType(command.vertex_buffer.index_type)
                    indexBuffer:mtl_index_buffer
              indexBufferOffset:command.vertex_buffer.index_buffer.range
                                    .offset];
    }
  }
  return true;
}

}  // namespace impeller
