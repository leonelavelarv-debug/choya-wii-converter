// Choya: WebGPU nulo. Implementa la API C de WebGPU (las cabeceras son las de Dawn, generadas de dawn.json) sin GPU
// y sin Dawn. aurora sigue armando sus pases y sus texturas como siempre, pero ninguna llamada hace trabajo de GPU:
// el motor GLES de Choya dibuja lo que aurora captura en common.cpp. Reemplaza a Dawn con el backend Null, que
// costaba memoria (búferes de 37 MB por triple, compilación de WGSL con Tint) y tiempo en el hilo de aurora.
//
// Lo que sí hace:
//  - Objetos con cuenta de referencias (AddRef/Release), para que los wgpu::Xxx de C++ funcionen igual.
//  - Búferes que se pueden mapear (MapRead/MapWrite o mappedAtCreation) tienen memoria de verdad: aurora escribe
//    sus vértices y uniformes en el búfer de staging mapeado. Los demás búferes no ocupan memoria.
//  - Texturas que recuerdan su tamaño, formato y usos (aurora los consulta).
//  - Llamadas con callback (MapAsync, RequestAdapter, ...) que terminan enseguida, respetando el modo: las
//    AllowSpontaneous se llaman en el momento; las WaitAnyOnly en WaitAny; las AllowProcessEvents en
//    ProcessEvents o WaitAny.
// Lo que falta en sin_dawn/webgpu_nulo_resto.inc (generado por scripts/generar-webgpu-nulo.py) no hace nada.
#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

namespace choya_nulo {

struct Objeto {
  std::atomic<uint32_t> refs{1};
  virtual ~Objeto() = default;
};

template <typename T> inline T* nuevo() { return new T(); }
inline void sumar(Objeto* o) {
  if (o != nullptr) o->refs.fetch_add(1, std::memory_order_relaxed);
}
inline void soltar(Objeto* o) {
  if (o != nullptr && o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

// Callbacks pendientes de los futuros (WGPUFuture).
struct Pendiente {
  uint64_t id;
  WGPUCallbackMode mode;
  std::function<void()> fn;
};
std::mutex g_mutex;
std::vector<Pendiente> g_pendientes;
std::vector<uint64_t> g_nunca;  // futuros que no terminan (la pérdida del dispositivo)
std::atomic<uint64_t> g_siguiente{1};

WGPUFuture programar(WGPUCallbackMode mode, std::function<void()> fn) {
  const uint64_t id = g_siguiente.fetch_add(1, std::memory_order_relaxed);
  if (mode == WGPUCallbackMode_AllowSpontaneous || mode == 0) {
    fn();
  } else {
    std::lock_guard lock(g_mutex);
    g_pendientes.push_back({id, mode, std::move(fn)});
  }
  return WGPUFuture{id};
}

// Saca (bajo el candado) y corre (fuera de él) los pendientes que cumplan `elegir`.
template <typename F> size_t correr(F elegir) {
  std::vector<std::function<void()>> listos;
  {
    std::lock_guard lock(g_mutex);
    for (auto it = g_pendientes.begin(); it != g_pendientes.end();) {
      if (elegir(*it)) {
        listos.push_back(std::move(it->fn));
        it = g_pendientes.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& fn : listos) fn();
  return listos.size();
}

inline WGPUStringView texto(const char* s) { return WGPUStringView{s, s != nullptr ? std::strlen(s) : 0}; }
inline WGPUStringView vacio() { return WGPUStringView{nullptr, 0}; }

WGPULimits limites() {
  WGPULimits l = WGPU_LIMITS_INIT;
  // Los mismos que Dawn Null (límites por defecto del nivel "core"), así aurora alinea y reparte igual que antes.
  l.maxTextureDimension1D = 8192;
  l.maxTextureDimension2D = 8192;
  l.maxTextureDimension3D = 2048;
  l.maxTextureArrayLayers = 256;
  l.maxBindGroups = 4;
  l.maxBindGroupsPlusVertexBuffers = 24;
  l.maxBindingsPerBindGroup = 1000;
  l.maxDynamicUniformBuffersPerPipelineLayout = 8;
  l.maxDynamicStorageBuffersPerPipelineLayout = 4;
  l.maxSampledTexturesPerShaderStage = 16;
  l.maxSamplersPerShaderStage = 16;
  l.maxStorageBuffersPerShaderStage = 8;
  l.maxStorageTexturesPerShaderStage = 4;
  l.maxUniformBuffersPerShaderStage = 12;
  l.maxUniformBufferBindingSize = 65536;
  l.maxStorageBufferBindingSize = 134217728;
  l.minUniformBufferOffsetAlignment = 256;
  l.minStorageBufferOffsetAlignment = 256;
  l.maxVertexBuffers = 8;
  l.maxBufferSize = 268435456;
  l.maxVertexAttributes = 16;
  l.maxVertexBufferArrayStride = 2048;
  l.maxInterStageShaderVariables = 16;
  l.maxColorAttachments = 8;
  l.maxColorAttachmentBytesPerSample = 32;
  l.maxComputeWorkgroupStorageSize = 16384;
  l.maxComputeInvocationsPerWorkgroup = 256;
  l.maxComputeWorkgroupSizeX = 256;
  l.maxComputeWorkgroupSizeY = 256;
  l.maxComputeWorkgroupSizeZ = 64;
  l.maxComputeWorkgroupsPerDimension = 65535;
  l.maxImmediateSize = 64;
  return l;
}

const WGPUFeatureName kFeatures[] = {WGPUFeatureName_TextureCompressionBC,
                                     WGPUFeatureName_ImplicitDeviceSynchronization};
bool tiene(WGPUFeatureName f) {
  for (auto x : kFeatures)
    if (x == f) return true;
  return false;
}

void info_adaptador(WGPUAdapterInfo* info) {
  info->vendor = texto("Choya");
  info->architecture = texto("");
  info->device = texto("Choya sin GPU (motor GLES)");
  info->description = texto("WebGPU nulo de Choya, sin Dawn");
  info->backendType = WGPUBackendType_Null;
  info->adapterType = WGPUAdapterType_CPU;
  info->vendorID = 0;
  info->deviceID = 0;
  info->subgroupMinSize = 4;
  info->subgroupMaxSize = 128;
}

}  // namespace choya_nulo

using choya_nulo::nuevo;
using choya_nulo::Objeto;
using choya_nulo::programar;
using choya_nulo::soltar;
using choya_nulo::sumar;

struct WGPUInstanceImpl : Objeto {};
struct WGPUAdapterImpl : Objeto {
  WGPUInstance instance = nullptr;
  ~WGPUAdapterImpl() override { soltar(instance); }
};
struct WGPUQueueImpl : Objeto {};
struct WGPUDeviceImpl : Objeto {
  WGPUAdapter adapter = nullptr;
  WGPUQueue queue = nullptr;
  WGPULimits limits{};
  WGPUDeviceLostCallbackInfo lost{};
  ~WGPUDeviceImpl() override {
    // Como Dawn: al soltar el dispositivo se avisa la pérdida con Destroyed (y el callback de C++ libera su lambda).
    if (lost.callback != nullptr) {
      const WGPUDevice nulo = nullptr;
      lost.callback(&nulo, WGPUDeviceLostReason_Destroyed, choya_nulo::texto("Choya: dispositivo soltado"),
                    lost.userdata1, lost.userdata2);
    }
    soltar(queue);
    soltar(adapter);
  }
};
struct WGPUBufferImpl : Objeto {
  uint64_t size = 0;
  WGPUBufferUsage usage = 0;
  uint8_t* mem = nullptr;
  WGPUBufferMapState state = WGPUBufferMapState_Unmapped;
  ~WGPUBufferImpl() override { std::free(mem); }
};
struct WGPUTextureImpl : Objeto {
  WGPUTextureUsage usage = 0;
  WGPUTextureDimension dimension = WGPUTextureDimension_2D;
  WGPUExtent3D size{1, 1, 1};
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  uint32_t mipLevelCount = 1;
  uint32_t sampleCount = 1;
};
struct WGPUTextureViewImpl : Objeto {
  WGPUTexture texture = nullptr;
  ~WGPUTextureViewImpl() override { soltar(texture); }
};
struct WGPUSurfaceImpl : Objeto {
  WGPUSurfaceConfiguration config{};
  bool configured = false;
};
struct WGPURenderPipelineImpl : Objeto {};
struct WGPUComputePipelineImpl : Objeto {};
struct WGPUBindGroupLayoutImpl : Objeto {};
struct WGPUShaderModuleImpl : Objeto {};
struct WGPUCommandEncoderImpl : Objeto {};
struct WGPUCommandBufferImpl : Objeto {};
struct WGPURenderPassEncoderImpl : Objeto {};
struct WGPUComputePassEncoderImpl : Objeto {};

#include "webgpu_nulo_tipos.inc"  // los demás objetos, sin datos

extern "C" {

// El motor de Choya lo busca (símbolo débil) para saber que aurora se compiló sin Dawn.
bool choya_webgpu_nulo() { return true; }

// --- Instancia ---------------------------------------------------------------------------------------------------
WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const*) { return nuevo<WGPUInstanceImpl>(); }

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance instance, WGPURequestAdapterOptions const*,
                                      WGPURequestAdapterCallbackInfo info) {
  auto* adapter = nuevo<WGPUAdapterImpl>();
  adapter->instance = instance;
  sumar(instance);
  return programar(info.mode, [info, adapter] {
    if (info.callback != nullptr) {
      info.callback(WGPURequestAdapterStatus_Success, adapter, choya_nulo::vacio(), info.userdata1, info.userdata2);
    } else {
      soltar(adapter);
    }
  });
}

WGPUWaitStatus wgpuInstanceWaitAny(WGPUInstance, size_t futureCount, WGPUFutureWaitInfo* futures, uint64_t) {
  if (futureCount == 0 || futures == nullptr) return WGPUWaitStatus_Success;
  bool alguno = false;
  for (size_t i = 0; i < futureCount; ++i) {
    const uint64_t id = futures[i].future.id;
    choya_nulo::correr([id](const choya_nulo::Pendiente& p) { return p.id == id; });
    bool nunca = false;
    {
      std::lock_guard lock(choya_nulo::g_mutex);
      for (auto n : choya_nulo::g_nunca) nunca = nunca || n == id;
    }
    futures[i].completed = nunca ? 0 : 1;
    alguno = alguno || !nunca;
  }
  return alguno ? WGPUWaitStatus_Success : WGPUWaitStatus_TimedOut;
}

void wgpuInstanceProcessEvents(WGPUInstance) {
  choya_nulo::correr([](const choya_nulo::Pendiente& p) { return p.mode == WGPUCallbackMode_AllowProcessEvents; });
}

WGPUSurface wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const*) { return nuevo<WGPUSurfaceImpl>(); }

void wgpuInstanceGetWGSLLanguageFeatures(WGPUInstance, WGPUSupportedWGSLLanguageFeatures* features) {
  features->featureCount = 0;
  features->features = nullptr;
}
WGPUBool wgpuInstanceHasWGSLLanguageFeature(WGPUInstance, WGPUWGSLLanguageFeatureName) { return 1; }
void wgpuGetInstanceFeatures(WGPUSupportedInstanceFeatures* features) {
  static const WGPUInstanceFeatureName kInst[] = {WGPUInstanceFeatureName_TimedWaitAny,
                                                  WGPUInstanceFeatureName_ShaderSourceSPIRV};
  features->featureCount = 2;
  features->features = kInst;
}
WGPUStatus wgpuGetInstanceLimits(WGPUInstanceLimits* limits) {
  limits->timedWaitAnyMaxCount = 64;
  return WGPUStatus_Success;
}
WGPUBool wgpuHasInstanceFeature(WGPUInstanceFeatureName) { return 1; }

// --- Adaptador ---------------------------------------------------------------------------------------------------
WGPUStatus wgpuAdapterGetInfo(WGPUAdapter, WGPUAdapterInfo* info) {
  choya_nulo::info_adaptador(info);
  return WGPUStatus_Success;
}
WGPUStatus wgpuAdapterGetLimits(WGPUAdapter, WGPULimits* limits) {
  WGPUChainedStruct* next = limits->nextInChain;
  *limits = choya_nulo::limites();
  limits->nextInChain = next;
  return WGPUStatus_Success;
}
void wgpuAdapterGetFeatures(WGPUAdapter, WGPUSupportedFeatures* features) {
  features->featureCount = sizeof(choya_nulo::kFeatures) / sizeof(choya_nulo::kFeatures[0]);
  features->features = choya_nulo::kFeatures;
}
WGPUBool wgpuAdapterHasFeature(WGPUAdapter, WGPUFeatureName feature) { return choya_nulo::tiene(feature) ? 1 : 0; }
WGPUInstance wgpuAdapterGetInstance(WGPUAdapter adapter) {
  sumar(adapter->instance);
  return adapter->instance;
}

static WGPUDevice crear_dispositivo(WGPUAdapter adapter, WGPUDeviceDescriptor const* descriptor) {
  auto* device = nuevo<WGPUDeviceImpl>();
  if (descriptor != nullptr) device->lost = descriptor->deviceLostCallbackInfo;
  device->adapter = adapter;
  sumar(adapter);
  device->queue = nuevo<WGPUQueueImpl>();
  device->limits = choya_nulo::limites();
  return device;
}
WGPUDevice wgpuAdapterCreateDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const* descriptor) {
  return crear_dispositivo(adapter, descriptor);
}
WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter adapter, WGPUDeviceDescriptor const* descriptor,
                                    WGPURequestDeviceCallbackInfo info) {
  WGPUDevice device = crear_dispositivo(adapter, descriptor);
  return programar(info.mode, [info, device] {
    if (info.callback != nullptr) {
      info.callback(WGPURequestDeviceStatus_Success, device, choya_nulo::vacio(), info.userdata1, info.userdata2);
    } else {
      soltar(device);
    }
  });
}

// --- Dispositivo -------------------------------------------------------------------------------------------------
WGPUStatus wgpuDeviceGetLimits(WGPUDevice device, WGPULimits* limits) {
  WGPUChainedStruct* next = limits->nextInChain;
  *limits = device->limits;
  limits->nextInChain = next;
  return WGPUStatus_Success;
}
void wgpuDeviceGetFeatures(WGPUDevice, WGPUSupportedFeatures* features) { wgpuAdapterGetFeatures(nullptr, features); }
WGPUBool wgpuDeviceHasFeature(WGPUDevice, WGPUFeatureName feature) { return choya_nulo::tiene(feature) ? 1 : 0; }
WGPUStatus wgpuDeviceGetAdapterInfo(WGPUDevice, WGPUAdapterInfo* info) {
  choya_nulo::info_adaptador(info);
  return WGPUStatus_Success;
}
WGPUAdapter wgpuDeviceGetAdapter(WGPUDevice device) {
  sumar(device->adapter);
  return device->adapter;
}
WGPUQueue wgpuDeviceGetQueue(WGPUDevice device) {
  sumar(device->queue);
  return device->queue;
}
WGPUFuture wgpuDeviceGetLostFuture(WGPUDevice) {
  const uint64_t id = choya_nulo::g_siguiente.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(choya_nulo::g_mutex);
  choya_nulo::g_nunca.push_back(id);
  return WGPUFuture{id};
}

static WGPUBuffer crear_bufer(WGPUBufferDescriptor const* d) {
  auto* buffer = nuevo<WGPUBufferImpl>();
  if (d == nullptr) return buffer;
  buffer->size = d->size;
  buffer->usage = d->usage;
  // Solo los búferes que la CPU puede ver llevan memoria. calloc deja las páginas sin tocar hasta que se escriben.
  if ((d->usage & (WGPUBufferUsage_MapRead | WGPUBufferUsage_MapWrite)) != 0 || d->mappedAtCreation) {
    buffer->mem = static_cast<uint8_t*>(std::calloc(d->size > 0 ? d->size : 1, 1));
  }
  if (d->mappedAtCreation) buffer->state = WGPUBufferMapState_Mapped;
  return buffer;
}
WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice, WGPUBufferDescriptor const* descriptor) {
  return crear_bufer(descriptor);
}
WGPUBuffer wgpuDeviceCreateErrorBuffer(WGPUDevice, WGPUBufferDescriptor const* descriptor) {
  return crear_bufer(descriptor);
}

static WGPUTexture crear_textura(WGPUTextureDescriptor const* d) {
  auto* texture = nuevo<WGPUTextureImpl>();
  if (d != nullptr) {
    texture->usage = d->usage;
    texture->dimension = d->dimension;
    texture->size = d->size;
    texture->format = d->format;
    texture->mipLevelCount = d->mipLevelCount;
    texture->sampleCount = d->sampleCount;
  }
  return texture;
}
WGPUTexture wgpuDeviceCreateTexture(WGPUDevice, WGPUTextureDescriptor const* descriptor) {
  return crear_textura(descriptor);
}
WGPUTexture wgpuDeviceCreateErrorTexture(WGPUDevice, WGPUTextureDescriptor const* descriptor) {
  return crear_textura(descriptor);
}

WGPUFuture wgpuDeviceCreateRenderPipelineAsync(WGPUDevice, WGPURenderPipelineDescriptor const*,
                                               WGPUCreateRenderPipelineAsyncCallbackInfo info) {
  auto* pipeline = nuevo<WGPURenderPipelineImpl>();
  return programar(info.mode, [info, pipeline] {
    if (info.callback != nullptr) {
      info.callback(WGPUCreatePipelineAsyncStatus_Success, pipeline, choya_nulo::vacio(), info.userdata1,
                    info.userdata2);
    } else {
      soltar(pipeline);
    }
  });
}
WGPUFuture wgpuDeviceCreateComputePipelineAsync(WGPUDevice, WGPUComputePipelineDescriptor const*,
                                                WGPUCreateComputePipelineAsyncCallbackInfo info) {
  auto* pipeline = nuevo<WGPUComputePipelineImpl>();
  return programar(info.mode, [info, pipeline] {
    if (info.callback != nullptr) {
      info.callback(WGPUCreatePipelineAsyncStatus_Success, pipeline, choya_nulo::vacio(), info.userdata1,
                    info.userdata2);
    } else {
      soltar(pipeline);
    }
  });
}
WGPUFuture wgpuDevicePopErrorScope(WGPUDevice, WGPUPopErrorScopeCallbackInfo info) {
  return programar(info.mode, [info] {
    if (info.callback != nullptr) {
      info.callback(WGPUPopErrorScopeStatus_Success, WGPUErrorType_NoError, choya_nulo::vacio(), info.userdata1,
                    info.userdata2);
    }
  });
}

// --- Cola ----------------------------------------------------------------------------------------------------------
WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue, WGPUQueueWorkDoneCallbackInfo info) {
  return programar(info.mode, [info] {
    if (info.callback != nullptr) {
      info.callback(WGPUQueueWorkDoneStatus_Success, choya_nulo::vacio(), info.userdata1, info.userdata2);
    }
  });
}
void wgpuQueueWriteBuffer(WGPUQueue, WGPUBuffer buffer, uint64_t offset, void const* data, size_t size) {
  if (buffer != nullptr && buffer->mem != nullptr && data != nullptr && offset + size <= buffer->size) {
    std::memcpy(buffer->mem + offset, data, size);
  }
}

// --- Búferes -------------------------------------------------------------------------------------------------------
WGPUFuture wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode, size_t, size_t, WGPUBufferMapCallbackInfo info) {
  const bool ok = buffer->mem != nullptr;
  if (ok) buffer->state = WGPUBufferMapState_Mapped;
  return programar(info.mode, [info, ok] {
    if (info.callback != nullptr) {
      info.callback(ok ? WGPUMapAsyncStatus_Success : WGPUMapAsyncStatus_Error,
                    choya_nulo::texto(ok ? "" : "Choya: búfer sin memoria de CPU"), info.userdata1, info.userdata2);
    }
  });
}
static uint8_t* rango(WGPUBuffer buffer, size_t offset, size_t size) {
  if (buffer == nullptr || buffer->mem == nullptr || buffer->state != WGPUBufferMapState_Mapped) return nullptr;
  if (offset > buffer->size) return nullptr;
  if (size != WGPU_WHOLE_MAP_SIZE && offset + size > buffer->size) return nullptr;
  return buffer->mem + offset;
}
void* wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size) { return rango(buffer, offset, size); }
void const* wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
  return rango(buffer, offset, size);
}
WGPUStatus wgpuBufferReadMappedRange(WGPUBuffer buffer, size_t offset, void* data, size_t size) {
  const uint8_t* p = rango(buffer, offset, size);
  if (p == nullptr) return WGPUStatus_Error;
  std::memcpy(data, p, size);
  return WGPUStatus_Success;
}
WGPUStatus wgpuBufferWriteMappedRange(WGPUBuffer buffer, size_t offset, void const* data, size_t size) {
  uint8_t* p = rango(buffer, offset, size);
  if (p == nullptr) return WGPUStatus_Error;
  std::memcpy(p, data, size);
  return WGPUStatus_Success;
}
void wgpuBufferUnmap(WGPUBuffer buffer) { buffer->state = WGPUBufferMapState_Unmapped; }
void wgpuBufferDestroy(WGPUBuffer buffer) {
  buffer->state = WGPUBufferMapState_Unmapped;
  std::free(buffer->mem);  // la memoria se va ya; el objeto vive mientras tenga referencias
  buffer->mem = nullptr;
}
WGPUBufferMapState wgpuBufferGetMapState(WGPUBuffer buffer) { return buffer->state; }
uint64_t wgpuBufferGetSize(WGPUBuffer buffer) { return buffer->size; }
WGPUBufferUsage wgpuBufferGetUsage(WGPUBuffer buffer) { return buffer->usage; }

void wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder, WGPUBuffer source, uint64_t sourceOffset,
                                          WGPUBuffer destination, uint64_t destinationOffset, uint64_t size) {
  if (source == nullptr || destination == nullptr || source->mem == nullptr || destination->mem == nullptr) return;
  if (size == WGPU_WHOLE_SIZE) size = source->size - sourceOffset;
  if (sourceOffset + size > source->size || destinationOffset + size > destination->size) return;
  std::memmove(destination->mem + destinationOffset, source->mem + sourceOffset, size);
}

// --- Texturas ------------------------------------------------------------------------------------------------------
WGPUTextureView wgpuTextureCreateView(WGPUTexture texture, WGPUTextureViewDescriptor const*) {
  auto* view = nuevo<WGPUTextureViewImpl>();
  view->texture = texture;
  sumar(texture);
  return view;
}
WGPUTextureView wgpuTextureCreateErrorView(WGPUTexture texture, WGPUTextureViewDescriptor const* descriptor) {
  return wgpuTextureCreateView(texture, descriptor);
}
uint32_t wgpuTextureGetWidth(WGPUTexture texture) { return texture->size.width; }
uint32_t wgpuTextureGetHeight(WGPUTexture texture) { return texture->size.height; }
uint32_t wgpuTextureGetDepthOrArrayLayers(WGPUTexture texture) { return texture->size.depthOrArrayLayers; }
WGPUTextureDimension wgpuTextureGetDimension(WGPUTexture texture) { return texture->dimension; }
WGPUTextureFormat wgpuTextureGetFormat(WGPUTexture texture) { return texture->format; }
uint32_t wgpuTextureGetMipLevelCount(WGPUTexture texture) { return texture->mipLevelCount; }
uint32_t wgpuTextureGetSampleCount(WGPUTexture texture) { return texture->sampleCount; }
WGPUTextureUsage wgpuTextureGetUsage(WGPUTexture texture) { return texture->usage; }
WGPUTextureViewDimension wgpuTextureGetTextureBindingViewDimension(WGPUTexture texture) {
  return texture->dimension == WGPUTextureDimension_3D ? WGPUTextureViewDimension_3D
                                                       : WGPUTextureViewDimension_2D;
}

// --- Superficie ----------------------------------------------------------------------------------------------------
WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface, WGPUAdapter, WGPUSurfaceCapabilities* caps) {
  static const WGPUTextureFormat kFormats[] = {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm};
  static const WGPUPresentMode kModes[] = {WGPUPresentMode_Fifo, WGPUPresentMode_Immediate, WGPUPresentMode_Mailbox};
  static const WGPUCompositeAlphaMode kAlpha[] = {WGPUCompositeAlphaMode_Opaque};
  caps->usages = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst |
                 WGPUTextureUsage_TextureBinding;
  caps->formatCount = 2;
  caps->formats = kFormats;
  caps->presentModeCount = 3;
  caps->presentModes = kModes;
  caps->alphaModeCount = 1;
  caps->alphaModes = kAlpha;
  return WGPUStatus_Success;
}
void wgpuSurfaceConfigure(WGPUSurface surface, WGPUSurfaceConfiguration const* config) {
  surface->config = *config;
  surface->config.nextInChain = nullptr;
  surface->config.device = nullptr;  // sin referencia: la superficie no mantiene vivo al dispositivo
  surface->config.viewFormatCount = 0;
  surface->config.viewFormats = nullptr;
  surface->configured = true;
}
void wgpuSurfaceUnconfigure(WGPUSurface surface) { surface->configured = false; }
void wgpuSurfaceGetCurrentTexture(WGPUSurface surface, WGPUSurfaceTexture* out) {
  auto* texture = nuevo<WGPUTextureImpl>();
  texture->usage = surface->config.usage;
  texture->format = surface->config.format;
  texture->size = WGPUExtent3D{surface->config.width > 0 ? surface->config.width : 1,
                               surface->config.height > 0 ? surface->config.height : 1, 1};
  out->texture = texture;
  out->status = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;
}
WGPUStatus wgpuSurfacePresent(WGPUSurface) { return WGPUStatus_Success; }

// --- Shaders -------------------------------------------------------------------------------------------------------
WGPUFuture wgpuShaderModuleGetCompilationInfo(WGPUShaderModule, WGPUCompilationInfoCallbackInfo info) {
  return programar(info.mode, [info] {
    if (info.callback != nullptr) {
      const WGPUCompilationInfo empty{nullptr, 0, nullptr};
      info.callback(WGPUCompilationInfoRequestStatus_Success, &empty, info.userdata1, info.userdata2);
    }
  });
}

}  // extern "C"

#include "webgpu_nulo_resto.inc"  // el resto de la API: no hace nada
