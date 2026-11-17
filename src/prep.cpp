// Scan clean-up for OCR: deskew + adaptive black and white.
// Freestanding C++ (no libc), built to WebAssembly. The page calls these functions on a pixel buffer.
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long uptr;
#define EXPORT extern "C" __attribute__((visibility("default")))

extern u8 __heap_base;
static uptr top = 0;

// Tiny bump allocator over wasm linear memory. reset() gives everything back.
EXPORT u8* alloc(u32 n) {
  if (!top) top = (uptr)&__heap_base;
  top = (top + 15) & ~(uptr)15;
  uptr r = top; top += n;
  uptr need = (top + 65535) >> 16, have = __builtin_wasm_memory_size(0);
  if (need > have && __builtin_wasm_memory_grow(0, need - have) == (unsigned long)-1) return 0;
  return (u8*)r;
}
EXPORT void reset() { top = 0; }

// sin / cos for small angles (|x| < 0.5 rad), no libm needed.
static float fsin(float x) { float x2 = x * x; return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f * (1.f - x2 / 72.f)))); }
static float fcos(float x) { float x2 = x * x; return 1.f - x2 / 2.f * (1.f - x2 / 12.f * (1.f - x2 / 30.f * (1.f - x2 / 56.f))); }
static const float DEG = 0.017453292f;

EXPORT void gray_from_rgba(const u8* rgba, u8* gray, u32 n) {
  for (u32 i = 0; i < n; i++) {
    const u8* p = rgba + 4 * i;
    gray[i] = (u8)((p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8);
  }
}

EXPORT void gray_to_rgba(const u8* gray, u8* rgba, u32 n) {
  for (u32 i = 0; i < n; i++) { u8 g = gray[i]; rgba[4*i] = g; rgba[4*i+1] = g; rgba[4*i+2] = g; rgba[4*i+3] = 255; }
}

// Input is a black and white image (0 = ink), see binarize(). Skew angle in degrees (-15..15). Rotating the image clockwise by this angle (rotate_gray) levels the text lines.
// Projection profile: when lines are level, dark pixels pile up in few rows, so the sum of squares of the row counts peaks.
EXPORT float deskew_angle(const u8* g, u32 w, u32 h) {
  const int t = 128;
  u32 step = (w > h ? w : h) / 900; if (step < 1) step = 1;
  u32 maxp = ((w / step) + 1) * ((h / step) + 1);
  float* px = (float*)alloc(maxp * 2 * sizeof(float));
  if (!px) return 0;
  u32 np = 0;
  float cx = w * .5f, cy = h * .5f;
  for (u32 y = 0; y < h; y += step) for (u32 x = 0; x < w; x += step)
    if (g[y * w + x] < t) { px[2 * np] = x - cx; px[2 * np + 1] = y - cy; np++; }
  if (np < 200) return 0;
  u32 bins = h / step + 2 + (w / step);                       // rotated y can stray a little outside the page
  u32* hist = (u32*)alloc(bins * sizeof(u32));
  if (!hist) return 0;
  float off = bins * .5f;
  auto score = [&](float deg) {
    float s = fsin(deg * DEG), c = fcos(deg * DEG), inv = 1.f / step;
    for (u32 i = 0; i < bins; i++) hist[i] = 0;
    for (u32 i = 0; i < np; i++) {
      float yr = (px[2 * i] * s + px[2 * i + 1] * c) * inv + off;
      int b = (int)yr; if (b >= 0 && b < (int)bins) hist[b]++;
    }
    float sc = 0; for (u32 i = 0; i < bins; i++) sc += (float)hist[i] * hist[i];
    return sc;
  };
  float best = 0, bs = -1;
  for (float a = -15.f; a <= 15.01f; a += 0.5f) { float sc = score(a); if (sc > bs) { bs = sc; best = a; } }
  float c0 = best;
  for (float a = c0 - 0.5f; a <= c0 + 0.51f; a += 0.1f) { float sc = score(a); if (sc > bs) { bs = sc; best = a; } }
  return best;
}

// Rotate clockwise by deg (y axis points down), bilinear, same size, `fill` outside.
EXPORT void rotate_gray(const u8* src, u8* dst, u32 w, u32 h, float deg, u8 fill) {
  float s = fsin(deg * DEG), c = fcos(deg * DEG), cx = w * .5f, cy = h * .5f;
  for (u32 y = 0; y < h; y++) {
    float dy = y - cy;
    for (u32 x = 0; x < w; x++) {
      float dx = x - cx;
      float sx = cx + dx * c + dy * s, sy = cy - dx * s + dy * c;
      if (sx < 0 || sy < 0 || sx >= w - 1 || sy >= h - 1) { dst[y * w + x] = fill; continue; }
      u32 ix = (u32)sx, iy = (u32)sy; float fx = sx - ix, fy = sy - iy;
      const u8* p = src + iy * w + ix;
      float v = (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[w] * (1 - fx) + p[w + 1] * fx) * fy;
      dst[y * w + x] = (u8)(v + .5f);
    }
  }
}

// Adaptive threshold (Bradley): a pixel is ink when it is darker than `ratio` (e.g. 0.85) of the mean of its neighbourhood.
// Works with shadows and uneven light. Output: 0 = ink, 255 = paper.
EXPORT int binarize(const u8* g, u8* out, u32 w, u32 h, u32 win, float ratio) {
  u32 W = w + 1;
  u32* ii = (u32*)alloc(W * (h + 1) * sizeof(u32));
  if (!ii) return 0;
  for (u32 x = 0; x < W; x++) ii[x] = 0;
  for (u32 y = 0; y < h; y++) {
    u32 row = 0; ii[(y + 1) * W] = 0;
    for (u32 x = 0; x < w; x++) { row += g[y * w + x]; ii[(y + 1) * W + x + 1] = ii[y * W + x + 1] + row; }
  }
  int r = (int)(win / 2); if (r < 2) r = 2;
  for (u32 y = 0; y < h; y++) {
    int y0 = (int)y - r, y1 = (int)y + r + 1; if (y0 < 0) y0 = 0; if (y1 > (int)h) y1 = (int)h;
    for (u32 x = 0; x < w; x++) {
      int x0 = (int)x - r, x1 = (int)x + r + 1; if (x0 < 0) x0 = 0; if (x1 > (int)w) x1 = (int)w;
      u32 sum = ii[y1 * W + x1] - ii[y0 * W + x1] - ii[y1 * W + x0] + ii[y0 * W + x0];
      u32 cnt = (u32)((x1 - x0) * (y1 - y0));
      out[y * w + x] = (g[y * w + x] * (float)cnt < sum * ratio) ? 0 : 255;
    }
  }
  return 1;
}

// Scanner edges, page shadows and the corners left by rotating are not text: paint a thin frame white.
EXPORT void clear_border(u8* bw, u32 w, u32 h, u32 m) {
  for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++)
    if (x < m || y < m || x >= w - m || y >= h - m) bw[y * w + x] = 255;
}
