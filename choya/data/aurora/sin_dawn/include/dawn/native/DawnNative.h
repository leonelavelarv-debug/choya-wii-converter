// Choya: lo poco de dawn/native/DawnNative.h que usa aurora (webgpu/gpu.cpp), para compilar sin Dawn.
// La biblioteca WebGPU nula de Choya (sin_dawn/webgpu_nulo.cpp) no tiene backends: no hay nada que validar ni
// interruptores (toggles) que listar.
#ifndef INCLUDE_DAWN_NATIVE_DAWNNATIVE_H_
#define INCLUDE_DAWN_NATIVE_DAWNNATIVE_H_

#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace dawn::platform {
class Platform;
}

namespace dawn::native {

enum BackendValidationLevel { Full, Partial, Disabled };

struct DawnInstanceDescriptor : wgpu::ChainedStruct {
  DawnInstanceDescriptor() { sType = wgpu::SType::DawnInstanceDescriptor; }
  uint32_t additionalRuntimeSearchPathsCount = 0;
  const char* const* additionalRuntimeSearchPaths = nullptr;
  dawn::platform::Platform* platform = nullptr;
  BackendValidationLevel backendValidationLevel = BackendValidationLevel::Disabled;
  bool beginCaptureOnStartup = false;
  WGPULoggingCallbackInfo loggingCallbackInfo = WGPU_LOGGING_CALLBACK_INFO_INIT;
};

inline std::vector<const char*> GetTogglesUsed(WGPUDevice) { return {}; }
inline std::vector<const char*> GetTogglesUsed(const wgpu::Adapter&) { return {}; }
inline void PerformIdleTasks(const wgpu::Device&) {}
inline bool IsDeviceLost(WGPUDevice) { return false; }

}  // namespace dawn::native

#endif  // INCLUDE_DAWN_NATIVE_DAWNNATIVE_H_
