#include "ScalableBuiltins.h"
#if CROSSINK_SCALABLE_FONTS
#include <GfxRenderer.h>
#include <Logging.h>

#include <optional>

#include "../../src/ReaderFontSizeStep.h"
#include "../../src/fontIds.h"
#include "HalScalableFont.h"
#include "ScalableAssets.generated.h"
namespace {
// Single built-in family (LEXENDDECA == 0) after the Bitter removal.
std::optional<HalScalableFont> faces[4];
bool ready = false;
const uint8_t* const assets[] = {lexenddeca_regularOutline, lexenddeca_boldOutline, lexenddeca_italicOutline,
                                 lexenddeca_bolditalicOutline};
constexpr size_t lengths[] = {sizeof(lexenddeca_regularOutline), sizeof(lexenddeca_boldOutline),
                              sizeof(lexenddeca_italicOutline), sizeof(lexenddeca_bolditalicOutline)};
constexpr int ids[4] = {LEXENDDECA_10_FONT_ID, LEXENDDECA_12_FONT_ID, LEXENDDECA_14_FONT_ID, LEXENDDECA_16_FONT_ID};
}  // namespace
int scalableBuiltinFontId(int id) {
  for (unsigned s = 0; s < 4; ++s)
    if (ids[s] == id && !ready) return UI_12_FONT_ID;
  const int result = int(uint32_t(id) ^ OutlineFingerprint ^ 0x53544600u ^ HalScalableFont::renderingRevision());
  return result ? result : 1;
}
int scalableBuiltinReaderFontId(unsigned family, unsigned points) {
  if (family != 0 || !ready) return UI_12_FONT_ID;
  // Preserve existing cache IDs for the original four sizes.
  for (unsigned s = 0; s < 4; ++s)
    if (points == 10 + s * 2) return scalableBuiltinFontId(ids[s]);
  return scalableBuiltinFontId(0x54540000 | (family << 8) | points);
}
void ensureScalableBuiltinFamily(GfxRenderer& renderer, unsigned family) {
  ScalableFontAccess access;
  if (family != 0 || ready) return;
  bool ok = true;
  for (unsigned style = 0; style < 4; ++style) {
    faces[style].emplace();
    if (!faces[style]->openMemory(assets[style], lengths[style])) {
      ok = false;
      break;
    }
  }
  if (ok) {
    for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize && ok; ++points) {
      for (unsigned style = 0; style < 4; ++style) {
        if (!faces[style]->atSize(points)) {
          LOG_ERR("TTF", "Built-in size %u unavailable", unsigned(points));
          ok = false;
          break;
        }
      }
    }
  }
  if (!ok) {
    for (unsigned style = 0; style < 4; ++style) faces[style].reset();
    LOG_ERR("TTF", "Built-in family unavailable; using UI recovery font");
  }
  ready = ok;
  for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points) {
    if (ok) {
      EpdFontFamily font(faces[0]->atSize(points), faces[1]->atSize(points), faces[2]->atSize(points),
                         faces[3]->atSize(points));
      renderer.insertFont(scalableBuiltinReaderFontId(family, points), font);
      for (unsigned size = 0; size < 4; ++size)
        if (points == 10 + size * 2) renderer.insertFont(ids[size], font);
    } else {
      const auto it = renderer.getFontMap().find(UI_12_FONT_ID);
      if (it != renderer.getFontMap().end()) {
        for (unsigned size = 0; size < 4; ++size)
          if (points == 10 + size * 2) renderer.insertFont(ids[size], it->second);
      }
    }
  }
}
#endif
