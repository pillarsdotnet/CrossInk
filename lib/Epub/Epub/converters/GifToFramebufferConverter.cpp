#include "GifToFramebufferConverter.h"

#include <AnimatedGIF.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>

#include <cstdlib>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// The AnimatedGIF object holds its LZW tables, palette, and line buffer inline
// (~20 KB). Heap-allocate on demand so it is only resident while decoding.
constexpr uint32_t GIF_DECODER_APPROX_SIZE = 24U * 1024U;

// Context passed through AnimatedGIF's draw callback via pDraw->pUser.
struct GifContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates

  PixelCache cache;
  bool caching{false};

  // Grayscale lookup table built once from the (frame-constant) palette.
  bool grayPaletteReady{false};
  uint8_t grayPalette[256];
};

// File I/O callbacks mirror the PNG converter: the FsFile* lives in fHandle.
void* gifOpen(const char* filename, int32_t* pSize) {
  auto f = makeUniqueNoThrow<FsFile>();
  if (!f) {
    LOG_ERR("GIF", "OOM: GIF file handle (%u free, %u max alloc)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return nullptr;
  }
  if (!Storage.openFileForRead("GIF", std::string(filename), *f)) {
    return nullptr;
  }
  *pSize = f->size();
  return f.release();  // AnimatedGIF owns this handle until gifClose deletes it.
}

void gifClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t gifRead(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t gifSeek(GIFFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  f->seek(pos);
  return pos;  // AnimatedGIF stores this as the new file position.
}

// Build a 256-entry grayscale LUT from the RGB565 palette. Transparent pixels
// blend into the white page background so ornaments with transparent edges look
// right on paper-white e-ink.
void buildGrayPalette(GifContext* ctx, const GIFDRAW* pDraw) {
  for (int i = 0; i < 256; i++) {
    uint16_t c = pDraw->pPalette[i];
    uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) << 3);
    uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) << 2);
    uint8_t b = static_cast<uint8_t>((c & 0x1F) << 3);
    ctx->grayPalette[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
  }
  if (pDraw->ucHasTransparency) {
    ctx->grayPalette[pDraw->ucTransparent] = 255;  // transparent -> white
  }
  ctx->grayPaletteReady = true;
}

void gifDrawCallback(GIFDRAW* pDraw) {
  GifContext* ctx = reinterpret_cast<GifContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return;

  if (!ctx->grayPaletteReady) {
    buildGrayPalette(ctx, pDraw);
  }

  const int srcY = pDraw->y;
  const int dstY = static_cast<int>(srcY * ctx->scale);

  // Skip if this destination row was already rendered (downscale collision).
  if (dstY == ctx->lastDstY) return;
  ctx->lastDstY = dstY;
  if (dstY >= ctx->dstHeight) return;

  const int outY = ctx->config->y + dstY;
  if (outY >= ctx->screenHeight) return;

  const int srcWidth = ctx->srcWidth;
  const int dstWidth = ctx->dstWidth;
  const int outXBase = ctx->config->x;
  const int screenWidth = ctx->screenWidth;
  const bool useDithering = ctx->config->useDithering;
  const int frameWidth = pDraw->iWidth;
  const uint8_t* indices = pDraw->pPixels;
  bool caching = ctx->caching;

  DirectPixelWriter pw;
  pw.init(*ctx->renderer);
  pw.beginRow(outY);

  DirectCacheWriter cw;
  if (caching) {
    if (!ctx->cache.advanceTo(dstY)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
    }
  }

  // Bresenham-style horizontal scaling (no per-pixel float division).
  int srcX = 0;
  int error = 0;
  for (int dstX = 0; dstX < dstWidth; dstX++) {
    const int outX = outXBase + dstX;
    if (outX >= 0 && outX < screenWidth) {
      const uint8_t gray = (srcX < frameWidth) ? ctx->grayPalette[indices[srcX]] : 255;

      uint8_t ditheredGray;
      if (useDithering) {
        ditheredGray = applyBayerDither4Level(gray, outX, outY);
      } else {
        ditheredGray = gray / 85;
        if (ditheredGray > 3) ditheredGray = 3;
      }
      pw.writePixel(outX, ditheredGray);
      if (caching) cw.writePixel(outX, ditheredGray);
    }

    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }
}

}  // namespace

bool GifToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  if (!MemoryBudget::hasHeapForImageDecoder("GIF", "GIF", GIF_DECODER_APPROX_SIZE)) {
    return false;
  }

  AnimatedGIF* gif = new (std::nothrow) AnimatedGIF();
  if (!gif) {
    LOG_ERR("GIF", "Failed to allocate GIF decoder for dimensions");
    return false;
  }

  gif->begin(GIF_PALETTE_RGB565_LE);
  if (gif->open(imagePath.c_str(), gifOpen, gifClose, gifRead, gifSeek, nullptr) == 0) {
    LOG_ERR("GIF", "Failed to open GIF for dimensions: %d", gif->getLastError());
    delete gif;
    return false;
  }

  out.width = static_cast<int16_t>(gif->getCanvasWidth());
  out.height = static_cast<int16_t>(gif->getCanvasHeight());

  gif->close();
  delete gif;
  return true;
}

bool GifToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("GIF", "Decoding GIF (first frame): %s", imagePath.c_str());

  if (!MemoryBudget::hasHeapForImageDecoder("GIF", "GIF", GIF_DECODER_APPROX_SIZE)) {
    return false;
  }

  AnimatedGIF* gif = new (std::nothrow) AnimatedGIF();
  if (!gif) {
    LOG_ERR("GIF", "Failed to allocate GIF decoder");
    return false;
  }

  gif->begin(GIF_PALETTE_RGB565_LE);

  GifContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  if (gif->open(imagePath.c_str(), gifOpen, gifClose, gifRead, gifSeek, gifDrawCallback) == 0) {
    LOG_ERR("GIF", "Failed to open GIF: %d", gif->getLastError());
    delete gif;
    return false;
  }

  const int w = gif->getCanvasWidth();
  const int h = gif->getCanvasHeight();
  if (!validateImageDimensions(w, h, "GIF")) {
    gif->close();
    delete gif;
    return false;
  }

  ctx.srcWidth = w;
  ctx.srcHeight = h;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale
    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;

  LOG_DBG("GIF", "GIF %dx%d -> %dx%d (scale %.2f)", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth, ctx.dstHeight,
          ctx.scale);

  // Stream the pixel cache one row at a time, exactly like the PNG path.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("GIF", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  // RAW draw type delivers palette indices per line to the callback (we apply
  // the palette + grayscale ourselves). Decode ONLY the first frame.
  gif->setDrawType(GIF_DRAW_RAW);

  const unsigned long decodeStart = millis();
  const int rc = gif->playFrame(false, nullptr, &ctx);
  const unsigned long decodeTime = millis() - decodeStart;

  gif->close();
  delete gif;

  if (rc < 0) {
    LOG_ERR("GIF", "Decode failed (first frame)");
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("GIF", "GIF decoding complete - render time: %lu ms", decodeTime);

  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool GifToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasGifExtension(extension);
}
