// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <optional>
#include <unordered_map>
#include <utility>

#include "impeller/core/formats.h"
#include "impeller/entity/contents/atlas_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/filters/blend_filter_contents.h"
#include "impeller/entity/contents/filters/color_filter_contents.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/entity.h"
#include "impeller/entity/texture_fill.frag.h"
#include "impeller/entity/texture_fill.vert.h"
#include "impeller/geometry/color.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {

AtlasContents::AtlasContents() = default;

AtlasContents::~AtlasContents() = default;

void AtlasContents::SetTexture(std::shared_ptr<Texture> texture) {
  texture_ = std::move(texture);
}

std::shared_ptr<Texture> AtlasContents::GetTexture() const {
  return texture_;
}

void AtlasContents::SetTransforms(std::vector<Matrix> transforms) {
  transforms_ = std::move(transforms);
  bounding_box_cache_.reset();
}

void AtlasContents::SetTextureCoordinates(std::vector<Rect> texture_coords) {
  texture_coords_ = std::move(texture_coords);
  bounding_box_cache_.reset();
}

void AtlasContents::SetColors(std::vector<Color> colors) {
  colors_ = std::move(colors);
}

void AtlasContents::SetAlpha(Scalar alpha) {
  alpha_ = alpha;
}

void AtlasContents::SetBlendMode(BlendMode blend_mode) {
  blend_mode_ = blend_mode;
}

void AtlasContents::SetCullRect(std::optional<Rect> cull_rect) {
  cull_rect_ = cull_rect;
}

struct AtlasBlenderKey {
  Color color;
  Rect rect;
  uint32_t color_key;

  struct Hash {
    std::size_t operator()(const AtlasBlenderKey& key) const {
      return fml::HashCombine(key.color_key, key.rect.GetWidth(),
                              key.rect.GetHeight(), key.rect.GetX(),
                              key.rect.GetY());
    }
  };

  struct Equal {
    bool operator()(const AtlasBlenderKey& lhs,
                    const AtlasBlenderKey& rhs) const {
      return lhs.rect == rhs.rect && lhs.color_key == rhs.color_key;
    }
  };
};

std::shared_ptr<SubAtlasResult> AtlasContents::GenerateSubAtlas() const {
  FML_DCHECK(colors_.size() > 0 && blend_mode_ != BlendMode::kSource &&
             blend_mode_ != BlendMode::kDestination);

  std::unordered_map<AtlasBlenderKey, std::vector<Matrix>,
                     AtlasBlenderKey::Hash, AtlasBlenderKey::Equal>
      sub_atlas = {};

  for (auto i = 0u; i < texture_coords_.size(); i++) {
    AtlasBlenderKey key = {.color = colors_[i],
                           .rect = texture_coords_[i],
                           .color_key = Color::ToIColor(colors_[i])};
    if (sub_atlas.find(key) == sub_atlas.end()) {
      sub_atlas[key] = {transforms_[i]};
    } else {
      sub_atlas[key].push_back(transforms_[i]);
    }
  }

  auto result = std::make_shared<SubAtlasResult>();
  Scalar x_offset = 0.0;
  Scalar y_offset = 0.0;
  Scalar x_extent = 0.0;
  Scalar y_extent = 0.0;

  for (auto it = sub_atlas.begin(); it != sub_atlas.end(); it++) {
    // This size was arbitrarily chosen to keep the textures from getting too
    // wide. We could instead use a more generic rect packer but in the majority
    // of cases the sample rects will be fairly close in size making this a good
    // enough approximation.
    if (x_offset >= 1000) {
      y_offset = y_extent + 1;
      x_offset = 0.0;
    }

    auto key = it->first;
    auto transforms = it->second;

    auto new_rect = Rect::MakeXYWH(x_offset, y_offset, key.rect.GetWidth(),
                                   key.rect.GetHeight());
    auto sub_transform = Matrix::MakeTranslation(Vector2(x_offset, y_offset));

    x_offset += std::ceil(key.rect.GetWidth()) + 1.0;

    result->sub_texture_coords.push_back(key.rect);
    result->sub_colors.push_back(key.color);
    result->sub_transforms.push_back(sub_transform);

    x_extent = std::max(x_extent, x_offset);
    y_extent = std::max(y_extent, std::ceil(y_offset + key.rect.GetHeight()));

    for (auto transform : transforms) {
      result->result_texture_coords.push_back(new_rect);
      result->result_transforms.push_back(transform);
    }
  }
  result->size = ISize(std::ceil(x_extent), std::ceil(y_extent));
  return result;
}

std::optional<Rect> AtlasContents::GetCoverage(const Entity& entity) const {
  if (cull_rect_.has_value()) {
    return cull_rect_.value().TransformBounds(entity.GetTransform());
  }
  return ComputeBoundingBox().TransformBounds(entity.GetTransform());
}

Rect AtlasContents::ComputeBoundingBox() const {
  if (!bounding_box_cache_.has_value()) {
    Rect bounding_box = {};
    for (size_t i = 0; i < texture_coords_.size(); i++) {
      auto matrix = transforms_[i];
      auto sample_rect = texture_coords_[i];
      auto bounds =
          Rect::MakeSize(sample_rect.GetSize()).TransformBounds(matrix);
      bounding_box = bounds.Union(bounding_box);
    }
    bounding_box_cache_ = bounding_box;
  }
  return bounding_box_cache_.value();
}

void AtlasContents::SetSamplerDescriptor(SamplerDescriptor desc) {
  sampler_descriptor_ = std::move(desc);
}

const SamplerDescriptor& AtlasContents::GetSamplerDescriptor() const {
  return sampler_descriptor_;
}

const std::vector<Matrix>& AtlasContents::GetTransforms() const {
  return transforms_;
}

const std::vector<Rect>& AtlasContents::GetTextureCoordinates() const {
  return texture_coords_;
}

const std::vector<Color>& AtlasContents::GetColors() const {
  return colors_;
}

bool AtlasContents::Render(const ContentContext& renderer,
                           const Entity& entity,
                           RenderPass& pass) const {
  if (texture_ == nullptr || blend_mode_ == BlendMode::kClear ||
      alpha_ <= 0.0) {
    return true;
  }

  // Ensure that we use the actual computed bounds and not a cull-rect
  // approximation of them.
  auto coverage = ComputeBoundingBox();

  if (blend_mode_ == BlendMode::kSource || colors_.size() == 0) {
    auto child_contents = AtlasTextureContents(*this);
    child_contents.SetAlpha(alpha_);
    child_contents.SetCoverage(coverage);
    return child_contents.Render(renderer, entity, pass);
  }
  if (blend_mode_ == BlendMode::kDestination) {
    auto child_contents = AtlasColorContents(*this);
    child_contents.SetAlpha(alpha_);
    child_contents.SetCoverage(coverage);
    return child_contents.Render(renderer, entity, pass);
  }

  constexpr size_t indices[6] = {0, 1, 2, 1, 2, 3};

  if (blend_mode_ <= BlendMode::kModulate) {
    // Simple Porter-Duff blends can be accomplished without a subpass.
    using VS = PorterDuffBlendPipeline::VertexShader;
    using FS = PorterDuffBlendPipeline::FragmentShader;

    VertexBufferBuilder<VS::PerVertexData> vtx_builder;
    vtx_builder.Reserve(texture_coords_.size() * 6);
    const auto texture_size = texture_->GetSize();
    auto& host_buffer = renderer.GetTransientsBuffer();

    for (size_t i = 0; i < texture_coords_.size(); i++) {
      auto sample_rect = texture_coords_[i];
      auto matrix = transforms_[i];
      auto points = sample_rect.GetPoints();
      auto transformed_points =
          Rect::MakeSize(sample_rect.GetSize()).GetTransformedPoints(matrix);
      auto color = colors_[i].Premultiply();
      for (size_t j = 0; j < 6; j++) {
        VS::PerVertexData data;
        data.vertices = transformed_points[indices[j]];
        data.texture_coords = points[indices[j]] / texture_size;
        data.color = color;
        vtx_builder.AppendVertex(data);
      }
    }

#ifdef IMPELLER_DEBUG
    pass.SetCommandLabel(
        SPrintF("DrawAtlas Blend (%s)", BlendModeToString(blend_mode_)));
#endif  // IMPELLER_DEBUG
    pass.SetVertexBuffer(vtx_builder.CreateVertexBuffer(host_buffer));
    pass.SetStencilReference(entity.GetClipDepth());
    pass.SetPipeline(
        renderer.GetPorterDuffBlendPipeline(OptionsFromPass(pass)));

    FS::FragInfo frag_info;
    VS::FrameInfo frame_info;

    auto dst_sampler_descriptor = sampler_descriptor_;
    if (renderer.GetDeviceCapabilities().SupportsDecalSamplerAddressMode()) {
      dst_sampler_descriptor.width_address_mode = SamplerAddressMode::kDecal;
      dst_sampler_descriptor.height_address_mode = SamplerAddressMode::kDecal;
    }
    auto dst_sampler = renderer.GetContext()->GetSamplerLibrary()->GetSampler(
        dst_sampler_descriptor);
    FS::BindTextureSamplerDst(pass, texture_, dst_sampler);
    frame_info.texture_sampler_y_coord_scale = texture_->GetYCoordScale();

    frag_info.output_alpha = alpha_;
    frag_info.input_alpha = 1.0;

    auto inverted_blend_mode =
        InvertPorterDuffBlend(blend_mode_).value_or(BlendMode::kSource);
    auto blend_coefficients =
        kPorterDuffCoefficients[static_cast<int>(inverted_blend_mode)];
    frag_info.src_coeff = blend_coefficients[0];
    frag_info.src_coeff_dst_alpha = blend_coefficients[1];
    frag_info.dst_coeff = blend_coefficients[2];
    frag_info.dst_coeff_src_alpha = blend_coefficients[3];
    frag_info.dst_coeff_src_color = blend_coefficients[4];

    FS::BindFragInfo(pass, host_buffer.EmplaceUniform(frag_info));

    frame_info.mvp = pass.GetOrthographicTransform() * entity.GetTransform();

    auto uniform_view = host_buffer.EmplaceUniform(frame_info);
    VS::BindFrameInfo(pass, uniform_view);

    return pass.Draw().ok();
  }

  // Advanced blends.

  auto sub_atlas = GenerateSubAtlas();
  auto sub_coverage = Rect::MakeSize(sub_atlas->size);

  auto src_contents = std::make_shared<AtlasTextureContents>(*this);
  src_contents->SetSubAtlas(sub_atlas);
  src_contents->SetCoverage(sub_coverage);

  auto dst_contents = std::make_shared<AtlasColorContents>(*this);
  dst_contents->SetSubAtlas(sub_atlas);
  dst_contents->SetCoverage(sub_coverage);

  Entity untransformed_entity;
  auto contents = ColorFilterContents::MakeBlend(
      blend_mode_,
      {FilterInput::Make(dst_contents), FilterInput::Make(src_contents)});
  auto snapshot =
      contents->RenderToSnapshot(renderer,              // renderer
                                 untransformed_entity,  // entity
                                 std::nullopt,          // coverage_limit
                                 std::nullopt,          // sampler_descriptor
                                 true,                  // msaa_enabled
                                 "AtlasContents Snapshot");  // label
  if (!snapshot.has_value()) {
    return false;
  }

  auto child_contents = AtlasTextureContents(*this);
  child_contents.SetAlpha(alpha_);
  child_contents.SetCoverage(coverage);
  child_contents.SetTexture(snapshot.value().texture);
  child_contents.SetUseDestination(true);
  child_contents.SetSubAtlas(sub_atlas);
  return child_contents.Render(renderer, entity, pass);
}

// AtlasTextureContents
// ---------------------------------------------------------

AtlasTextureContents::AtlasTextureContents(const AtlasContents& parent)
    : parent_(parent) {}

AtlasTextureContents::~AtlasTextureContents() {}

std::optional<Rect> AtlasTextureContents::GetCoverage(
    const Entity& entity) const {
  return coverage_.TransformBounds(entity.GetTransform());
}

void AtlasTextureContents::SetAlpha(Scalar alpha) {
  alpha_ = alpha;
}

void AtlasTextureContents::SetCoverage(Rect coverage) {
  coverage_ = coverage;
}

void AtlasTextureContents::SetUseDestination(bool value) {
  use_destination_ = value;
}

void AtlasTextureContents::SetSubAtlas(
    const std::shared_ptr<SubAtlasResult>& subatlas) {
  subatlas_ = subatlas;
}

void AtlasTextureContents::SetTexture(std::shared_ptr<Texture> texture) {
  texture_ = std::move(texture);
}

bool AtlasTextureContents::Render(const ContentContext& renderer,
                                  const Entity& entity,
                                  RenderPass& pass) const {
  using VS = TextureFillVertexShader;
  using FS = TextureFillFragmentShader;

  auto texture = texture_ ? texture_ : parent_.GetTexture();
  if (texture == nullptr) {
    return true;
  }

  std::vector<Rect> texture_coords;
  std::vector<Matrix> transforms;
  if (subatlas_) {
    texture_coords = use_destination_ ? subatlas_->result_texture_coords
                                      : subatlas_->sub_texture_coords;
    transforms = use_destination_ ? subatlas_->result_transforms
                                  : subatlas_->sub_transforms;
  } else {
    texture_coords = parent_.GetTextureCoordinates();
    transforms = parent_.GetTransforms();
  }

  const Size texture_size(texture->GetSize());
  VertexBufferBuilder<VS::PerVertexData> vertex_builder;
  vertex_builder.Reserve(texture_coords.size() * 6);
  constexpr size_t indices[6] = {0, 1, 2, 1, 2, 3};
  for (size_t i = 0; i < texture_coords.size(); i++) {
    auto sample_rect = texture_coords[i];
    auto matrix = transforms[i];
    auto points = sample_rect.GetPoints();
    auto transformed_points =
        Rect::MakeSize(sample_rect.GetSize()).GetTransformedPoints(matrix);

    for (size_t j = 0; j < 6; j++) {
      VS::PerVertexData data;
      data.position = transformed_points[indices[j]];
      data.texture_coords = points[indices[j]] / texture_size;
      vertex_builder.AppendVertex(data);
    }
  }

  if (!vertex_builder.HasVertices()) {
    return true;
  }

  pass.SetCommandLabel("AtlasTexture");

  auto& host_buffer = renderer.GetTransientsBuffer();

  VS::FrameInfo frame_info;
  frame_info.mvp = pass.GetOrthographicTransform() * entity.GetTransform();
  frame_info.texture_sampler_y_coord_scale = texture->GetYCoordScale();
  frame_info.alpha = alpha_;

  auto options = OptionsFromPassAndEntity(pass, entity);
  pass.SetPipeline(renderer.GetTexturePipeline(options));
  pass.SetStencilReference(entity.GetClipDepth());
  pass.SetVertexBuffer(vertex_builder.CreateVertexBuffer(host_buffer));
  VS::BindFrameInfo(pass, host_buffer.EmplaceUniform(frame_info));
  FS::BindTextureSampler(pass, texture,
                         renderer.GetContext()->GetSamplerLibrary()->GetSampler(
                             parent_.GetSamplerDescriptor()));
  return pass.Draw().ok();
}

// AtlasColorContents
// ---------------------------------------------------------

AtlasColorContents::AtlasColorContents(const AtlasContents& parent)
    : parent_(parent) {}

AtlasColorContents::~AtlasColorContents() {}

std::optional<Rect> AtlasColorContents::GetCoverage(
    const Entity& entity) const {
  return coverage_.TransformBounds(entity.GetTransform());
}

void AtlasColorContents::SetAlpha(Scalar alpha) {
  alpha_ = alpha;
}

void AtlasColorContents::SetCoverage(Rect coverage) {
  coverage_ = coverage;
}

void AtlasColorContents::SetSubAtlas(
    const std::shared_ptr<SubAtlasResult>& subatlas) {
  subatlas_ = subatlas;
}

bool AtlasColorContents::Render(const ContentContext& renderer,
                                const Entity& entity,
                                RenderPass& pass) const {
  using VS = GeometryColorPipeline::VertexShader;
  using FS = GeometryColorPipeline::FragmentShader;

  std::vector<Rect> texture_coords;
  std::vector<Matrix> transforms;
  std::vector<Color> colors;
  if (subatlas_) {
    texture_coords = subatlas_->sub_texture_coords;
    colors = subatlas_->sub_colors;
    transforms = subatlas_->sub_transforms;
  } else {
    texture_coords = parent_.GetTextureCoordinates();
    transforms = parent_.GetTransforms();
    colors = parent_.GetColors();
  }

  VertexBufferBuilder<VS::PerVertexData> vertex_builder;
  vertex_builder.Reserve(texture_coords.size() * 6);
  constexpr size_t indices[6] = {0, 1, 2, 1, 2, 3};
  for (size_t i = 0; i < texture_coords.size(); i++) {
    auto sample_rect = texture_coords[i];
    auto matrix = transforms[i];
    auto transformed_points =
        Rect::MakeSize(sample_rect.GetSize()).GetTransformedPoints(matrix);

    for (size_t j = 0; j < 6; j++) {
      VS::PerVertexData data;
      data.position = transformed_points[indices[j]];
      data.color = colors[i].Premultiply();
      vertex_builder.AppendVertex(data);
    }
  }

  if (!vertex_builder.HasVertices()) {
    return true;
  }

  pass.SetCommandLabel("AtlasColors");

  auto& host_buffer = renderer.GetTransientsBuffer();

  VS::FrameInfo frame_info;
  frame_info.mvp = pass.GetOrthographicTransform() * entity.GetTransform();

  FS::FragInfo frag_info;
  frag_info.alpha = alpha_;

  auto opts = OptionsFromPassAndEntity(pass, entity);
  opts.blend_mode = BlendMode::kSourceOver;
  pass.SetPipeline(renderer.GetGeometryColorPipeline(opts));
  pass.SetStencilReference(entity.GetClipDepth());
  pass.SetVertexBuffer(vertex_builder.CreateVertexBuffer(host_buffer));
  VS::BindFrameInfo(pass, host_buffer.EmplaceUniform(frame_info));
  FS::BindFragInfo(pass, host_buffer.EmplaceUniform(frag_info));
  return pass.Draw().ok();
}

}  // namespace impeller
