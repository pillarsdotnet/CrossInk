#pragma once

#include "ImageToFramebufferDecoder.h"

// Decodes the FIRST frame of a GIF (static or animated) into the framebuffer.
// These e-ink devices are unsuitable for animation, so animated GIFs render as
// a single still (frame 0) rather than looping.
class GifToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "GIF"; }
};
