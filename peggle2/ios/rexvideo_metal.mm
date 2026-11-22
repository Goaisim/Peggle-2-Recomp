// Metal GraphicsSystem, CommandProcessor, Presenter, ImmediateDrawer, Texture cache, and
// zero-copy 320x180 ScreenWatcher readback for iOS (adapted from peggle2-mac onto ReXGlue).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include <dispatch/dispatch.h>

#include "ios_bridge.h"
#include "msl_prelude.h"
#include "msl_translator.h"
#include "../src/rex_engine_monitor.h"

#include "graphics/ui_quad_sniffer.h"

#include <rex/assert.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string/buffer.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/graphics_util.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex_ios {
namespace {

using namespace rex::graphics;

struct XeDraw {
  float ndcScale[4];
  float ndcOffset[4];
  uint32_t indexed, indexBase, indexFormat, indexEndian;
  int32_t indexOffset;
  uint32_t flags, primitive, alphaFunc;
  float alphaRef;
  float pixelScale;
  uint32_t memSplitWord;
  uint32_t pad;
};
static_assert(sizeof(XeDraw) == 80);

struct ResolveParams {
  uint32_t x0, y0, width, height;
  uint32_t pitchAligned, heightAligned;
  uint32_t destBase, endian, swap, clearValue;
  uint32_t scale, memSplitByte;
};

static uint32_t SwapWord(uint32_t value, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
    case xenos::Endian::k8in32:
      return __builtin_bswap32(value);
    case xenos::Endian::k16in32:
      return (value >> 16) | (value << 16);
    default:
      return value;
  }
}

static uint32_t SwapWordU(uint32_t value, uint32_t endian) {
  return SwapWord(value, static_cast<xenos::Endian>(endian & 3u));
}

static uint8_t ExpandBits(uint32_t v, uint32_t bits) {
  const uint32_t mx = (1u << bits) - 1u;
  return uint8_t((v * 255u + mx / 2u) / mx);
}

static void DecodeRgb565(uint16_t c, uint8_t out[3]) {
  out[0] = ExpandBits((c >> 11) & 31u, 5);
  out[1] = ExpandBits((c >> 5) & 63u, 6);
  out[2] = ExpandBits(c & 31u, 5);
}

static void DecodeDxtBlockToRgba(int dxt_type, const uint8_t* blk, uint8_t* dst, size_t dst_stride) {
  uint8_t c[4][4]{};
  const uint8_t* color_src = (dxt_type == 1) ? blk : (blk + 8);
  const uint16_t c0 = uint16_t(color_src[0]) | (uint16_t(color_src[1]) << 8);
  const uint16_t c1 = uint16_t(color_src[2]) | (uint16_t(color_src[3]) << 8);
  DecodeRgb565(c0, c[0]);
  DecodeRgb565(c1, c[1]);
  c[0][3] = 255;
  c[1][3] = 255;
  if (dxt_type != 1 || c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      c[2][i] = uint8_t((2u * c[0][i] + c[1][i]) / 3u);
      c[3][i] = uint8_t((c[0][i] + 2u * c[1][i]) / 3u);
    }
    c[2][3] = 255;
    c[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      c[2][i] = uint8_t((c[0][i] + c[1][i]) / 2u);
      c[3][i] = 0;
    }
    c[2][3] = 255;
    c[3][3] = 0;
  }
  const uint32_t lut = uint32_t(color_src[4]) | (uint32_t(color_src[5]) << 8) |
                       (uint32_t(color_src[6]) << 16) | (uint32_t(color_src[7]) << 24);
  uint8_t dxt5_a[8]{};
  uint64_t dxt5_bits = 0;
  if (dxt_type == 5) {
    dxt5_a[0] = blk[0];
    dxt5_a[1] = blk[1];
    if (dxt5_a[0] > dxt5_a[1]) {
      for (int i = 0; i < 6; ++i) dxt5_a[2 + i] = uint8_t(((6 - i) * dxt5_a[0] + (1 + i) * dxt5_a[1]) / 7);
    } else {
      for (int i = 0; i < 4; ++i) dxt5_a[2 + i] = uint8_t(((4 - i) * dxt5_a[0] + (1 + i) * dxt5_a[1]) / 5);
      dxt5_a[6] = 0;
      dxt5_a[7] = 255;
    }
    for (int i = 0; i < 6; ++i) dxt5_bits |= uint64_t(blk[2 + i]) << (i * 8);
  }
  for (uint32_t py = 0; py < 4; ++py) {
    uint8_t* row = dst + py * dst_stride;
    for (uint32_t px = 0; px < 4; ++px) {
      const uint32_t idx = py * 4 + px;
      const uint32_t ci = (lut >> (idx * 2)) & 3u;
      row[px * 4 + 0] = c[ci][0];
      row[px * 4 + 1] = c[ci][1];
      row[px * 4 + 2] = c[ci][2];
      uint8_t alpha = c[ci][3];
      if (dxt_type == 3) {
        const uint8_t ab = blk[py * 2 + (px >> 1)];
        const uint8_t a4 = (px & 1u) ? (ab >> 4) : (ab & 0xFu);
        alpha = uint8_t(a4 * 17u);
      } else if (dxt_type == 5) {
        alpha = dxt5_a[(dxt5_bits >> (idx * 3)) & 7u];
      }
      row[px * 4 + 3] = alpha;
    }
  }
}

class MetalShader final : public Shader {
 public:
  MetalShader(xenos::ShaderType type, uint64_t hash, const uint32_t* dwords, size_t count)
      : Shader(type, hash, dwords, count) {}

  MslShaderInfo info{};
  id<MTLLibrary> library = nil;
  std::unordered_map<uint32_t, id<MTLFunction>> specialized;

  id<MTLFunction> Specialize(uint32_t interpolants, uint32_t alpha_func) {
    if (!library) return nil;
    const uint32_t key = interpolants | (alpha_func << 16);
    __strong id<MTLFunction>& fn = specialized[key];
    if (!fn) {
      MTLFunctionConstantValues* vals = [MTLFunctionConstantValues new];
      [vals setConstantValue:&interpolants type:MTLDataTypeUInt atIndex:0];
      [vals setConstantValue:&alpha_func type:MTLDataTypeUInt atIndex:1];
      NSError* err = nil;
      fn = [library newFunctionWithName:info.isPixel ? @"xe_fragment" : @"xe_vertex"
                         constantValues:vals
                                  error:&err];
    }
    return fn;
  }
};

static std::atomic<uint32_t> g_perf_draws{0};
static std::atomic<uint32_t> g_perf_tex_uploads{0};
static std::atomic<uint32_t> g_perf_buf_allocs{0};
static std::atomic<uint32_t> g_cur_buf_allocs{0};
static std::atomic<uint64_t> g_guest_frame_seq{0};
static std::mutex g_metal_cmd_mutex;
static std::atomic<uint64_t> g_next_metal_cmd_id{1};
static id<MTLCommandBuffer> g_last_inflight_cmd = nil;
static uint64_t g_last_inflight_cmd_id = 0;

static uint64_t PrepareTrackedMetalCmd(id<MTLCommandBuffer> cmd) {
  if (!cmd) return 0;
  const uint64_t cmd_id = g_next_metal_cmd_id.fetch_add(1, std::memory_order_relaxed);
  [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
    std::lock_guard<std::mutex> lock(g_metal_cmd_mutex);
    if (g_last_inflight_cmd_id == cmd_id) {
      g_last_inflight_cmd = nil;
      g_last_inflight_cmd_id = 0;
    }
  }];
  return cmd_id;
}

static void MarkCommittedMetalCmd(id<MTLCommandBuffer> cmd, uint64_t cmd_id) {
  if (!cmd || !cmd_id) return;
  const MTLCommandBufferStatus st = cmd.status;
  if (st == MTLCommandBufferStatusCompleted || st == MTLCommandBufferStatusError) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_metal_cmd_mutex);
  if (cmd_id > g_last_inflight_cmd_id &&
      cmd.status != MTLCommandBufferStatusCompleted &&
      cmd.status != MTLCommandBufferStatusError) {
    g_last_inflight_cmd = cmd;
    g_last_inflight_cmd_id = cmd_id;
  }
}

static uint64_t SampleTexFingerprint(const uint8_t* base, uint32_t extent) {
  if (!base || extent < 256u) {
    return XXH3_64bits(base, extent);
  }
  uint8_t sample[256];
  const uint32_t max_off = extent - 64u;
  std::memcpy(sample + 0, base, 64);
  std::memcpy(sample + 64, base + (max_off / 3u), 64);
  std::memcpy(sample + 128, base + ((max_off * 2u) / 3u), 64);
  std::memcpy(sample + 192, base + max_off, 64);
  return XXH3_64bits(sample, sizeof(sample)) ^ (uint64_t(extent) << 32);
}

class MetalImmediateTexture final : public rex::ui::ImmediateTexture {
 public:
  MetalImmediateTexture(uint32_t width, uint32_t height, id<MTLTexture> tex,
                        rex::ui::ImmediateTextureFilter filter, bool repeated)
      : ImmediateTexture(width, height),
        texture(tex),
        linear(filter == rex::ui::ImmediateTextureFilter::kLinear),
        repeat(repeated) {}
  id<MTLTexture> texture = nil;
  bool linear = true;
  bool repeat = false;
};

class MetalUIDrawContext final : public rex::ui::UIDrawContext {
 public:
  MetalUIDrawContext(rex::ui::Presenter& presenter, uint32_t w, uint32_t h,
                     id<MTLRenderCommandEncoder> enc)
      : UIDrawContext(presenter, w, h), encoder(enc) {}
  id<MTLRenderCommandEncoder> encoder = nil;
};

class MetalImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit MetalImmediateDrawer(id<MTLDevice> device) : device_(device) {
    NSError* err = nil;
    id<MTLLibrary> lib = [device_ newLibraryWithSource:@(kPresentAndUiShaders)
                                               options:nil
                                                 error:&err];
    if (!lib) {
      REXLOG_ERROR("Metal UI shader compile error: {}", err.localizedDescription.UTF8String);
      return;
    }
    MTLVertexDescriptor* vdesc = [MTLVertexDescriptor vertexDescriptor];
    vdesc.attributes[0].format = MTLVertexFormatFloat2;
    vdesc.attributes[0].offset = offsetof(rex::ui::ImmediateVertex, x);
    vdesc.attributes[0].bufferIndex = 0;
    vdesc.attributes[1].format = MTLVertexFormatFloat2;
    vdesc.attributes[1].offset = offsetof(rex::ui::ImmediateVertex, u);
    vdesc.attributes[1].bufferIndex = 0;
    vdesc.attributes[2].format = MTLVertexFormatUChar4Normalized;
    vdesc.attributes[2].offset = offsetof(rex::ui::ImmediateVertex, color);
    vdesc.attributes[2].bufferIndex = 0;
    vdesc.layouts[0].stride = sizeof(rex::ui::ImmediateVertex);

    auto make_pso = [&](NSString* frag_name) {
      MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
      p.vertexFunction = [lib newFunctionWithName:@"ui_vertex"];
      p.fragmentFunction = [lib newFunctionWithName:frag_name];
      p.vertexDescriptor = vdesc;
      p.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      p.colorAttachments[0].blendingEnabled = YES;
      p.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
      p.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      p.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
      p.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      NSError* perr = nil;
      return [device_ newRenderPipelineStateWithDescriptor:p error:&perr];
    };
    pso_tex_ = make_pso(@"ui_fragment_tex");
    pso_solid_ = make_pso(@"ui_fragment_solid");

    for (int f = 0; f < 2; ++f) {
      for (int r = 0; r < 2; ++r) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = f ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = f ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = r ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = r ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        samplers_[f][r] = [device_ newSamplerStateWithDescriptor:sd];
      }
    }
  }

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(
      uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool is_repeated,
      const uint8_t* data) override {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:std::max(1u, width)
                                                          height:std::max(1u, height)
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [device_ newTextureWithDescriptor:desc];
    if (tex && data && width > 0 && height > 0) {
      [tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
               withBytes:data
             bytesPerRow:width * 4];
    }
    return std::make_unique<MetalImmediateTexture>(width, height, tex, filter, is_repeated);
  }

  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override {
    ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
    auto& ctx = static_cast<MetalUIDrawContext&>(ui_draw_context);
    encoder_ = ctx.encoder;
    rt_w_ = ctx.render_target_width();
    rt_h_ = ctx.render_target_height();
    ui_overflow_bufs_[ui_slot_].clear();
    batch_used_primary_ = false;
    has_batch_vbuf_ = false;
    active_ib_ = nil;
    if (encoder_) {
      float inv_size[2] = {1.0f / std::max(1.0f, this->coordinate_space_width()),
                           1.0f / std::max(1.0f, this->coordinate_space_height())};
      [encoder_ setVertexBytes:inv_size length:sizeof(inv_size) atIndex:1];
    }
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    batch_ = batch;
    has_batch_vbuf_ = false;
    active_ib_ = nil;
    if (!batch_.vertices || batch_.vertex_count <= 0) return;
    const size_t vbytes = size_t(batch_.vertex_count) * sizeof(rex::ui::ImmediateVertex);
    id<MTLBuffer> active_vb = nil;
    if (!batch_used_primary_) {
      __strong id<MTLBuffer>& vb = vbuf_[ui_slot_];
      if (!vb || vb.length < vbytes) {
        vb = [device_ newBufferWithLength:std::max<size_t>(vbytes, 65536)
                                 options:MTLResourceStorageModeShared];
        if (vb) g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
      }
      active_vb = vb;
    } else {
      active_vb = [device_ newBufferWithLength:std::max<size_t>(vbytes, 65536)
                                       options:MTLResourceStorageModeShared];
      if (active_vb) {
        g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
        ui_overflow_bufs_[ui_slot_].push_back(active_vb);
      }
    }
    if (!active_vb || !active_vb.contents || active_vb.length < vbytes) return;
    std::memcpy(active_vb.contents, batch_.vertices, vbytes);

    if (batch_.indices && batch_.index_count > 0) {
      const size_t ibytes = size_t(batch_.index_count) * sizeof(uint16_t);
      id<MTLBuffer> ib_target = nil;
      if (!batch_used_primary_) {
        __strong id<MTLBuffer>& ib = ibuf_[ui_slot_];
        if (!ib || ib.length < ibytes) {
          ib = [device_ newBufferWithLength:std::max<size_t>(ibytes, 65536)
                                   options:MTLResourceStorageModeShared];
          if (ib) g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
        }
        ib_target = ib;
      } else {
        ib_target = [device_ newBufferWithLength:std::max<size_t>(ibytes, 65536)
                                         options:MTLResourceStorageModeShared];
        if (ib_target) {
          g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
          ui_overflow_bufs_[ui_slot_].push_back(ib_target);
        }
      }
      if (!ib_target || !ib_target.contents || ib_target.length < ibytes) return;
      std::memcpy(ib_target.contents, batch_.indices, ibytes);
      active_ib_ = ib_target;
    }
    batch_used_primary_ = true;
    if (encoder_) {
      [encoder_ setVertexBuffer:active_vb offset:0 atIndex:0];
      has_batch_vbuf_ = true;
    }
  }

  void Draw(const rex::ui::ImmediateDraw& draw) override {
    if (!encoder_ || !has_batch_vbuf_ || draw.count <= 0 || draw.base_vertex < 0) return;
    uint32_t sx = 0, sy = 0, sw = rt_w_, sh = rt_h_;
    if (draw.scissor) {
      if (!ScissorToRenderTarget(draw, sx, sy, sw, sh) || sw == 0 || sh == 0) {
        return;
      }
    }
    [encoder_ setScissorRect:{sx, sy, sw, sh}];
    auto* mtex = static_cast<MetalImmediateTexture*>(draw.texture);
    if (mtex && mtex->texture) {
      [encoder_ setRenderPipelineState:pso_tex_];
      [encoder_ setFragmentTexture:mtex->texture atIndex:0];
      [encoder_ setFragmentSamplerState:samplers_[mtex->linear ? 1 : 0][mtex->repeat ? 1 : 0]
                                atIndex:0];
    } else {
      [encoder_ setRenderPipelineState:pso_solid_];
    }
    const MTLPrimitiveType pt = (draw.primitive_type == rex::ui::ImmediatePrimitiveType::kLines)
                                    ? MTLPrimitiveTypeLine
                                    : MTLPrimitiveTypeTriangle;
    if (batch_.indices && batch_.index_count > 0) {
      if (!active_ib_ || draw.index_offset < 0 ||
          size_t(draw.index_offset) + size_t(draw.count) > size_t(batch_.index_count) ||
          (size_t(draw.index_offset) + size_t(draw.count)) * sizeof(uint16_t) > active_ib_.length) {
        return;
      }
      [encoder_ drawIndexedPrimitives:pt
                           indexCount:draw.count
                            indexType:MTLIndexTypeUInt16
                          indexBuffer:active_ib_
                    indexBufferOffset:size_t(draw.index_offset) * sizeof(uint16_t)
                        instanceCount:1
                           baseVertex:draw.base_vertex
                         baseInstance:0];
    } else {
      if (size_t(draw.count) > size_t(batch_.vertex_count)) {
        return;
      }
      [encoder_ drawPrimitives:pt vertexStart:0 vertexCount:draw.count];
    }
  }

  void EndDrawBatch() override {
    batch_ = {};
    has_batch_vbuf_ = false;
    active_ib_ = nil;
  }

  void End() override {
    encoder_ = nil;
    has_batch_vbuf_ = false;
    active_ib_ = nil;
    ui_slot_ = (ui_slot_ + 1) % kUiRingCount;
    ImmediateDrawer::End();
  }

 private:
  static constexpr int kUiRingCount = 3;
  id<MTLDevice> device_ = nil;
  id<MTLRenderPipelineState> pso_tex_ = nil;
  id<MTLRenderPipelineState> pso_solid_ = nil;
  id<MTLSamplerState> samplers_[2][2]{};
  id<MTLBuffer> vbuf_[kUiRingCount]{};
  id<MTLBuffer> ibuf_[kUiRingCount]{};
  std::vector<id<MTLBuffer>> ui_overflow_bufs_[kUiRingCount];
  int ui_slot_ = 0;
  bool batch_used_primary_ = false;
  bool has_batch_vbuf_ = false;
  id<MTLBuffer> active_ib_ = nil;
  rex::ui::ImmediateDrawBatch batch_{};
  id<MTLRenderCommandEncoder> encoder_ = nil;
  uint32_t rt_w_ = 0;
  uint32_t rt_h_ = 0;
};

static id<MTLTexture> g_guest_frame_tex = nil;
static id<MTLTexture> g_guest_frontbuffer_mtl = nil;

class MetalPresenter final : public rex::ui::Presenter {
 public:
  MetalPresenter(id<MTLDevice> device, id<MTLCommandQueue> queue, HostGpuLossCallback cb)
      : Presenter(cb), device_(device), queue_(queue) {
    InitializeCommonSurfaceIndependent();
    NSError* err = nil;
    id<MTLLibrary> lib = [device_ newLibraryWithSource:@(kPresentAndUiShaders)
                                               options:nil
                                                 error:&err];
    if (lib) {
      MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
      p.vertexFunction = [lib newFunctionWithName:@"present_vertex"];
      p.fragmentFunction = [lib newFunctionWithName:@"present_fragment"];
      p.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      present_pso_ = [device_ newRenderPipelineStateWithDescriptor:p error:&err];
    }
    MTLTextureDescriptor* gd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:256
                                                                                 height:1
                                                                              mipmapped:NO];
    gd.usage = MTLTextureUsageShaderRead;
    gd.storageMode = MTLStorageModeShared;
    default_gamma_ = [device_ newTextureWithDescriptor:gd];
    uint32_t ramp[256];
    for (uint32_t i = 0; i < 256; ++i) {
      ramp[i] = i | (i << 8) | (i << 16) | 0xFF000000u;
    }
    [default_gamma_ replaceRegion:MTLRegionMake2D(0, 0, 256, 1)
                      mipmapLevel:0
                        withBytes:ramp
                      bytesPerRow:sizeof(ramp)];
  }

  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override {
    return rex::ui::Surface::kTypeFlag_CAMetalLayer;
  }

  bool CaptureGuestOutput(rex::ui::RawImage& image_out) override {
    id<MTLTexture> tex = nil;
    uint32_t w = 0, h = 0;
    {
      std::lock_guard<std::mutex> lock(fb_mutex_);
      tex = guest_fb_;
      w = guest_fb_w_;
      h = guest_fb_h_;
    }
    if (!tex || !w || !h) return false;
    image_out.width = w;
    image_out.height = h;
    image_out.stride = size_t(w) * 4;
    image_out.data.resize(size_t(w) * h * 4);
    MTLTextureDescriptor* sd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:w
                                                          height:h
                                                       mipmapped:NO];
    sd.storageMode = MTLStorageModeShared;
    id<MTLTexture> staging = [device_ newTextureWithDescriptor:sd];
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                toTexture:staging
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    [staging getBytes:image_out.data.data()
          bytesPerRow:image_out.stride
           fromRegion:MTLRegionMake2D(0, 0, w, h)
          mipmapLevel:0];
    return true;
  }

  void PublishGuestTexture(id<MTLTexture> tex, uint32_t fb_w, uint32_t fb_h, float uv_sx,
                           float uv_sy, id<MTLTexture> gamma_lut) {
    {
      std::lock_guard<std::mutex> lock(fb_mutex_);
      g_guest_frame_tex = tex;
      g_guest_frontbuffer_mtl = tex;
      guest_fb_ = tex;
      guest_fb_w_ = fb_w;
      guest_fb_h_ = fb_h;
      uv_scale_[0] = uv_sx;
      uv_scale_[1] = uv_sy;
      gamma_lut_ = gamma_lut;
    }
    g_guest_frame_seq.fetch_add(1, std::memory_order_release);
    RefreshGuestOutput(fb_w, fb_h, 1280, 720, [](GuestOutputRefreshContext& ctx) {
      ctx.SetIs8bpc(true);
      return true;
    });
  }

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      rex::ui::Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override {
    (void)new_surface_width;
    (void)new_surface_height;
    (void)was_paintable;
    if (new_surface.GetType() != rex::ui::Surface::kTypeIndex_CAMetalLayer) {
      return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    layer_ = current_ios_metal_layer();
    if (!layer_) return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    is_vsync_implicit_out = true;
    return SurfacePaintConnectResult::kSuccess;
  }

  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override { layer_ = nil; }

  bool RefreshGuestOutputImpl(uint32_t, uint32_t, uint32_t,
                              std::function<bool(GuestOutputRefreshContext&)> refresher,
                              bool& is_8bpc_out_ref) override {
    class RefreshCtx final : public GuestOutputRefreshContext {
     public:
      explicit RefreshCtx(bool& ref) : GuestOutputRefreshContext(ref) {}
    } ctx(is_8bpc_out_ref);
    return refresher ? refresher(ctx) : true;
  }

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override {
    if (rex::IsAppBackgroundPaused()) return PaintResult::kNotPresented;
    CAMetalLayer* layer = layer_;
    if (!layer) return PaintResult::kNotPresented;
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) return PaintResult::kNotPresented;

    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    if (!cmd) return PaintResult::kNotPresented;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    const uint32_t dw = uint32_t(drawable.texture.width);
    const uint32_t dh = uint32_t(drawable.texture.height);

    id<MTLTexture> fb = nil;
    id<MTLTexture> lut = nil;
    uint32_t fb_w = 0, fb_h = 0;
    float uv[2] = {1.0f, 1.0f};
    {
      std::lock_guard<std::mutex> lock(fb_mutex_);
      fb = guest_fb_ ? guest_fb_ : (g_guest_frame_tex ? g_guest_frame_tex : g_guest_frontbuffer_mtl);
      lut = gamma_lut_ ? gamma_lut_ : default_gamma_;
      fb_w = guest_fb_w_ ? guest_fb_w_ : (fb ? uint32_t(fb.width) : 0u);
      fb_h = guest_fb_h_ ? guest_fb_h_ : (fb ? uint32_t(fb.height) : 0u);
      uv[0] = uv_scale_[0];
      uv[1] = uv_scale_[1];
    }

    if (fb && fb_w > 0 && fb_h > 0 && present_pso_) {
      double scale = std::min(double(dw) / fb_w, double(dh) / fb_h);
      double w = fb_w * scale, h = fb_h * scale;
      [enc setViewport:{(dw - w) * 0.5, (dh - h) * 0.5, w, h, 0.0, 1.0}];
      [enc setScissorRect:{0, 0, dw, dh}];
      [enc setRenderPipelineState:present_pso_];
      [enc setFragmentBytes:uv length:sizeof(uv) atIndex:0];
      [enc setFragmentTexture:fb atIndex:0];
      [enc setFragmentTexture:lut atIndex:1];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }

    if (execute_ui_drawers) {
      [enc setViewport:{0.0, 0.0, double(dw), double(dh), 0.0, 1.0}];
      [enc setScissorRect:{0, 0, dw, dh}];
      MetalUIDrawContext ui_ctx(*this, dw, dh, enc);
      ExecuteUIDrawersFromUIThread(ui_ctx);
    }

    [enc endEncoding];
    [cmd presentDrawable:drawable];
    const uint64_t cmd_id = PrepareTrackedMetalCmd(cmd);
    [cmd commit];
    MarkCommittedMetalCmd(cmd, cmd_id);
    RecordHostPresent();
    if (fb) {
      RecordRenderedFrame();
    }
    return PaintResult::kPresented;
  }

  static CAMetalLayer* current_ios_metal_layer();

 private:
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> present_pso_ = nil;
  id<MTLTexture> default_gamma_ = nil;
  CAMetalLayer* layer_ = nil;
  std::mutex fb_mutex_;
  id<MTLTexture> guest_fb_ = nil;
  id<MTLTexture> gamma_lut_ = nil;
  uint32_t guest_fb_w_ = 0;
  uint32_t guest_fb_h_ = 0;
  float uv_scale_[2] = {1.0f, 1.0f};
};

// Retrieve layer attached by IOSWindowedApp / PeggleViewController.
extern "C" void* rex_ios_get_metal_layer_ptr();
CAMetalLayer* MetalPresenter::current_ios_metal_layer() {
  return (__bridge CAMetalLayer*)rex_ios_get_metal_layer_ptr();
}

class MetalGraphicsProvider final : public rex::ui::GraphicsProvider {
 public:
  MetalGraphicsProvider(id<MTLDevice> device, id<MTLCommandQueue> queue)
      : device_(device), queue_(queue) {}

  std::unique_ptr<rex::ui::Presenter> CreatePresenter(
      rex::ui::Presenter::HostGpuLossCallback cb) override {
    auto p = std::make_unique<MetalPresenter>(device_, queue_, cb);
    latest_presenter_ = p.get();
    return p;
  }

  std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer() override {
    return std::make_unique<MetalImmediateDrawer>(device_);
  }

  MetalPresenter* latest_presenter() const { return latest_presenter_; }

 private:
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  MetalPresenter* latest_presenter_ = nullptr;
};

class MetalCommandProcessor final : public CommandProcessor {
 public:
  MetalCommandProcessor(GraphicsSystem* gs, rex::system::KernelState* ks, id<MTLDevice> dev,
                        id<MTLCommandQueue> q, MetalGraphicsProvider* prov)
      : CommandProcessor(gs, ks), device_(dev), queue_(q), provider_(prov) {}

 protected:
  bool SetupContext() override {
    // Map guest physical memory zero-copy for unified Apple Silicon / A-series GPU access.
    uint8_t* phys = memory_->TranslatePhysical(0);
    gpu_mem_ = nil;
    gpu_mem_hi_ = nil;
    gpu_mem_split_byte_ = 0xFFFFFFFFu;
    if (phys) {
      const size_t max_buf = size_t([device_ maxBufferLength]);
      if (max_buf >= 0x20000000ULL) {
        gpu_mem_ = [device_ newBufferWithBytesNoCopy:phys
                                              length:0x20000000
                                             options:MTLResourceStorageModeShared
                                         deallocator:nil];
      }
      if (!gpu_mem_ && max_buf >= 0x1FFF0000ULL) {
        gpu_mem_ = [device_ newBufferWithBytesNoCopy:phys
                                              length:0x1FFF0000
                                             options:MTLResourceStorageModeShared
                                         deallocator:nil];
      }
      if (!gpu_mem_) {
        // Older 3-4 GB iPhones cap MTLBuffer at 256 MB (0x10000000); split 512 MB across lo+hi.
        constexpr size_t kHalf = 0x10000000ULL;
        gpu_mem_ = [device_ newBufferWithBytesNoCopy:phys
                                              length:kHalf
                                             options:MTLResourceStorageModeShared
                                         deallocator:nil];
        if (gpu_mem_) {
          gpu_mem_hi_ = [device_ newBufferWithBytesNoCopy:phys + kHalf
                                                   length:kHalf
                                                  options:MTLResourceStorageModeShared
                                              deallocator:nil];
          if (!gpu_mem_hi_) {
            gpu_mem_hi_ = [device_ newBufferWithBytesNoCopy:phys + kHalf
                                                     length:0x0FFF0000ULL
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
          }
          if (gpu_mem_hi_) {
            gpu_mem_split_byte_ = uint32_t(kHalf);
          }
        }
      }
    }
    if (!gpu_mem_) {
      REXLOG_ERROR("MetalCommandProcessor: failed to map physical guest memory");
      rex::SetBootError("Metal gpu_mem map failed");
      return false;
    }

    MTLTextureDescriptor* dummy_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    dummy_tex_ = [device_ newTextureWithDescriptor:dummy_desc];

    NSError* err = nil;
    id<MTLLibrary> res_lib = [device_ newLibraryWithSource:@(kResolveShaders) options:nil error:&err];
    if (!res_lib) {
      REXLOG_ERROR("Metal resolve shaders compile failed: {}", err.localizedDescription.UTF8String);
      return false;
    }
    resolve_pso_ =
        [device_ newComputePipelineStateWithFunction:[res_lib newFunctionWithName:@"resolve"]
                                               error:&err];
    clear_pso_ =
        [device_ newComputePipelineStateWithFunction:[res_lib newFunctionWithName:@"clear"]
                                               error:&err];

    id<MTLLibrary> ui_lib = [device_ newLibraryWithSource:@(kPresentAndUiShaders) options:nil error:&err];
    if (ui_lib) {
      MTLRenderPipelineDescriptor* swp = [MTLRenderPipelineDescriptor new];
      swp.vertexFunction = [ui_lib newFunctionWithName:@"present_vertex"];
      swp.fragmentFunction = [ui_lib newFunctionWithName:@"watcher_downsample_fragment"];
      swp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
      watcher_pso_ = [device_ newRenderPipelineStateWithDescriptor:swp error:&err];
    }

    for (int i = 0; i < 2; ++i) {
      MTLTextureDescriptor* sd =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                             width:320
                                                            height:180
                                                         mipmapped:NO];
      sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      sd.storageMode = MTLStorageModePrivate;
      watcher_tex_[i] = [device_ newTextureWithDescriptor:sd];
      watcher_buf_[i] = [device_ newBufferWithLength:320u * 180u * 4u
                                             options:MTLResourceStorageModeShared];
      g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
      watcher_busy_[i].store(false, std::memory_order_relaxed);
      watcher_done_[i].store(false, std::memory_order_relaxed);
    }

    handler_tracker_ = std::make_shared<CmdHandlerTracker>();
    ring_sem_ = dispatch_semaphore_create(kConstRingSlots - 1);
    for (int i = 0; i < kConstRingSlots; ++i) {
      const_ring_[i] = [device_ newBufferWithLength:kConstRingBytes
                                           options:MTLResourceStorageModeShared];
      const_ring_ptr_[i] =
          const_ring_[i] ? static_cast<uint8_t*>([const_ring_[i] contents]) : nullptr;
      if (const_ring_[i]) g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
      frame_overflow_bufs_[i].clear();
      frame_overflow_textures_[i].clear();
    }
    ring_slot_ = 0;
    swap_index_ = 0;
    ring_offset_ = 0;
    ResetConstCache();

    if (memory_ && register_file_) {
      ui_sniffer_ = std::make_unique<UiQuadSniffer>(*register_file_, *memory_);
    }
    return resolve_pso_ != nil && clear_pso_ != nil;
  }

  void ShutdownContext() override {
    EndPass();
    Submit(true);
    rex_ios_wait_for_metal_idle();
    if (handler_tracker_) {
      std::unique_lock<std::mutex> lock(handler_tracker_->mutex);
      handler_tracker_->cv.wait_for(
          lock, std::chrono::milliseconds(500),
          [this]() { return handler_tracker_->pending <= 0; });
      handler_tracker_->alive = false;
    }
    last_submitted_cmd_ = nil;
    ui_sniffer_.reset();
    textures_.clear();
    resolved_.clear();
    targets_.clear();
    pipelines_.clear();
    depth_states_.clear();
    shaders_.clear();
    samplers_.clear();
    for (int i = 0; i < kConstRingSlots; ++i) {
      const_ring_[i] = nil;
      const_ring_ptr_[i] = nullptr;
      frame_overflow_bufs_[i].clear();
      frame_overflow_textures_[i].clear();
    }
    ring_sem_ = nil;
    gpu_mem_ = nil;
    gpu_mem_hi_ = nil;
  }

  void OnPrimaryBufferEnd() override {
    if (commands_) {
      Submit(false);
    }
  }

  void PrepareForWait() override {
    if (commands_) {
      Submit(false);
    }
  }

  void MakeCoherent() override {
    CommandProcessor::MakeCoherent();
    if (commands_ || last_submitted_cmd_) {
      Submit(true);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t, const uint32_t* host_address,
                     uint32_t dword_count) override {
    const uint64_t hash = XXH3_64bits(host_address, size_t(dword_count) * 4) +
                          (shader_type == xenos::ShaderType::kPixel ? 1ull : 0ull);
    auto [it, inserted] = shaders_.try_emplace(hash);
    if (!inserted && it->second) {
      return it->second.get();
    }

    auto shader = std::make_unique<MetalShader>(shader_type, hash, host_address, dword_count);
    rex::string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    MslTranslator translator;
    std::string body = translator.Translate(*shader, shader->info);
    std::string src = std::string(kMslPrelude) + body +
                      (shader_type == xenos::ShaderType::kPixel ? FragmentEntry(shader->info)
                                                                : kMslVertexEntry);

    @autoreleasepool {
      MTLCompileOptions* opt = [MTLCompileOptions new];
      if (@available(iOS 18.0, *)) {
        opt.mathMode = MTLMathModeSafe;
      } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
      }
      NSError* err = nil;
      shader->library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                              options:opt
                                                error:&err];
      if (!shader->library) {
        REXLOG_ERROR("Metal shader {:016X} failed: {}", hash,
                     err ? err.localizedDescription.UTF8String : "unknown");
        rex::SetBootError("Metal shader compile failed");
      } else {
        rex::RecordShaderCompiled();
      }
    }
    Shader* raw = shader.get();
    it->second = std::move(shader);
    return raw;
  }

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool) override {
    auto& regs = *register_file_;
    auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
    if (mode == xenos::EdramMode::kCopy) {
      return IssueCopy();
    }
    const bool depth_only = (mode == xenos::EdramMode::kDepthOnly);
    if (mode != xenos::EdramMode::kColorDepth && !depth_only) {
      return true;
    }

    auto* vs = static_cast<MetalShader*>(active_vertex_shader_);
    auto* ps = static_cast<MetalShader*>(active_pixel_shader_);
    if (!vs || !vs->library || !ps || !ps->library) {
      return false;
    }

    MTLPrimitiveType mtl_prim;
    uint32_t count = index_count;
    switch (prim_type) {
      case xenos::PrimitiveType::kPointList: mtl_prim = MTLPrimitiveTypePoint; break;
      case xenos::PrimitiveType::kLineList: mtl_prim = MTLPrimitiveTypeLine; break;
      case xenos::PrimitiveType::kLineStrip: mtl_prim = MTLPrimitiveTypeLineStrip; break;
      case xenos::PrimitiveType::kTriangleList: mtl_prim = MTLPrimitiveTypeTriangle; break;
      case xenos::PrimitiveType::kTriangleStrip: mtl_prim = MTLPrimitiveTypeTriangleStrip; break;
      case xenos::PrimitiveType::kTriangleFan:
        mtl_prim = MTLPrimitiveTypeTriangle;
        count = count >= 3 ? (count - 2) * 3 : 0;
        break;
      case xenos::PrimitiveType::kRectangleList:
        mtl_prim = MTLPrimitiveTypeTriangle;
        count = count / 3 * 6;
        break;
      case xenos::PrimitiveType::kQuadList:
        mtl_prim = MTLPrimitiveTypeTriangle;
        count = count / 4 * 6;
        break;
      default:
        return false;
    }
    if (!count) return true;

    auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    const bool polygonal =
        (mtl_prim == MTLPrimitiveTypeTriangle || mtl_prim == MTLPrimitiveTypeTriangleStrip);
    if (polygonal && sc.cull_front && sc.cull_back) return true;

    const uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
    const uint32_t color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
    id<MTLTexture> colors[4] = {};
    uint32_t blend[4] = {};
    id<MTLTexture> size_ref = nil;
    for (uint32_t i = 0; i < 4; ++i) {
      if (depth_only || !(ps->info.colorTargets & (1u << i)) || !((color_mask >> (i * 4)) & 0xF)) {
        continue;
      }
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      colors[i] = GetTarget(info.color_base, pitch, uint32_t(info.color_format), false);
      blend[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]];
      if (!size_ref) size_ref = colors[i];
    }

    reg::RB_DEPTHCONTROL depth_ctrl = draw_util::GetNormalizedDepthControl(regs);
    id<MTLTexture> depth = nil;
    if (depth_ctrl.z_enable || depth_ctrl.stencil_enable) {
      auto info = regs.Get<reg::RB_DEPTH_INFO>();
      depth = GetTarget(info.depth_base, pitch, uint32_t(info.depth_format), true);
      if (!size_ref) size_ref = depth;
    }
    if (!size_ref) return true;

    const uint32_t width = uint32_t(size_ref.width);
    const uint32_t height = uint32_t(size_ref.height);

    draw_util::Scissor scissor{};
    draw_util::GetScissor(regs, scissor);
    uint32_t sx = std::min(scissor.offset[0], width);
    uint32_t sy = std::min(scissor.offset[1], height);
    uint32_t sw = std::min(scissor.extent[0], width - sx);
    uint32_t sh = std::min(scissor.extent[1], height - sy);
    if (!sw || !sh) return true;

    draw_util::ViewportInfo vp{};
    draw_util::GetHostViewportInfo(regs, 1, 1, true, width, height, false, depth_ctrl, false, false,
                                   ps->info.writesDepth, vp);

    id<MTLTexture> textures[32]{};
    id<MTLSamplerState> samplers[32]{};
    float tex_scales[32]{};
    uint32_t tex_count = 0;
    uint32_t used_tex_mask = 0;
    for (uint32_t t = 0; t < 32; ++t) {
      if (!(ps->info.textureSlots & (1u << t))) continue;
      used_tex_mask |= 1u << t;
      auto fetch = regs.GetTextureFetch(t);
      id<MTLTexture> tex = GetTexture(fetch);
      const float scale = tex ? float(tex.width) / float(fetch.size_2d.width + 1) : 1.0f;
      tex_scales[t] = scale >= 1.5f ? std::round(scale) : 1.0f;
      textures[tex_count] = tex ? tex : dummy_tex_;
      samplers[tex_count] = GetSampler(fetch);
      tex_count++;
    }

    const bool in_gameplay_screen =
        rex_monitor::IsInGameplay() && (rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY);
    if (!in_gameplay_screen && ui_sniffer_ && rex::graphics::IsUiQuadSnifferActive()) {
      PrimitiveProcessor::ProcessingResult pr{};
      pr.guest_primitive_type = prim_type;
      pr.host_primitive_type = prim_type;
      pr.guest_draw_vertex_count = index_count;
      pr.host_draw_vertex_count = index_count;
      ui_sniffer_->OnDraw(
          vs, ps, pr, depth_ctrl, index_buffer_info != nullptr,
          index_buffer_info ? index_buffer_info->guest_base : 0,
          index_buffer_info ? index_buffer_info->format : xenos::IndexFormat::kInt16,
          index_buffer_info ? index_buffer_info->endianness : xenos::Endian::kNone,
          used_tex_mask);
    }

    auto cc = regs.Get<reg::RB_COLORCONTROL>();
    const uint32_t alpha_func = cc.alpha_test_enable ? uint32_t(cc.alpha_func) : 7u;
    id<MTLRenderPipelineState> pipeline =
        GetPipeline(vs, ps, colors, depth, blend, color_mask, alpha_func);
    if (!pipeline) return false;

    id<MTLRenderCommandEncoder> enc = BeginPass(colors, depth, sx + sw, sy + sh);
    [enc setRenderPipelineState:pipeline];
    [enc setViewport:{double(vp.xy_offset[0]), double(vp.xy_offset[1]), double(vp.xy_extent[0]),
                      double(vp.xy_extent[1]), double(vp.z_min), double(vp.z_max)}];
    [enc setScissorRect:{sx, sy, sw, sh}];
    [enc setCullMode:!polygonal ? MTLCullModeNone
                                : sc.cull_front ? MTLCullModeFront
                                                : sc.cull_back ? MTLCullModeBack : MTLCullModeNone];
    [enc setFrontFacingWinding:sc.face ? MTLWindingClockwise : MTLWindingCounterClockwise];
    if (depth) {
      const uint32_t rm = regs[XE_GPU_REG_RB_STENCILREFMASK];
      const uint32_t rmb = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
      [enc setDepthStencilState:GetDepthState(depth_ctrl, rm, rmb)];
      [enc setStencilFrontReferenceValue:rm & 0xFF
                      backReferenceValue:(depth_ctrl.backface_enable ? rmb : rm) & 0xFF];
    }
    auto rf = [&](uint32_t idx) { return rex::memory::Reinterpret<float>(regs[idx]); };
    [enc setBlendColorRed:rf(XE_GPU_REG_RB_BLEND_RED)
                    green:rf(XE_GPU_REG_RB_BLEND_GREEN)
                     blue:rf(XE_GPU_REG_RB_BLEND_BLUE)
                    alpha:rf(XE_GPU_REG_RB_BLEND_ALPHA)];

    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    XeDraw d{};
    for (int i = 0; i < 3; ++i) {
      d.ndcScale[i] = vp.ndc_scale[i];
      d.ndcOffset[i] = vp.ndc_offset[i];
    }
    d.indexed = index_buffer_info ? 1u : 0u;
    d.indexBase = index_buffer_info ? index_buffer_info->guest_base : 0u;
    d.indexFormat =
        (index_buffer_info && index_buffer_info->format == xenos::IndexFormat::kInt32) ? 1u : 0u;
    d.indexEndian = index_buffer_info ? uint32_t(index_buffer_info->endianness) : 0u;
    d.indexOffset = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
    d.flags = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 4u : 0u);
    d.primitive = uint32_t(prim_type);
    d.alphaFunc = alpha_func;
    d.alphaRef = rf(XE_GPU_REG_RB_ALPHA_REF);
    d.pixelScale = 1.0f;
    d.memSplitWord = (gpu_mem_split_byte_ == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (gpu_mem_split_byte_ >> 2);
    d.pad = 0;

    uint32_t vs_off = 0, vs_bool_off = 0, fetch_off = 0, ps_off = 0;
    id<MTLBuffer> vs_buf = UploadConstBlock(&regs[XE_GPU_REG_SHADER_CONSTANT_000_X], 256u * 16u,
                                            last_vs_const_, last_vs_buf_, last_vs_off_, vs_off);
    id<MTLBuffer> bool_buf =
        UploadConstBlock(&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 40u * 4u,
                         last_bool_const_, last_bool_buf_, last_bool_off_, vs_bool_off);
    id<MTLBuffer> fetch_buf =
        UploadConstBlock(&regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 96u * 2u * 4u,
                         last_fetch_const_, last_fetch_buf_, last_fetch_off_, fetch_off);
    id<MTLBuffer> ps_buf = UploadConstBlock(&regs[XE_GPU_REG_SHADER_CONSTANT_256_X], 256u * 16u,
                                            last_ps_const_, last_ps_buf_, last_ps_off_, ps_off);

    [enc setVertexBytes:&d length:sizeof(d) atIndex:0];
    if (vs_buf) {
      if (enc_vs_buf_ != vs_buf || enc_vs_off_ != vs_off) {
        [enc setVertexBuffer:vs_buf offset:vs_off atIndex:1];
        enc_vs_buf_ = vs_buf;
        enc_vs_off_ = vs_off;
      }
    } else {
      [enc setVertexBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_000_X] length:256 * 16 atIndex:1];
    }
    if (bool_buf) {
      if (enc_bool_buf_ != bool_buf || enc_bool_off_ != vs_bool_off) {
        [enc setVertexBuffer:bool_buf offset:vs_bool_off atIndex:2];
        [enc setFragmentBuffer:bool_buf offset:vs_bool_off atIndex:2];
        enc_bool_buf_ = bool_buf;
        enc_bool_off_ = vs_bool_off;
      }
    } else {
      [enc setVertexBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031] length:40 * 4 atIndex:2];
      [enc setFragmentBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031] length:40 * 4 atIndex:2];
    }
    if (fetch_buf) {
      if (enc_fetch_buf_ != fetch_buf || enc_fetch_off_ != fetch_off) {
        [enc setVertexBuffer:fetch_buf offset:fetch_off atIndex:3];
        enc_fetch_buf_ = fetch_buf;
        enc_fetch_off_ = fetch_off;
      }
    } else {
      [enc setVertexBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] length:96 * 2 * 4 atIndex:3];
    }
    [enc setFragmentBytes:&d length:sizeof(d) atIndex:0];
    if (ps_buf) {
      if (enc_ps_buf_ != ps_buf || enc_ps_off_ != ps_off) {
        [enc setFragmentBuffer:ps_buf offset:ps_off atIndex:1];
        enc_ps_buf_ = ps_buf;
        enc_ps_off_ = ps_off;
      }
    } else {
      [enc setFragmentBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_256_X] length:256 * 16 atIndex:1];
    }
    [enc setFragmentBytes:tex_scales length:sizeof(tex_scales) atIndex:3];
    for (uint32_t i = 0; i < tex_count; ++i) {
      [enc setFragmentTexture:textures[i] atIndex:i];
      [enc setFragmentSamplerState:samplers[i] atIndex:i];
    }
    ++frame_draws_;
    [enc drawPrimitives:mtl_prim vertexStart:0 vertexCount:count];
    return true;
  }

  bool IssueCopy() override {
    const auto& regs = *register_file_;
    auto copy = regs.Get<reg::RB_COPY_CONTROL>();
    if (copy.copy_command != xenos::CopyCommand::kRaw &&
        copy.copy_command != xenos::CopyCommand::kConvert) {
      return true;
    }

    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!ResolveRect(regs, x0, y0, x1, y1)) return true;

    const uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
    const uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];

    if (uint32_t(copy.copy_src_select) < 4) {
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[uint32_t(copy.copy_src_select)]);
      id<MTLTexture> target = GetTarget(info.color_base, pitch, uint32_t(info.color_format), false);
      x1 = std::min(x1, int32_t(target.width));
      y1 = std::min(y1, int32_t(target.height));
      if (x0 >= x1 || y0 >= y1) return true;

      const bool readable_rt = (target.pixelFormat == MTLPixelFormatRGBA8Unorm ||
                                target.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ||
                                target.pixelFormat == MTLPixelFormatRGB10A2Unorm ||
                                target.pixelFormat == MTLPixelFormatRGBA16Float);
      const bool rgba8 = (target.pixelFormat == MTLPixelFormatRGBA8Unorm ||
                          target.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB);
      auto dest = regs.Get<reg::RB_COPY_DEST_INFO>();
      auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
      ResolveParams params{uint32_t(x0),
                           uint32_t(y0),
                           uint32_t(x1 - x0),
                           uint32_t(y1 - y0),
                           rex::align(uint32_t(dest_pitch.copy_dest_pitch), 32u),
                           rex::align(uint32_t(dest_pitch.copy_dest_height), 32u),
                           dest_base,
                           uint32_t(dest.copy_dest_endian),
                           uint32_t(dest.copy_dest_swap),
                           regs[XE_GPU_REG_RB_COLOR_CLEAR],
                           1u,
                           gpu_mem_split_byte_};

      const bool copy_rgba8 =
          (dest.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 ||
           dest.copy_dest_format == xenos::ColorFormat::k_8_8_8_8_A ||
           dest.copy_dest_format == xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16 ||
           dest.copy_dest_format == xenos::ColorFormat::k_2_10_10_10 ||
           dest.copy_dest_format == xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16);
      if (dest_base && readable_rt && copy_rgba8 &&
          dest_pitch.copy_dest_pitch && dest_pitch.copy_dest_height) {
        id<MTLTexture> copy_tex =
            GetResolveTexture(dest_base, dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height);
        DispatchCompute(resolve_pso_, params, target, copy_tex);
        last_resolved_tex_ = copy_tex;
        // Only evict cached CPU textures overlapping the resolved physical EDRAM destination.
        const uint32_t r_start = dest_base & 0x1FFFFFFFu;
        const uint64_t r_bytes = uint64_t(rex::align(uint32_t(dest_pitch.copy_dest_pitch), 32u)) *
                                 rex::align(uint32_t(dest_pitch.copy_dest_height), 32u) * 4ull;
        const uint64_t r_end = uint64_t(r_start) + r_bytes;
        std::erase_if(textures_, [r_start, r_end](const auto& kv) {
          const TexEntry& e = kv.second;
          return e.phys_end == 0 ||
                 (uint64_t(e.phys_start) < r_end && uint64_t(e.phys_end) > r_start);
        });
      }
      if (copy.color_clear_enable) {
        if (target.pixelFormat == MTLPixelFormatRGBA8Unorm && x0 == 0 && y0 == 0 &&
            uint32_t(x1) == uint32_t(target.width) && uint32_t(y1) == uint32_t(target.height)) {
          EndPass();
          MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
          pass.renderTargetWidth = uint32_t(x1);
          pass.renderTargetHeight = uint32_t(y1);
          pass.colorAttachments[0].texture = target;
          pass.colorAttachments[0].loadAction = MTLLoadActionClear;
          pass.colorAttachments[0].storeAction = MTLStoreActionStore;
          const uint32_t c = params.clearValue;
          pass.colorAttachments[0].clearColor = MTLClearColorMake(
              (c & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
              ((c >> 16) & 0xFF) / 255.0, (c >> 24) / 255.0);
          [[Commands() renderCommandEncoderWithDescriptor:pass] endEncoding];
        } else if (rgba8) {
          DispatchCompute(clear_pso_, params, nil, target);
        }
      }
    }

    if (copy.depth_clear_enable) {
      auto info = regs.Get<reg::RB_DEPTH_INFO>();
      id<MTLTexture> depth = GetTarget(info.depth_base, pitch, uint32_t(info.depth_format), true);
      pending_depth_clears_[(__bridge void*)depth] = {regs[XE_GPU_REG_RB_DEPTH_CLEAR],
                                                       uint32_t(x1), uint32_t(y1)};
    }
    return true;
  }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t fb_width, uint32_t fb_height) override {
    @autoreleasepool {
      const bool in_gameplay_screen =
        rex_monitor::IsInGameplay() && (rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY);
    if (!in_gameplay_screen && ui_sniffer_ && rex::graphics::IsUiQuadSnifferActive()) {
        ui_sniffer_->OnFrameSwap();
      }
      auto fetch = register_file_->GetTextureFetch(0);
      id<MTLTexture> fb = GetTexture(fetch);
      float uv_x = (fetch.size_2d.width > 0)
                       ? (float(fb_width) / float(fetch.size_2d.width + 1))
                       : 1.0f;
      float uv_y = (fetch.size_2d.height > 0)
                       ? (float(fb_height) / float(fetch.size_2d.height + 1))
                       : 1.0f;
      if (!fb && frontbuffer_ptr) {
        auto it = resolved_.find(frontbuffer_ptr & 0x1FFFFFFFu);
        if (it != resolved_.end() && it->second.texture) {
          fb = it->second.texture;
          uv_x = float(fb_width) / float(std::max(1u, it->second.width));
          uv_y = float(fb_height) / float(std::max(1u, it->second.height));
        }
      }
      if (!fb && last_resolved_tex_) {
        fb = last_resolved_tex_;
        uv_x = float(fb_width) / float(std::max<uint32_t>(1u, uint32_t(fb.width)));
        uv_y = float(fb_height) / float(std::max<uint32_t>(1u, uint32_t(fb.height)));
      }
      if (!fb && pass_colors_[0]) {
        fb = pass_colors_[0];
        uv_x = float(fb_width) / float(std::max<uint32_t>(1u, uint32_t(fb.width)));
        uv_y = float(fb_height) / float(std::max<uint32_t>(1u, uint32_t(fb.height)));
      }
      EndPass();

      const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
      for (int i = 0; i < 2; ++i) {
        if (watcher_done_[i].exchange(false, std::memory_order_acquire)) {
          const uint8_t* px = static_cast<const uint8_t*>([watcher_buf_[i] contents]);
          const uint64_t cap_ms = watcher_cap_ms_[i];
          rex::ui::Presenter::UpdateScreenWatcherFrame(px, 320, 180, cap_ms);
          if (watcher_cap_fast_[i]) {
            float angle = 0.0f;
            uint32_t dots = 0;
            if (rex_monitor::MeasureVisualTurretAngleFromPixels(px, 320, 180, angle, &dots) &&
                dots >= 4) {
              rex::ui::Presenter::UpdateVisualTurretSample(angle, dots, cap_ms);
            }
          }
          watcher_busy_[i].store(false, std::memory_order_release);
        }
      }

      const bool fast_cadence = rex::ui::Presenter::IsScreenWatcherFastCadence();
      const uint64_t min_interval = 100ull;  // Cap iOS ScreenWatcher readback at <= 10 Hz.

      // Skip ScreenWatcher GPU readbacks completely while actively playing gameplay or backgrounded.
      if (fb && watcher_pso_ && watcher_buf_[0] && watcher_buf_[1] && !in_gameplay_screen &&
          !rex::IsAppBackgroundPaused() && (now_ms - last_watcher_ms_ >= min_interval)) {
        const int slot = watcher_slot_ & 1;
        bool expected = false;
        if (watcher_busy_[slot].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
          last_watcher_ms_ = now_ms;
          watcher_slot_++;
          float uv[2] = {uv_x, uv_y};
          MTLRenderPassDescriptor* swp = [MTLRenderPassDescriptor renderPassDescriptor];
          swp.colorAttachments[0].texture = watcher_tex_[slot];
          swp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
          swp.colorAttachments[0].storeAction = MTLStoreActionStore;
          id<MTLCommandBuffer> wcmd = Commands();
          if (wcmd) {
            id<MTLRenderCommandEncoder> enc = [wcmd renderCommandEncoderWithDescriptor:swp];
            [enc setRenderPipelineState:watcher_pso_];
            [enc setFragmentBytes:uv length:sizeof(uv) atIndex:0];
            [enc setFragmentTexture:fb atIndex:0];
            [enc setFragmentTexture:GammaRampTexture() atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];

            id<MTLBlitCommandEncoder> blit = [wcmd blitCommandEncoder];
            [blit copyFromTexture:watcher_tex_[slot]
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(320, 180, 1)
                             toBuffer:watcher_buf_[slot]
                    destinationOffset:0
               destinationBytesPerRow:320u * 4u
             destinationBytesPerImage:320u * 180u * 4u];
            [blit endEncoding];

            watcher_cap_ms_[slot] = now_ms;
            watcher_cap_fast_[slot] = fast_cadence;
            std::atomic<bool>* done_flag = &watcher_done_[slot];
            std::shared_ptr<CmdHandlerTracker> tracker = handler_tracker_;
            if (tracker) tracker->Schedule();
            [wcmd addCompletedHandler:^(id<MTLCommandBuffer>) {
              if (tracker) {
                std::lock_guard<std::mutex> lock(tracker->mutex);
                if (tracker->alive) {
                  done_flag->store(true, std::memory_order_release);
                }
                if (--tracker->pending <= 0) {
                  tracker->pending = 0;
                  tracker->cv.notify_all();
                }
              } else {
                done_flag->store(true, std::memory_order_release);
              }
            }];
          } else {
            watcher_busy_[slot].store(false, std::memory_order_release);
          }
        }
      }

      id<MTLTexture> gamma_tex = GammaRampTexture();
      const bool bg_paused = rex::IsAppBackgroundPaused();
      id<MTLCommandBuffer> swap_cmd = Commands();
      const bool scheduled_ring_sig = (ring_sem_ != nil && swap_cmd != nil);
      if (scheduled_ring_sig) {
        dispatch_semaphore_t sem = ring_sem_;
        std::shared_ptr<CmdHandlerTracker> tracker = handler_tracker_;
        if (tracker) tracker->Schedule();
        [swap_cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          dispatch_semaphore_signal(sem);
          if (tracker) tracker->Complete();
        }];
      }
      Submit(bg_paused || (ring_sem_ && !scheduled_ring_sig));
      ++swap_index_;
      if (ring_sem_) {
        ring_slot_ = int(swap_index_ % kConstRingSlots);
        if (scheduled_ring_sig) {
          dispatch_semaphore_wait(ring_sem_, DISPATCH_TIME_FOREVER);
        }
        frame_overflow_bufs_[ring_slot_].clear();
        frame_overflow_textures_[ring_slot_].clear();
        ring_offset_ = 0;
      }
      ResetConstCache();
      g_perf_draws.store(frame_draws_, std::memory_order_relaxed);
      g_perf_tex_uploads.store(frame_tex_uploads_, std::memory_order_relaxed);
      g_perf_buf_allocs.store(g_cur_buf_allocs.exchange(0, std::memory_order_relaxed),
                              std::memory_order_relaxed);
      frame_draws_ = 0;
      frame_tex_uploads_ = 0;

      if (fb && !bg_paused) {
        g_guest_frame_tex = fb;
        g_guest_frontbuffer_mtl = fb;
        if (provider_ && provider_->latest_presenter()) {
          provider_->latest_presenter()->PublishGuestTexture(
              fb, fb_width ? fb_width : 1280, fb_height ? fb_height : 720, uv_x, uv_y, gamma_tex);
        }
      }

      generation_++;
      if (generation_ % 60 == 0) {
        std::erase_if(textures_, [this](const auto& kv) {
          return generation_ - kv.second.checked_gen > 600;
        });
        std::erase_if(resolved_, [this](const auto& kv) {
          return generation_ - kv.second.resolved_gen > 600;
        });
      }
    }
  }

 private:
  static constexpr int kConstRingSlots = 3;
  static constexpr int kTexRingSlots = 4;
  static constexpr uint32_t kConstRingBytes = 4u * 1024u * 1024u;

  struct TexEntry {
    id<MTLTexture> textures[kTexRingSlots]{};
    uint64_t slot_used_gen[kTexRingSlots]{};
    uint64_t data_hash = 0;
    uint64_t sample_hash = 0;
    uint64_t checked_gen = 0;
    uint32_t phys_start = 0;
    uint32_t phys_end = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t swizzle = 0;
    uint32_t mutation_count = 0;
    MTLPixelFormat pf = MTLPixelFormatInvalid;
    int active_slot = 0;
    bool is_dynamic = false;
  };
  struct ResolvedTarget {
    id<MTLTexture> texture = nil;
    uint32_t width = 0, height = 0, bytes = 0;
    uint64_t resolved_gen = 0;
    uint64_t resolves = 0;
    uint64_t pending_seq = 0;
    std::unordered_map<uint32_t, id<MTLTexture>> views;
    struct Window {
      id<MTLTexture> texture = nil;
      uint64_t copied = ~0ull;
      std::unordered_map<uint32_t, id<MTLTexture>> views;
    };
    std::unordered_map<uint32_t, Window> windows;
  };
  struct DepthClear {
    uint32_t value = 0, right = 0, bottom = 0;
  };

  id<MTLCommandBuffer> Commands() {
    if (!commands_) commands_ = [queue_ commandBuffer];
    return commands_;
  }
  void EndPass() {
    if (encoder_) {
      [encoder_ endEncoding];
      encoder_ = nil;
    }
    enc_vs_buf_ = enc_ps_buf_ = enc_bool_buf_ = enc_fetch_buf_ = nil;
    enc_vs_off_ = enc_ps_off_ = enc_bool_off_ = enc_fetch_off_ = UINT32_MAX;
  }
  void Submit(bool wait) {
    EndPass();
    if (commands_) {
      id<MTLCommandBuffer> cmd = commands_;
      commands_ = nil;
      last_submitted_cmd_ = wait ? nil : cmd;
      const uint64_t seq = next_submit_seq_++;
      std::atomic<uint64_t>* done_seq = &completed_submit_seq_;
      std::shared_ptr<CmdHandlerTracker> tracker = handler_tracker_;
      if (tracker) tracker->Schedule();
      [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        if (tracker) {
          std::lock_guard<std::mutex> lock(tracker->mutex);
          if (tracker->alive) {
            uint64_t prev = done_seq->load(std::memory_order_relaxed);
            while (prev < seq && !done_seq->compare_exchange_weak(prev, seq, std::memory_order_release)) {}
          }
          if (--tracker->pending <= 0) {
            tracker->pending = 0;
            tracker->cv.notify_all();
          }
        } else {
          uint64_t prev = done_seq->load(std::memory_order_relaxed);
          while (prev < seq && !done_seq->compare_exchange_weak(prev, seq, std::memory_order_release)) {}
        }
      }];
      const uint64_t cmd_id = PrepareTrackedMetalCmd(cmd);
      [cmd commit];
      MarkCommittedMetalCmd(cmd, cmd_id);
      if (wait) {
        [cmd waitUntilCompleted];
        last_submitted_cmd_ = nil;
      }
    } else if (wait && last_submitted_cmd_) {
      const MTLCommandBufferStatus st = last_submitted_cmd_.status;
      if (st == MTLCommandBufferStatusCommitted || st == MTLCommandBufferStatusScheduled) {
        [last_submitted_cmd_ waitUntilCompleted];
      }
      last_submitted_cmd_ = nil;
    }
  }

  static MTLTextureSwizzle MapSwizzle(uint32_t code) {
    switch (code) {
      case 0: return MTLTextureSwizzleRed;
      case 1: return MTLTextureSwizzleGreen;
      case 2: return MTLTextureSwizzleBlue;
      case 3: return MTLTextureSwizzleAlpha;
      case 4: return MTLTextureSwizzleZero;
      default: return MTLTextureSwizzleOne;
    }
  }
  static MTLTextureSwizzleChannels Swizzles(uint32_t sw) {
    return MTLTextureSwizzleChannelsMake(MapSwizzle(sw & 7), MapSwizzle((sw >> 3) & 7),
                                         MapSwizzle((sw >> 6) & 7), MapSwizzle((sw >> 9) & 7));
  }

  static MTLPixelFormat ColorFormat(xenos::ColorRenderTargetFormat fmt) {
    switch (fmt) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8: return MTLPixelFormatRGBA8Unorm;
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return MTLPixelFormatRGBA8Unorm_sRGB;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: return MTLPixelFormatRGB10A2Unorm;
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return MTLPixelFormatRG16Float;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT: return MTLPixelFormatR32Float;
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return MTLPixelFormatRG32Float;
      default: return MTLPixelFormatRGBA16Float;
    }
  }

  id<MTLTexture> GetTarget(uint32_t base_tiles, uint32_t pitch, uint32_t format, bool depth) {
    const uint64_t key = uint64_t(base_tiles) | (uint64_t(pitch) << 12) |
                         (uint64_t(format) << 26) | (uint64_t(depth) << 40);
    __strong id<MTLTexture>& tex = targets_[key];
    if (!tex) {
      uint32_t tpr = std::max(1u, (pitch + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples);
      uint32_t rows = (xenos::kEdramTileCount - std::min(base_tiles, xenos::kEdramTileCount - 1)) / tpr;
      uint32_t h = std::clamp(rows * xenos::kEdramTileHeightSamples, 1u, 2048u);
      uint32_t w = std::max(pitch, 1u);
      MTLPixelFormat pf = depth ? MTLPixelFormatDepth32Float_Stencil8
                                : ColorFormat(static_cast<xenos::ColorRenderTargetFormat>(format));
      MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
      d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (depth ? 0 : MTLTextureUsageShaderWrite);
      d.storageMode = MTLStorageModePrivate;
      tex = [device_ newTextureWithDescriptor:d];
    }
    return tex;
  }

  id<MTLTexture> GetResolveTexture(uint32_t base, uint32_t width, uint32_t height) {
    ResolvedTarget& r = resolved_[base];
    if (!r.texture || r.width != width || r.height != height) {
      MTLTextureDescriptor* d =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                             width:width
                                                            height:height
                                                         mipmapped:NO];
      d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
      d.storageMode = MTLStorageModeShared;
      r.texture = [device_ newTextureWithDescriptor:d];
      r.views.clear();
      r.windows.clear();
      r.width = width;
      r.height = height;
      r.bytes = rex::align(width, 32u) * rex::align(height, 32u) * 4;
    }
    r.resolved_gen = generation_;
    r.resolves++;
    r.pending_seq = next_submit_seq_;
    return r.texture;
  }

  void DispatchCompute(id<MTLComputePipelineState> pso, const ResolveParams& params,
                       id<MTLTexture> src, id<MTLTexture> dst) {
    EndPass();
    id<MTLComputeCommandEncoder> enc = [Commands() computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setTexture:src atIndex:0];
    [enc setTexture:dst atIndex:1];
    [enc setBuffer:gpu_mem_ offset:0 atIndex:0];
    [enc setBuffer:(gpu_mem_hi_ ? gpu_mem_hi_ : gpu_mem_) offset:0 atIndex:2];
    [enc setBytes:&params length:sizeof(params) atIndex:1];
    [enc dispatchThreads:MTLSizeMake(params.width, params.height, 1)
        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
  }

  bool ResolveRect(const RegisterFile& regs, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) {
    auto fetch = regs.GetVertexFetch(0);
    if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6) return false;
    const uint32_t* raw = memory_->TranslatePhysical<const uint32_t*>(fetch.address * 4);
    float half = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
    int32_t v[6];
    for (int i = 0; i < 6; ++i) {
      v[i] = rex::ui::FloatToD3D11Fixed16p8(
          rex::memory::Reinterpret<float>(SwapWord(raw[i], fetch.endian)) + half);
    }
    x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8;
    y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
    x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8;
    y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      auto off = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
      x0 += off.window_x_offset;
      x1 += off.window_x_offset;
      y0 += off.window_y_offset;
      y1 += off.window_y_offset;
    }
    draw_util::Scissor sc{};
    draw_util::GetScissor(regs, sc, false);
    int32_t right = int32_t(sc.offset[0] + sc.extent[0]);
    int32_t bottom = int32_t(sc.offset[1] + sc.extent[1]);
    x0 = std::clamp(x0, int32_t(sc.offset[0]), right);
    x1 = std::clamp(x1, int32_t(sc.offset[0]), right);
    y0 = std::clamp(y0, int32_t(sc.offset[1]), bottom);
    y1 = std::clamp(y1, int32_t(sc.offset[1]), bottom);
    x0 &= ~7;
    y0 &= ~7;
    x1 = rex::align(x1, 8);
    y1 = rex::align(y1, 8);
    int32_t pitch = int32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch & ~7u);
    x0 = std::min(x0, pitch);
    x1 = std::min(x1, pitch);
    return x0 < x1 && y0 < y1;
  }

  id<MTLTexture> GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
    // Check GPU resolve cache first.
    const auto* fmt_info = FormatInfo::Get(uint32_t(fetch.format));
    if (!fmt_info) return nil;
    const uint32_t fbase = fetch.base_address << 12;
    const uint32_t fbytes = rex::align(uint32_t(fetch.size_2d.width) + 1, 32u) *
                            rex::align(uint32_t(fetch.size_2d.height) + 1, 32u) *
                            fmt_info->bits_per_pixel / 8;
    for (auto& [addr, r] : resolved_) {
      if (fbase >= addr + r.bytes || addr >= fbase + fbytes) continue;
      const uint32_t w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1;
      const bool match = addr == fbase && fetch.tiled &&
                         fetch.dimension == xenos::DataDimension::k2DOrStacked &&
                         (fetch.format == xenos::TextureFormat::k_8_8_8_8 ||
                          fetch.format == xenos::TextureFormat::k_8_8_8_8_A ||
                          fetch.format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16 ||
                          fetch.format == xenos::TextureFormat::k_2_10_10_10 ||
                          fetch.format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16) &&
                         fetch.pitch * 32 == rex::align(r.width, 32u) && w <= r.width &&
                         h <= r.height;
      if (!match) {
        if (r.pending_seq > completed_submit_seq_.load(std::memory_order_acquire)) {
          Submit(true);
        }
        break;
      }
      id<MTLTexture> src = r.texture;
      auto* views = &r.views;
      if (w != r.width || h != r.height) {
        ResolvedTarget::Window& win = r.windows[w | (h << 16)];
        if (!win.texture) {
          MTLTextureDescriptor* td =
              [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                 width:w
                                                                height:h
                                                             mipmapped:NO];
          td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
          td.storageMode = MTLStorageModePrivate;
          win.texture = [device_ newTextureWithDescriptor:td];
        }
        if (win.copied != r.resolves) {
          EndPass();
          id<MTLBlitCommandEncoder> blit = [Commands() blitCommandEncoder];
          [blit copyFromTexture:r.texture
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:win.texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
          [blit endEncoding];
          win.copied = r.resolves;
        }
        src = win.texture;
        views = &win.views;
      }
      const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma;
      __strong id<MTLTexture>& view = (*views)[fetch.swizzle | (gamma ? 0x10000 : 0)];
      if (!view) {
        view = [src newTextureViewWithPixelFormat:gamma ? MTLPixelFormatRGBA8Unorm_sRGB
                                                       : MTLPixelFormatRGBA8Unorm
                                       textureType:MTLTextureType2D
                                            levels:NSMakeRange(0, 1)
                                            slices:NSMakeRange(0, 1)
                                           swizzle:Swizzles(fetch.swizzle)];
      }
      return view;
    }

    if (fetch.type != xenos::FetchConstantType::kTexture ||
        fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked) {
      return nil;
    }

    const uint64_t key = XXH3_64bits(&fetch, sizeof(fetch));
    TexEntry& entry = textures_[key];
    id<MTLTexture> active_tex = entry.textures[entry.active_slot];
    // Same-frame fast hit for all textures (IssueCopy range eviction handles mid-frame updates).
    if (active_tex && entry.checked_gen == generation_) {
      entry.slot_used_gen[entry.active_slot] = generation_;
      return active_tex;
    }

    uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
    texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                   &mip_min, &mip_max);
    if (!base_page) return nil;
    const uint32_t width = w1 + 1, height = h1 + 1;
    const bool is_video_plane =
        (fetch.format == xenos::TextureFormat::k_8) && (width >= 320u && height >= 180u);

    const auto* info = FormatInfo::Get(uint32_t(fetch.format));
    if (!info) return nil;
    const uint32_t bpb = info->bytes_per_block();
    if (!bpb) return nil;
    const uint32_t bpb_log2 = rex::log2_floor(bpb);
    const uint32_t blocks_x = (width + info->block_width - 1) / info->block_width;
    const uint32_t blocks_y = (height + info->block_height - 1) / info->block_height;

    texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1, fetch.tiled,
        fetch.format, fetch.packed_mips, true, 0);
    uint32_t ox = 0, oy = 0, oz = 0;
    if (fetch.packed_mips && layout.packed_level == 0) {
      texture_util::GetPackedMipOffset(width, height, 1, fetch.format, 0, ox, oy, oz);
    }

    const uint32_t phys_off = (base_page << 12) & 0x1FFFFFFFu;
    const uint32_t extent = layout.base.level_data_extent_bytes;
    if (extent == 0 || uint64_t(phys_off) + extent > 0x1FFF0000ULL) return nil;
    const uint8_t* base = memory_->TranslatePhysical<const uint8_t*>(base_page << 12);
    const bool full_hash_every_frame = entry.is_dynamic || is_video_plane;
    const uint64_t sample_hash = full_hash_every_frame ? 0ull : SampleTexFingerprint(base, extent);
    if (!full_hash_every_frame && active_tex && entry.phys_start == phys_off &&
        entry.phys_end == phys_off + extent && entry.width == width && entry.height == height &&
        extent > 16384u && sample_hash == entry.sample_hash && ((key + generation_) & 63u) != 0u) {
      entry.checked_gen = generation_;
      entry.slot_used_gen[entry.active_slot] = generation_;
      return active_tex;
    }

    const uint64_t new_hash = XXH3_64bits(base, extent);
    entry.checked_gen = generation_;
    entry.phys_start = phys_off;
    entry.phys_end = phys_off + extent;
    entry.sample_hash = sample_hash;
    if (new_hash == entry.data_hash && active_tex) {
      entry.slot_used_gen[entry.active_slot] = generation_;
      return active_tex;
    }
    const bool had_existing = (active_tex != nil);
    if (had_existing) {
      ++entry.mutation_count;
      if (entry.mutation_count >= 2u) {
        entry.is_dynamic = true;
      }
    }
    entry.data_hash = new_hash;

    uint32_t row_bytes = blocks_x * bpb;
    tex_upload_blocks_.resize(size_t(row_bytes) * blocks_y);
    const uint32_t pitch_blocks = layout.base.row_pitch_bytes >> bpb_log2;
    for (uint32_t y = 0; y < blocks_y; ++y) {
      for (uint32_t x = 0; x < blocks_x; ++x) {
        const uint32_t sx = x + ox, sy = y + oy;
        const uint32_t off =
            fetch.tiled ? uint32_t(texture_util::GetTiledOffset2D(int32_t(sx), int32_t(sy),
                                                                  pitch_blocks, bpb_log2))
                        : sy * layout.base.row_pitch_bytes + (sx << bpb_log2);
        if (off + bpb > extent) continue;
        uint8_t* out = &tex_upload_blocks_[size_t(y) * row_bytes + size_t(x) * bpb];
        if (bpb >= 4) {
          for (uint32_t i = 0; i < bpb; i += 4) {
            uint32_t w;
            std::memcpy(&w, base + off + i, 4);
            w = SwapWord(w, fetch.endianness);
            std::memcpy(out + i, &w, 4);
          }
        } else {
          uint32_t w;
          std::memcpy(&w, base + (off & ~3u), 4);
          w = SwapWord(w, fetch.endianness) >> ((off & 3u) * 8u);
          std::memcpy(out, &w, bpb);
        }
      }
    }

    const bool gamma = (fetch.sign_x == xenos::TextureSign::kGamma);
    bool bc_supported = false;
    if (@available(iOS 16.4, *)) {
      bc_supported = [device_ supportsBCTextureCompression];
    }

    MTLPixelFormat pf = MTLPixelFormatInvalid;
    int expand16 = 0;  // 1=565, 2=1555, 3=4444
    int decode_dxt = 0;
    switch (fetch.format) {
      case xenos::TextureFormat::k_8: pf = MTLPixelFormatR8Unorm; break;
      case xenos::TextureFormat::k_8_8: pf = MTLPixelFormatRG8Unorm; break;
      case xenos::TextureFormat::k_8_8_8_8:
      case xenos::TextureFormat::k_8_8_8_8_A:
        pf = gamma ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
        break;
      case xenos::TextureFormat::k_2_10_10_10: pf = MTLPixelFormatRGB10A2Unorm; break;
      case xenos::TextureFormat::k_16_FLOAT: pf = MTLPixelFormatR16Float; break;
      case xenos::TextureFormat::k_16_16_FLOAT: pf = MTLPixelFormatRG16Float; break;
      case xenos::TextureFormat::k_16_16_16_16_FLOAT: pf = MTLPixelFormatRGBA16Float; break;
      case xenos::TextureFormat::k_32_FLOAT: pf = MTLPixelFormatR32Float; break;
      case xenos::TextureFormat::k_32_32_FLOAT: pf = MTLPixelFormatRG32Float; break;
      case xenos::TextureFormat::k_32_32_32_32_FLOAT: pf = MTLPixelFormatRGBA32Float; break;
      case xenos::TextureFormat::k_5_6_5:
        pf = MTLPixelFormatRGBA8Unorm;
        expand16 = 1;
        break;
      case xenos::TextureFormat::k_1_5_5_5:
        pf = MTLPixelFormatRGBA8Unorm;
        expand16 = 2;
        break;
      case xenos::TextureFormat::k_4_4_4_4:
        pf = MTLPixelFormatRGBA8Unorm;
        expand16 = 3;
        break;
      case xenos::TextureFormat::k_DXT1:
        if (bc_supported) {
          if (@available(iOS 16.4, *)) pf = gamma ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA;
        } else {
          pf = gamma ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
          decode_dxt = 1;
        }
        break;
      case xenos::TextureFormat::k_DXT2_3:
        if (bc_supported) {
          if (@available(iOS 16.4, *)) pf = gamma ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA;
        } else {
          pf = gamma ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
          decode_dxt = 3;
        }
        break;
      case xenos::TextureFormat::k_DXT4_5:
        if (bc_supported) {
          if (@available(iOS 16.4, *)) pf = gamma ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA;
        } else {
          pf = gamma ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
          decode_dxt = 5;
        }
        break;
      default: return nil;
    }

    const uint8_t* upload_ptr = tex_upload_blocks_.data();
    if (expand16 != 0) {
      tex_upload_rgba_.resize(size_t(width) * height * 4);
      for (size_t i = 0; i < size_t(width) * height; ++i) {
        uint16_t v = 0;
        std::memcpy(&v, &tex_upload_blocks_[i * 2], 2);
        uint8_t* o = &tex_upload_rgba_[i * 4];
        if (expand16 == 1) {
          o[0] = ExpandBits(v & 31, 5); o[1] = ExpandBits((v >> 5) & 63, 6);
          o[2] = ExpandBits(v >> 11, 5); o[3] = 255;
        } else if (expand16 == 2) {
          o[0] = ExpandBits(v & 31, 5); o[1] = ExpandBits((v >> 5) & 31, 5);
          o[2] = ExpandBits((v >> 10) & 31, 5); o[3] = (v >> 15) ? 255 : 0;
        } else {
          for (int k = 0; k < 4; ++k) o[k] = ExpandBits((v >> (k * 4)) & 15, 4);
        }
      }
      upload_ptr = tex_upload_rgba_.data();
      row_bytes = width * 4;
    } else if (decode_dxt != 0) {
      const uint32_t dec_w = std::max(4u, blocks_x * 4);
      const uint32_t dec_h = std::max(4u, blocks_y * 4);
      const uint32_t dec_stride = dec_w * 4;
      tex_upload_rgba_.resize(size_t(dec_stride) * dec_h);
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          DecodeDxtBlockToRgba(decode_dxt, &tex_upload_blocks_[(size_t(by) * blocks_x + bx) * bpb],
                               &tex_upload_rgba_[(size_t(by) * 4 * dec_w + bx * 4) * 4], dec_stride);
        }
      }
      if (dec_w != width) {
        tex_upload_blocks_.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
          std::memcpy(&tex_upload_blocks_[size_t(y) * width * 4],
                      &tex_upload_rgba_[size_t(y) * dec_stride], width * 4);
        }
        upload_ptr = tex_upload_blocks_.data();
      } else {
        upload_ptr = tex_upload_rgba_.data();
      }
      row_bytes = width * 4;
    }

    int target_slot = 0;
    if (entry.is_dynamic || is_video_plane || had_existing) {
      const int preferred = int(swap_index_ % kTexRingSlots);
      target_slot = -1;
      for (int k = 0; k < kTexRingSlots; ++k) {
        const int cand = (preferred + k) % kTexRingSlots;
        if (entry.slot_used_gen[cand] == 0 ||
            (generation_ - entry.slot_used_gen[cand]) >= uint64_t(kConstRingSlots)) {
          target_slot = cand;
          break;
        }
      }
      if (target_slot < 0) {
        // All 4 ring slots were used within the last 2 frames; retire old texture without stalling.
        target_slot = preferred;
        if (entry.textures[target_slot]) {
          frame_overflow_textures_[ring_slot_].push_back(entry.textures[target_slot]);
          entry.textures[target_slot] = nil;
        }
      }
    }

    __strong id<MTLTexture>& dst_tex = entry.textures[target_slot];
    const bool tex_matches = dst_tex != nil &&
                             entry.pf == pf &&
                             entry.width == width &&
                             entry.height == height &&
                             entry.swizzle == fetch.swizzle &&
                             dst_tex.pixelFormat == pf &&
                             dst_tex.width == width &&
                             dst_tex.height == height &&
                             dst_tex.mipmapLevelCount == 1 &&
                             dst_tex.textureType == MTLTextureType2D &&
                             dst_tex.storageMode == MTLStorageModeShared;
    if (!tex_matches) {
      if (pf == MTLPixelFormatInvalid) return nil;
      if (entry.pf != pf || entry.width != width || entry.height != height ||
          entry.swizzle != fetch.swizzle) {
        for (int k = 0; k < kTexRingSlots; ++k) {
          if (entry.textures[k]) {
            frame_overflow_textures_[ring_slot_].push_back(entry.textures[k]);
            entry.textures[k] = nil;
          }
          entry.slot_used_gen[k] = 0;
        }
      }
      MTLTextureDescriptor* desc =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:width height:height mipmapped:NO];
      desc.usage = MTLTextureUsageShaderRead;
      desc.storageMode = MTLStorageModeShared;
      desc.swizzle = Swizzles(fetch.swizzle);
      dst_tex = [device_ newTextureWithDescriptor:desc];
      entry.pf = pf;
      entry.width = width;
      entry.height = height;
      entry.swizzle = fetch.swizzle;
    }
    if (!dst_tex || !upload_ptr || row_bytes == 0) return nil;
    ++frame_tex_uploads_;
    [dst_tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:upload_ptr
               bytesPerRow:row_bytes];
    entry.active_slot = target_slot;
    entry.slot_used_gen[target_slot] = generation_;
    return dst_tex;
  }

  id<MTLSamplerState> GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
    const uint64_t key = uint64_t(fetch.clamp_x) | (uint64_t(fetch.clamp_y) << 3) |
                         (uint64_t(fetch.clamp_z) << 6) | (uint64_t(fetch.mag_filter) << 9) |
                         (uint64_t(fetch.min_filter) << 11) | (uint64_t(fetch.border_color) << 13);
    __strong id<MTLSamplerState>& samp = samplers_[key];
    if (!samp) {
      auto addr = [](xenos::ClampMode m) {
        switch (m) {
          case xenos::ClampMode::kRepeat: return MTLSamplerAddressModeRepeat;
          case xenos::ClampMode::kMirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
          default: return MTLSamplerAddressModeClampToEdge;  // iOS universal clamp mode
        }
      };
      MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
      sd.sAddressMode = addr(fetch.clamp_x);
      sd.tAddressMode = addr(fetch.clamp_y);
      sd.rAddressMode = addr(fetch.clamp_z);
      sd.magFilter = (fetch.mag_filter == xenos::TextureFilter::kPoint) ? MTLSamplerMinMagFilterNearest
                                                                        : MTLSamplerMinMagFilterLinear;
      sd.minFilter = (fetch.min_filter == xenos::TextureFilter::kPoint) ? MTLSamplerMinMagFilterNearest
                                                                        : MTLSamplerMinMagFilterLinear;
      samp = [device_ newSamplerStateWithDescriptor:sd];
    }
    return samp;
  }

  id<MTLRenderCommandEncoder> BeginPass(id<MTLTexture> const colors[4], id<MTLTexture> depth,
                                        uint32_t right, uint32_t bottom) {
    bool same = encoder_ && depth == pass_depth_ && right <= pass_w_ && bottom <= pass_h_;
    for (int i = 0; i < 4 && same; ++i) same = (colors[i] == pass_colors_[i]);
    if (same && !(depth && pending_depth_clears_.count((__bridge void*)depth))) {
      return encoder_;
    }
    EndPass();

    uint32_t max_w = UINT32_MAX, max_h = UINT32_MAX;
    auto inc = [&](id<MTLTexture> tex) {
      auto [w, h] = drawn_extents_[(__bridge void*)tex];
      right = std::max(right, w);
      bottom = std::max(bottom, h);
      max_w = std::min(max_w, uint32_t(tex.width));
      max_h = std::min(max_h, uint32_t(tex.height));
    };
    for (int i = 0; i < 4; ++i) if (colors[i]) inc(colors[i]);
    if (depth) {
      inc(depth);
      if (auto it = pending_depth_clears_.find((__bridge void*)depth); it != pending_depth_clears_.end()) {
        right = std::max(right, it->second.right);
        bottom = std::max(bottom, it->second.bottom);
      }
    }
    right = std::min(right, max_w);
    bottom = std::min(bottom, max_h);
    for (int i = 0; i < 4; ++i) if (colors[i]) drawn_extents_[(__bridge void*)colors[i]] = {right, bottom};
    if (depth) drawn_extents_[(__bridge void*)depth] = {right, bottom};
    pass_w_ = right;
    pass_h_ = bottom;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.renderTargetWidth = right;
    pass.renderTargetHeight = bottom;
    for (int i = 0; i < 4; ++i) {
      pass_colors_[i] = colors[i];
      if (!colors[i]) continue;
      pass.colorAttachments[i].texture = colors[i];
      pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    pass_depth_ = depth;
    if (depth) {
      auto it = pending_depth_clears_.find((__bridge void*)depth);
      const bool clr = (it != pending_depth_clears_.end());
      const uint32_t val = clr ? it->second.value : 0;
      if (clr) pending_depth_clears_.erase(it);
      pass.depthAttachment.texture = depth;
      pass.depthAttachment.loadAction = clr ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = double(val >> 8) / double(0xFFFFFF);
      pass.stencilAttachment.texture = depth;
      pass.stencilAttachment.loadAction = clr ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.stencilAttachment.storeAction = MTLStoreActionStore;
      pass.stencilAttachment.clearStencil = val & 0xFF;
    }
    encoder_ = [Commands() renderCommandEncoderWithDescriptor:pass];
    [encoder_ setVertexBuffer:gpu_mem_ offset:0 atIndex:4];
    [encoder_ setVertexBuffer:(gpu_mem_hi_ ? gpu_mem_hi_ : gpu_mem_) offset:0 atIndex:5];
    ResetConstCache();
    return encoder_;
  }

  static MTLBlendFactor MapBlendFactor(xenos::BlendFactor f) {
    switch (f) {
      case xenos::BlendFactor::kZero: return MTLBlendFactorZero;
      case xenos::BlendFactor::kOne: return MTLBlendFactorOne;
      case xenos::BlendFactor::kSrcColor: return MTLBlendFactorSourceColor;
      case xenos::BlendFactor::kOneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
      case xenos::BlendFactor::kSrcAlpha: return MTLBlendFactorSourceAlpha;
      case xenos::BlendFactor::kOneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
      case xenos::BlendFactor::kDstColor: return MTLBlendFactorDestinationColor;
      case xenos::BlendFactor::kOneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
      case xenos::BlendFactor::kDstAlpha: return MTLBlendFactorDestinationAlpha;
      case xenos::BlendFactor::kOneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
      case xenos::BlendFactor::kConstantColor: return MTLBlendFactorBlendColor;
      case xenos::BlendFactor::kOneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
      case xenos::BlendFactor::kConstantAlpha: return MTLBlendFactorBlendAlpha;
      case xenos::BlendFactor::kOneMinusConstantAlpha: return MTLBlendFactorOneMinusBlendAlpha;
      case xenos::BlendFactor::kSrcAlphaSaturate: return MTLBlendFactorSourceAlphaSaturated;
      default: return MTLBlendFactorZero;
    }
  }
  static MTLBlendOperation MapBlendOp(xenos::BlendOp o) {
    switch (o) {
      case xenos::BlendOp::kSubtract: return MTLBlendOperationSubtract;
      case xenos::BlendOp::kMin: return MTLBlendOperationMin;
      case xenos::BlendOp::kMax: return MTLBlendOperationMax;
      case xenos::BlendOp::kRevSubtract: return MTLBlendOperationReverseSubtract;
      default: return MTLBlendOperationAdd;
    }
  }

  id<MTLRenderPipelineState> GetPipeline(MetalShader* vs, MetalShader* ps,
                                         id<MTLTexture> const colors[4], id<MTLTexture> depth,
                                         const uint32_t blend[4], uint32_t color_mask,
                                         uint32_t alpha_func) {
    struct {
      void* vs; void* ps;
      uint32_t formats[5];
      uint32_t blend[4];
      uint32_t mask, alpha;
    } key{vs, ps, {}, {}, color_mask, alpha_func};
    for (int i = 0; i < 4; ++i) {
      key.formats[i] = colors[i] ? uint32_t(colors[i].pixelFormat) : 0;
      key.blend[i] = colors[i] ? blend[i] : 0;
    }
    key.formats[4] = depth ? uint32_t(depth.pixelFormat) : 0;
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    __strong id<MTLRenderPipelineState>& pso = pipelines_[hash];
    if (pso || failed_pipelines_.count(hash)) return pso;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    const uint32_t interp = vs->info.interpolators & ps->info.inputsRead;
    d.vertexFunction = vs->Specialize(interp, alpha_func);
    d.fragmentFunction = ps->Specialize(interp, alpha_func);
    if (!d.vertexFunction || !d.fragmentFunction) {
      failed_pipelines_.insert(hash);
      return nil;
    }
    for (int i = 0; i < 4; ++i) {
      if (!colors[i]) continue;
      auto* att = d.colorAttachments[i];
      att.pixelFormat = colors[i].pixelFormat;
      att.writeMask = ((color_mask >> (i * 4)) & 1 ? MTLColorWriteMaskRed : 0) |
                      ((color_mask >> (i * 4)) & 2 ? MTLColorWriteMaskGreen : 0) |
                      ((color_mask >> (i * 4)) & 4 ? MTLColorWriteMaskBlue : 0) |
                      ((color_mask >> (i * 4)) & 8 ? MTLColorWriteMaskAlpha : 0);
      reg::RB_BLENDCONTROL b;
      b.value = blend[i];
      const bool opaque = b.color_srcblend == xenos::BlendFactor::kOne &&
                          b.color_destblend == xenos::BlendFactor::kZero &&
                          b.color_comb_fcn == xenos::BlendOp::kAdd &&
                          b.alpha_srcblend == xenos::BlendFactor::kOne &&
                          b.alpha_destblend == xenos::BlendFactor::kZero &&
                          b.alpha_comb_fcn == xenos::BlendOp::kAdd;
      att.blendingEnabled = !opaque;
      att.sourceRGBBlendFactor = MapBlendFactor(b.color_srcblend);
      att.destinationRGBBlendFactor = MapBlendFactor(b.color_destblend);
      att.rgbBlendOperation = MapBlendOp(b.color_comb_fcn);
      att.sourceAlphaBlendFactor = MapBlendFactor(b.alpha_srcblend);
      att.destinationAlphaBlendFactor = MapBlendFactor(b.alpha_destblend);
      att.alphaBlendOperation = MapBlendOp(b.alpha_comb_fcn);
    }
    if (depth) {
      d.depthAttachmentPixelFormat = depth.pixelFormat;
      d.stencilAttachmentPixelFormat = depth.pixelFormat;
    }
    NSError* err = nil;
    pso = [device_ newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) {
      failed_pipelines_.insert(hash);
      REXLOG_ERROR("Metal pipeline creation failed: {}", err ? err.localizedDescription.UTF8String : "");
      rex::SetBootError("Metal pipeline failed");
    }
    return pso;
  }

  id<MTLDepthStencilState> GetDepthState(reg::RB_DEPTHCONTROL ctrl, uint32_t rm, uint32_t rmb) {
    const uint32_t parts[3] = {ctrl.value, rm & 0xFFFF00u, rmb & 0xFFFF00u};
    const uint64_t key = XXH3_64bits(parts, sizeof(parts));
    __strong id<MTLDepthStencilState>& state = depth_states_[key];
    if (state) return state;
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    if (ctrl.z_enable) {
      d.depthCompareFunction = MTLCompareFunction(ctrl.zfunc);
      d.depthWriteEnabled = ctrl.z_write_enable;
    }
    if (ctrl.stencil_enable) {
      reg::RB_STENCILREFMASK f_reg, b_reg;
      f_reg.value = rm;
      b_reg.value = ctrl.backface_enable ? rmb : rm;
      MTLStencilDescriptor* f = [MTLStencilDescriptor new];
      f.stencilCompareFunction = MTLCompareFunction(ctrl.stencilfunc);
      f.stencilFailureOperation = MTLStencilOperation(ctrl.stencilfail);
      f.depthFailureOperation = MTLStencilOperation(ctrl.stencilzfail);
      f.depthStencilPassOperation = MTLStencilOperation(ctrl.stencilzpass);
      f.readMask = f_reg.stencilmask;
      f.writeMask = f_reg.stencilwritemask;
      MTLStencilDescriptor* b = [MTLStencilDescriptor new];
      b.stencilCompareFunction = MTLCompareFunction(ctrl.backface_enable ? ctrl.stencilfunc_bf : ctrl.stencilfunc);
      b.stencilFailureOperation = MTLStencilOperation(ctrl.backface_enable ? ctrl.stencilfail_bf : ctrl.stencilfail);
      b.depthFailureOperation = MTLStencilOperation(ctrl.backface_enable ? ctrl.stencilzfail_bf : ctrl.stencilzfail);
      b.depthStencilPassOperation = MTLStencilOperation(ctrl.backface_enable ? ctrl.stencilzpass_bf : ctrl.stencilzpass);
      b.readMask = b_reg.stencilmask;
      b.writeMask = b_reg.stencilwritemask;
      d.frontFaceStencil = f;
      d.backFaceStencil = b;
    }
    state = [device_ newDepthStencilStateWithDescriptor:d];
    return state;
  }

  id<MTLTexture> GammaRampTexture() {
    const auto* tbl = gamma_ramp_256_entry_table();
    uint32_t texels[256];
    bool has_lut = false;
    if (tbl) {
      for (uint32_t i = 0; i < 256; ++i) {
        if (tbl[i].value != 0) {
          has_lut = true;
          break;
        }
      }
    }
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t r = has_lut ? (tbl[i].color_10_red >> 2) : i;
      const uint32_t g = has_lut ? (tbl[i].color_10_green >> 2) : i;
      const uint32_t b = has_lut ? (tbl[i].color_10_blue >> 2) : i;
      texels[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    const uint64_t hash = XXH3_64bits(texels, sizeof(texels));
    if (!gamma_ramp_ || hash != gamma_hash_) {
      gamma_hash_ = hash;
      MTLTextureDescriptor* d =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                             width:256
                                                            height:1
                                                         mipmapped:NO];
      d.usage = MTLTextureUsageShaderRead;
      d.storageMode = MTLStorageModeShared;
      gamma_ramp_ = [device_ newTextureWithDescriptor:d];
      [gamma_ramp_ replaceRegion:MTLRegionMake2D(0, 0, 256, 1)
                     mipmapLevel:0
                       withBytes:texels
                     bytesPerRow:sizeof(texels)];
    }
    return gamma_ramp_;
  }

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  MetalGraphicsProvider* provider_ = nullptr;
  id<MTLBuffer> gpu_mem_ = nil;
  id<MTLBuffer> gpu_mem_hi_ = nil;
  uint32_t gpu_mem_split_byte_ = 0xFFFFFFFFu;
  id<MTLCommandBuffer> commands_ = nil;
  id<MTLCommandBuffer> last_submitted_cmd_ = nil;
  id<MTLRenderCommandEncoder> encoder_ = nil;
  id<MTLTexture> pass_colors_[4]{};
  id<MTLTexture> pass_depth_ = nil;
  uint32_t pass_w_ = 0, pass_h_ = 0;
  id<MTLTexture> dummy_tex_ = nil;
  id<MTLComputePipelineState> resolve_pso_ = nil;
  id<MTLComputePipelineState> clear_pso_ = nil;
  id<MTLRenderPipelineState> watcher_pso_ = nil;
  id<MTLTexture> watcher_tex_[2]{};
  id<MTLBuffer> watcher_buf_[2]{};
  std::atomic<bool> watcher_busy_[2]{{false}, {false}};
  std::atomic<bool> watcher_done_[2]{{false}, {false}};
  uint64_t watcher_cap_ms_[2]{};
  bool watcher_cap_fast_[2]{};
  int watcher_slot_ = 0;
  uint64_t last_watcher_ms_ = 0;
  id<MTLTexture> gamma_ramp_ = nil;
  id<MTLTexture> last_resolved_tex_ = nil;
  uint64_t gamma_hash_ = 0;

  std::unordered_map<uint64_t, std::unique_ptr<MetalShader>> shaders_;
  std::unordered_map<uint64_t, TexEntry> textures_;
  std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
  std::unordered_map<uint32_t, ResolvedTarget> resolved_;
  std::unordered_map<uint64_t, id<MTLTexture>> targets_;
  std::unordered_map<void*, std::pair<uint32_t, uint32_t>> drawn_extents_;
  std::unordered_map<void*, DepthClear> pending_depth_clears_;
  std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines_;
  std::unordered_set<uint64_t> failed_pipelines_;
  std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_states_;
  void ResetConstCache() {
    last_vs_const_.fill(0);
    last_ps_const_.fill(0);
    last_bool_const_.fill(0);
    last_fetch_const_.fill(0);
    last_vs_buf_ = last_ps_buf_ = last_bool_buf_ = last_fetch_buf_ = nil;
    last_vs_off_ = last_ps_off_ = last_bool_off_ = last_fetch_off_ = UINT32_MAX;
    enc_vs_buf_ = enc_ps_buf_ = enc_bool_buf_ = enc_fetch_buf_ = nil;
    enc_vs_off_ = enc_ps_off_ = enc_bool_off_ = enc_fetch_off_ = UINT32_MAX;
  }

  template <size_t N>
  id<MTLBuffer> UploadConstBlock(const void* src, uint32_t bytes, std::array<uint32_t, N>& shadow,
                                 __strong id<MTLBuffer>& cached_buf, uint32_t& cached_off, uint32_t& out_off) {
    if (!src || bytes == 0 || bytes > shadow.size() * sizeof(uint32_t)) return nil;
    if (cached_buf && cached_off != UINT32_MAX && std::memcmp(shadow.data(), src, bytes) == 0) {
      out_off = cached_off;
      return cached_buf;
    }
    id<MTLBuffer> target = const_ring_[ring_slot_];
    uint8_t* ptr = const_ring_ptr_[ring_slot_];
    uint32_t off = 0;
    if (!target || !ptr || !RexIosRingBumpAlloc(ring_offset_, bytes, kConstRingBytes, off)) {
      target = [device_ newBufferWithLength:std::max<uint32_t>(bytes, 256u)
                                    options:MTLResourceStorageModeShared];
      if (!target) return nil;
      ptr = static_cast<uint8_t*>([target contents]);
      if (!ptr || target.length < bytes) return nil;
      g_cur_buf_allocs.fetch_add(1, std::memory_order_relaxed);
      frame_overflow_bufs_[ring_slot_].push_back(target);
      off = 0;
    }
    std::memcpy(ptr + off, src, bytes);
    std::memcpy(shadow.data(), src, bytes);
    cached_buf = target;
    cached_off = off;
    out_off = off;
    return target;
  }

  struct CmdHandlerTracker {
    std::mutex mutex;
    std::condition_variable cv;
    int pending = 0;
    bool alive = true;
    void Schedule() {
      std::lock_guard<std::mutex> lock(mutex);
      ++pending;
    }
    void Complete() {
      std::lock_guard<std::mutex> lock(mutex);
      if (--pending <= 0) {
        pending = 0;
        cv.notify_all();
      }
    }
  };
  std::shared_ptr<CmdHandlerTracker> handler_tracker_;
  id<MTLBuffer> const_ring_[kConstRingSlots]{};
  uint8_t* const_ring_ptr_[kConstRingSlots]{};
  std::vector<id<MTLBuffer>> frame_overflow_bufs_[kConstRingSlots];
  std::vector<id<MTLTexture>> frame_overflow_textures_[kConstRingSlots];
  dispatch_semaphore_t ring_sem_ = nil;
  int ring_slot_ = 0;
  uint64_t swap_index_ = 0;
  uint32_t ring_offset_ = 0;

  std::array<uint32_t, 256 * 4> last_vs_const_{};
  std::array<uint32_t, 256 * 4> last_ps_const_{};
  std::array<uint32_t, 40> last_bool_const_{};
  std::array<uint32_t, 96 * 2> last_fetch_const_{};
  id<MTLBuffer> last_vs_buf_ = nil, last_ps_buf_ = nil, last_bool_buf_ = nil, last_fetch_buf_ = nil;
  uint32_t last_vs_off_ = UINT32_MAX, last_ps_off_ = UINT32_MAX;
  uint32_t last_bool_off_ = UINT32_MAX, last_fetch_off_ = UINT32_MAX;
  id<MTLBuffer> enc_vs_buf_ = nil, enc_ps_buf_ = nil, enc_bool_buf_ = nil, enc_fetch_buf_ = nil;
  uint32_t enc_vs_off_ = UINT32_MAX, enc_ps_off_ = UINT32_MAX;
  uint32_t enc_bool_off_ = UINT32_MAX, enc_fetch_off_ = UINT32_MAX;

  std::vector<uint8_t> tex_upload_blocks_;
  std::vector<uint8_t> tex_upload_rgba_;
  uint32_t frame_draws_ = 0;
  uint32_t frame_tex_uploads_ = 0;

  uint64_t generation_ = 1;
  uint64_t next_submit_seq_ = 1;
  std::atomic<uint64_t> completed_submit_seq_{0};
  std::unique_ptr<UiQuadSniffer> ui_sniffer_;
};

class MetalGraphicsSystem final : public GraphicsSystem {
 public:
  MetalGraphicsSystem() = default;
  ~MetalGraphicsSystem() override = default;

  std::string name() const override { return "Metal"; }

 protected:
  void CreateProvider(bool with_presentation) override {
    (void)with_presentation;
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      rex::SetBootError("Metal: MTLCreateSystemDefaultDevice returned nil");
      return;
    }
    queue_ = [device_ newCommandQueue];
    if (!queue_) {
      rex::SetBootError("Metal: newCommandQueue returned nil");
      return;
    }
    auto prov = std::make_unique<MetalGraphicsProvider>(device_, queue_);
    metal_provider_ = prov.get();
    provider_ = std::move(prov);
  }

  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<MetalCommandProcessor>(this, kernel_state_, device_, queue_,
                                                   metal_provider_);
  }

 private:
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  MetalGraphicsProvider* metal_provider_ = nullptr;
};

}  // namespace
}  // namespace rex_ios

std::unique_ptr<rex::graphics::GraphicsSystem> rex_ios_create_graphics_system() {
  return std::make_unique<rex_ios::MetalGraphicsSystem>();
}

RexIosPerfSnapshot rex_ios_get_perf_snapshot() {
  RexIosPerfSnapshot s{};
  s.draws_per_frame = rex_ios::g_perf_draws.load(std::memory_order_relaxed);
  s.tex_uploads_per_frame = rex_ios::g_perf_tex_uploads.load(std::memory_order_relaxed);
  s.buf_allocs_per_frame = rex_ios::g_perf_buf_allocs.load(std::memory_order_relaxed);
  return s;
}

uint64_t rex_ios_get_guest_frame_seq() {
  return rex_ios::g_guest_frame_seq.load(std::memory_order_acquire);
}

void rex_ios_wait_for_metal_idle() {
  id<MTLCommandBuffer> cmd = nil;
  {
    std::lock_guard<std::mutex> lock(rex_ios::g_metal_cmd_mutex);
    cmd = rex_ios::g_last_inflight_cmd;
  }
  if (cmd) {
    const MTLCommandBufferStatus st = cmd.status;
    if (st == MTLCommandBufferStatusCommitted || st == MTLCommandBufferStatusScheduled) {
      [cmd waitUntilCompleted];
    }
    std::lock_guard<std::mutex> lock(rex_ios::g_metal_cmd_mutex);
    if (rex_ios::g_last_inflight_cmd == cmd &&
        (cmd.status == MTLCommandBufferStatusCompleted ||
         cmd.status == MTLCommandBufferStatusError)) {
      rex_ios::g_last_inflight_cmd = nil;
      rex_ios::g_last_inflight_cmd_id = 0;
    }
  }
}
