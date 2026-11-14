// Prueba del WebGPU nulo de Choya con la API C++ de Dawn, como la usa aurora (webgpu/gpu.cpp, gfx/common.cpp).
#include <webgpu/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>

#include <cstdio>
#include <cstring>

#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FALLO %s:%d %s\n", __FILE__, __LINE__, #c);      \
      return 1;                                                     \
    }                                                               \
  } while (0)

int main() {
  dawn::native::DawnInstanceDescriptor dawnDesc;
  wgpu::InstanceDescriptor instanceDescriptor{};
  instanceDescriptor.nextInChain = &dawnDesc;
  wgpu::Instance instance = wgpu::CreateInstance(&instanceDescriptor);
  CHECK(instance);

  wgpu::Adapter adapter;
  const wgpu::RequestAdapterOptions options{.backendType = wgpu::BackendType::Null};
  auto future = instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView) {
                                          if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
                                        });
  CHECK(!adapter);  // WaitAnyOnly: todavía no
  CHECK(instance.WaitAny(future, 5000000000) == wgpu::WaitStatus::Success);
  CHECK(adapter);
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  CHECK(info.backendType == wgpu::BackendType::Null);
  wgpu::Limits limits{};
  adapter.GetLimits(&limits);
  CHECK(limits.minUniformBufferOffsetAlignment == 256);
  CHECK(adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC));

  wgpu::DeviceDescriptor deviceDescriptor;
  int lostCalls = 0;
  deviceDescriptor.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [&lostCalls](const wgpu::Device&, wgpu::DeviceLostReason, wgpu::StringView) { ++lostCalls; });
  deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView) {});
  wgpu::Device device;
  auto f2 = adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                  [&](wgpu::RequestDeviceStatus s, wgpu::Device d, wgpu::StringView) {
                                    if (s == wgpu::RequestDeviceStatus::Success) device = std::move(d);
                                  });
  CHECK(instance.WaitAny(f2, 5000000000) == wgpu::WaitStatus::Success);
  CHECK(device);
  wgpu::Queue queue = device.GetQueue();
  CHECK(queue);
  CHECK(dawn::native::GetTogglesUsed(device.Get()).empty());

  // Búfer de staging como el de aurora: MapWrite, se mapea con AllowSpontaneous y se escribe.
  const wgpu::BufferDescriptor staging{.usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
                                       .size = 1 << 20};
  wgpu::Buffer buf = device.CreateBuffer(&staging);
  bool mapped = false;
  buf.MapAsync(wgpu::MapMode::Write, 0, 1 << 20, wgpu::CallbackMode::AllowSpontaneous,
               [&](wgpu::MapAsyncStatus status, wgpu::StringView) { mapped = status == wgpu::MapAsyncStatus::Success; });
  CHECK(mapped);
  auto* p = static_cast<unsigned char*>(buf.GetMappedRange(4096, 1024));
  CHECK(p != nullptr);
  std::memset(p, 0xAB, 1024);
  buf.Unmap();
  CHECK(buf.GetMappedRange(0, 16) == nullptr);

  // Un búfer de GPU no tiene memoria: mapearlo falla.
  const wgpu::BufferDescriptor gpuOnly{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                       .size = 64 << 20};
  wgpu::Buffer uniforms = device.CreateBuffer(&gpuOnly);
  CHECK(uniforms.GetSize() == (64u << 20));

  // Lectura: copia de staging a un búfer MapRead, MapAsync con AllowProcessEvents.
  const wgpu::BufferDescriptor readback{.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                        .size = 1024};
  wgpu::Buffer rb = device.CreateBuffer(&readback);
  auto enc = device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(buf, 4096, rb, 0, 1024);
  auto cmd = enc.Finish();
  queue.Submit(1, &cmd);
  bool readDone = false;
  rb.MapAsync(wgpu::MapMode::Read, 0, 1024, wgpu::CallbackMode::AllowProcessEvents,
              [&](wgpu::MapAsyncStatus s, wgpu::StringView) { readDone = s == wgpu::MapAsyncStatus::Success; });
  CHECK(!readDone);
  instance.ProcessEvents();
  CHECK(readDone);
  CHECK(static_cast<const unsigned char*>(rb.GetConstMappedRange(0, 1024))[1023] == 0xAB);

  // Texturas, vistas y pases.
  const wgpu::TextureDescriptor td{.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
                                   .size = {640, 528, 1},
                                   .format = wgpu::TextureFormat::RGBA8Unorm};
  wgpu::Texture tex = device.CreateTexture(&td);
  CHECK(tex.GetWidth() == 640 && tex.GetHeight() == 528);
  CHECK(tex.GetFormat() == wgpu::TextureFormat::RGBA8Unorm);
  wgpu::TextureView view = tex.CreateView();
  CHECK(view);
  tex = {};  // la vista la mantiene viva
  wgpu::RenderPassColorAttachment color{.view = view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor rp{.colorAttachmentCount = 1, .colorAttachments = &color};
  auto enc2 = device.CreateCommandEncoder();
  auto pass = enc2.BeginRenderPass(&rp);
  pass.SetViewport(0, 0, 640, 528, 0, 1);
  pass.Draw(3);
  pass.End();
  auto cmd2 = enc2.Finish();
  queue.Submit(1, &cmd2);

  // Shaders y pipelines.
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = "@vertex fn main() -> @builtin(position) vec4f { return vec4f(); }";
  const wgpu::ShaderModuleDescriptor smd{.nextInChain = &wgsl};
  auto module = device.CreateShaderModule(&smd);
  CHECK(module);
  wgpu::RenderPipeline pipeline;
  const wgpu::RenderPipelineDescriptor rpd{.vertex = {.module = module}};
  auto f3 = device.CreateRenderPipelineAsync(&rpd, wgpu::CallbackMode::AllowSpontaneous,
                                             [&](wgpu::CreatePipelineAsyncStatus s, wgpu::RenderPipeline p,
                                                 wgpu::StringView) { pipeline = std::move(p); });
  (void)f3;
  CHECK(pipeline);
  CHECK(pipeline.GetBindGroupLayout(0));

  {  // Superficie.
  wgpu::Surface surface = instance.CreateSurface(nullptr);
  wgpu::SurfaceCapabilities caps;
  CHECK(surface.GetCapabilities(adapter, &caps) == wgpu::Status::Success);
  CHECK(caps.formatCount > 0 && caps.presentModeCount > 0);
  const wgpu::SurfaceConfiguration sc{.device = device, .format = caps.formats[0], .width = 640, .height = 480};
  surface.Configure(&sc);
  wgpu::SurfaceTexture st;
  surface.GetCurrentTexture(&st);
  CHECK(st.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal);
  CHECK(st.texture.GetWidth() == 640);
  CHECK(surface.Present() == wgpu::Status::Success);
  }

  // Futuro de la pérdida del dispositivo: nunca termina.
  bool popped = false;
  auto f4 = device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                                 [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType t, wgpu::StringView) {
                                   popped = t == wgpu::ErrorType::NoError;
                                 });
  CHECK(instance.WaitAny(f4, 0) == wgpu::WaitStatus::Success && popped);

  dawn::native::PerformIdleTasks(device);
  device = {};
  queue = {};
  CHECK(lostCalls == 1);  // al soltar el dispositivo: Destroyed
  std::printf("webgpu nulo: bien\n");
  return 0;
}
