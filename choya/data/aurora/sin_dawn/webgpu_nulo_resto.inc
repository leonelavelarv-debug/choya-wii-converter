// Generado por scripts/generar-webgpu-nulo.py a partir de include/dawn/webgpu.h. No editar a mano.
extern "C" {
WGPUProc wgpuGetProcAddress(WGPUStringView procName) { return nullptr; }
WGPUStatus wgpuAdapterGetFormatCapabilities(WGPUAdapter adapter, WGPUTextureFormat format, WGPUDawnFormatCapabilities * capabilities) { return WGPUStatus_Success; }
void wgpuAdapterAddRef(WGPUAdapter adapter) { choya_nulo::sumar(adapter); }
void wgpuAdapterRelease(WGPUAdapter adapter) { choya_nulo::soltar(adapter); }
void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo adapterInfo) {}
void wgpuAdapterPropertiesMemoryHeapsFreeMembers(WGPUAdapterPropertiesMemoryHeaps adapterPropertiesMemoryHeaps) {}
void wgpuAdapterPropertiesSubgroupMatrixConfigsFreeMembers(WGPUAdapterPropertiesSubgroupMatrixConfigs adapterPropertiesSubgroupMatrixConfigs) {}
void wgpuBindGroupSetLabel(WGPUBindGroup bindGroup, WGPUStringView label) {}
void wgpuBindGroupAddRef(WGPUBindGroup bindGroup) { choya_nulo::sumar(bindGroup); }
void wgpuBindGroupRelease(WGPUBindGroup bindGroup) { choya_nulo::soltar(bindGroup); }
void wgpuBindGroupLayoutSetLabel(WGPUBindGroupLayout bindGroupLayout, WGPUStringView label) {}
void wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout bindGroupLayout) { choya_nulo::sumar(bindGroupLayout); }
void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout bindGroupLayout) { choya_nulo::soltar(bindGroupLayout); }
WGPUTexelBufferView wgpuBufferCreateTexelView(WGPUBuffer buffer, WGPUTexelBufferViewDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUTexelBufferViewImpl>(); }
void wgpuBufferSetLabel(WGPUBuffer buffer, WGPUStringView label) {}
void wgpuBufferAddRef(WGPUBuffer buffer) { choya_nulo::sumar(buffer); }
void wgpuBufferRelease(WGPUBuffer buffer) { choya_nulo::soltar(buffer); }
void wgpuCommandBufferSetLabel(WGPUCommandBuffer commandBuffer, WGPUStringView label) {}
void wgpuCommandBufferAddRef(WGPUCommandBuffer commandBuffer) { choya_nulo::sumar(commandBuffer); }
void wgpuCommandBufferRelease(WGPUCommandBuffer commandBuffer) { choya_nulo::soltar(commandBuffer); }
WGPUComputePassEncoder wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder commandEncoder, WGPU_NULLABLE WGPUComputePassDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUComputePassEncoderImpl>(); }
WGPURenderPassEncoder wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder commandEncoder, WGPURenderPassDescriptor const * descriptor) { return choya_nulo::nuevo<WGPURenderPassEncoderImpl>(); }
void wgpuCommandEncoderClearBuffer(WGPUCommandEncoder commandEncoder, WGPUBuffer buffer, uint64_t offset, uint64_t size) {}
void wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder commandEncoder, WGPUTexelCopyBufferInfo const * source, WGPUTexelCopyTextureInfo const * destination, WGPUExtent3D const * copySize) {}
void wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder commandEncoder, WGPUTexelCopyTextureInfo const * source, WGPUTexelCopyBufferInfo const * destination, WGPUExtent3D const * copySize) {}
void wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder commandEncoder, WGPUTexelCopyTextureInfo const * source, WGPUTexelCopyTextureInfo const * destination, WGPUExtent3D const * copySize) {}
WGPUCommandBuffer wgpuCommandEncoderFinish(WGPUCommandEncoder commandEncoder, WGPU_NULLABLE WGPUCommandBufferDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUCommandBufferImpl>(); }
void wgpuCommandEncoderInjectValidationError(WGPUCommandEncoder commandEncoder, WGPUStringView message) {}
void wgpuCommandEncoderInsertDebugMarker(WGPUCommandEncoder commandEncoder, WGPUStringView markerLabel) {}
void wgpuCommandEncoderPopDebugGroup(WGPUCommandEncoder commandEncoder) {}
void wgpuCommandEncoderPushDebugGroup(WGPUCommandEncoder commandEncoder, WGPUStringView groupLabel) {}
void wgpuCommandEncoderResolveQuerySet(WGPUCommandEncoder commandEncoder, WGPUQuerySet querySet, uint32_t firstQuery, uint32_t queryCount, WGPUBuffer destination, uint64_t destinationOffset) {}
void wgpuCommandEncoderSetLabel(WGPUCommandEncoder commandEncoder, WGPUStringView label) {}
void wgpuCommandEncoderWriteBuffer(WGPUCommandEncoder commandEncoder, WGPUBuffer buffer, uint64_t bufferOffset, uint8_t const * data, uint64_t size) {}
void wgpuCommandEncoderWriteTimestamp(WGPUCommandEncoder commandEncoder, WGPUQuerySet querySet, uint32_t queryIndex) {}
void wgpuCommandEncoderAddRef(WGPUCommandEncoder commandEncoder) { choya_nulo::sumar(commandEncoder); }
void wgpuCommandEncoderRelease(WGPUCommandEncoder commandEncoder) { choya_nulo::soltar(commandEncoder); }
void wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder computePassEncoder, uint32_t workgroupCountX, uint32_t workgroupCountY, uint32_t workgroupCountZ) {}
void wgpuComputePassEncoderDispatchWorkgroupsIndirect(WGPUComputePassEncoder computePassEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset) {}
void wgpuComputePassEncoderEnd(WGPUComputePassEncoder computePassEncoder) {}
void wgpuComputePassEncoderInsertDebugMarker(WGPUComputePassEncoder computePassEncoder, WGPUStringView markerLabel) {}
void wgpuComputePassEncoderPopDebugGroup(WGPUComputePassEncoder computePassEncoder) {}
void wgpuComputePassEncoderPushDebugGroup(WGPUComputePassEncoder computePassEncoder, WGPUStringView groupLabel) {}
void wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder computePassEncoder, uint32_t groupIndex, WGPU_NULLABLE WGPUBindGroup group, size_t dynamicOffsetCount, uint32_t const * dynamicOffsets) {}
void wgpuComputePassEncoderSetImmediates(WGPUComputePassEncoder computePassEncoder, uint32_t offset, void const * data, size_t size) {}
void wgpuComputePassEncoderSetLabel(WGPUComputePassEncoder computePassEncoder, WGPUStringView label) {}
void wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder computePassEncoder, WGPUComputePipeline pipeline) {}
void wgpuComputePassEncoderSetResourceTable(WGPUComputePassEncoder computePassEncoder, WGPU_NULLABLE WGPUResourceTable table) {}
void wgpuComputePassEncoderWriteTimestamp(WGPUComputePassEncoder computePassEncoder, WGPUQuerySet querySet, uint32_t queryIndex) {}
void wgpuComputePassEncoderAddRef(WGPUComputePassEncoder computePassEncoder) { choya_nulo::sumar(computePassEncoder); }
void wgpuComputePassEncoderRelease(WGPUComputePassEncoder computePassEncoder) { choya_nulo::soltar(computePassEncoder); }
WGPUBindGroupLayout wgpuComputePipelineGetBindGroupLayout(WGPUComputePipeline computePipeline, uint32_t groupIndex) { return choya_nulo::nuevo<WGPUBindGroupLayoutImpl>(); }
void wgpuComputePipelineSetLabel(WGPUComputePipeline computePipeline, WGPUStringView label) {}
void wgpuComputePipelineAddRef(WGPUComputePipeline computePipeline) { choya_nulo::sumar(computePipeline); }
void wgpuComputePipelineRelease(WGPUComputePipeline computePipeline) { choya_nulo::soltar(computePipeline); }
void wgpuDawnDrmFormatCapabilitiesFreeMembers(WGPUDawnDrmFormatCapabilities dawnDrmFormatCapabilities) {}
WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice device, WGPUBindGroupDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUBindGroupImpl>(); }
WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice device, WGPUBindGroupLayoutDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUBindGroupLayoutImpl>(); }
WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice device, WGPU_NULLABLE WGPUCommandEncoderDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUCommandEncoderImpl>(); }
WGPUComputePipeline wgpuDeviceCreateComputePipeline(WGPUDevice device, WGPUComputePipelineDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUComputePipelineImpl>(); }
WGPUExternalTexture wgpuDeviceCreateErrorExternalTexture(WGPUDevice device) { return choya_nulo::nuevo<WGPUExternalTextureImpl>(); }
WGPUShaderModule wgpuDeviceCreateErrorShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const * descriptor, WGPUStringView errorMessage) { return choya_nulo::nuevo<WGPUShaderModuleImpl>(); }
WGPUExternalTexture wgpuDeviceCreateExternalTexture(WGPUDevice device, WGPUExternalTextureDescriptor const * externalTextureDescriptor) { return choya_nulo::nuevo<WGPUExternalTextureImpl>(); }
WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice device, WGPUPipelineLayoutDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUPipelineLayoutImpl>(); }
WGPUQuerySet wgpuDeviceCreateQuerySet(WGPUDevice device, WGPUQuerySetDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUQuerySetImpl>(); }
WGPURenderBundleEncoder wgpuDeviceCreateRenderBundleEncoder(WGPUDevice device, WGPURenderBundleEncoderDescriptor const * descriptor) { return choya_nulo::nuevo<WGPURenderBundleEncoderImpl>(); }
WGPURenderPipeline wgpuDeviceCreateRenderPipeline(WGPUDevice device, WGPURenderPipelineDescriptor const * descriptor) { return choya_nulo::nuevo<WGPURenderPipelineImpl>(); }
WGPUResourceTable wgpuDeviceCreateResourceTable(WGPUDevice device, WGPUResourceTableDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUResourceTableImpl>(); }
WGPUSampler wgpuDeviceCreateSampler(WGPUDevice device, WGPU_NULLABLE WGPUSamplerDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUSamplerImpl>(); }
WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUShaderModuleImpl>(); }
void wgpuDeviceDestroy(WGPUDevice device) {}
void wgpuDeviceForceLoss(WGPUDevice device, WGPUDeviceLostReason type, WGPUStringView message) {}
WGPUStatus wgpuDeviceGetAHardwareBufferProperties(WGPUDevice device, void * handle, WGPUAHardwareBufferProperties * properties) { return WGPUStatus_Success; }
WGPUSharedBufferMemory wgpuDeviceImportSharedBufferMemory(WGPUDevice device, WGPUSharedBufferMemoryDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUSharedBufferMemoryImpl>(); }
WGPUSharedFence wgpuDeviceImportSharedFence(WGPUDevice device, WGPUSharedFenceDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUSharedFenceImpl>(); }
WGPUSharedTextureMemory wgpuDeviceImportSharedTextureMemory(WGPUDevice device, WGPUSharedTextureMemoryDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUSharedTextureMemoryImpl>(); }
void wgpuDeviceInjectError(WGPUDevice device, WGPUErrorType type, WGPUStringView message) {}
void wgpuDevicePushErrorScope(WGPUDevice device, WGPUErrorFilter filter) {}
void wgpuDeviceSetLabel(WGPUDevice device, WGPUStringView label) {}
void wgpuDeviceSetLoggingCallback(WGPUDevice device, WGPULoggingCallbackInfo callbackInfo) {}
void wgpuDeviceTick(WGPUDevice device) {}
void wgpuDeviceValidateTextureDescriptor(WGPUDevice device, WGPUTextureDescriptor const * descriptor) {}
void wgpuDeviceAddRef(WGPUDevice device) { choya_nulo::sumar(device); }
void wgpuDeviceRelease(WGPUDevice device) { choya_nulo::soltar(device); }
void wgpuExternalTextureDestroy(WGPUExternalTexture externalTexture) {}
void wgpuExternalTextureExpire(WGPUExternalTexture externalTexture) {}
void wgpuExternalTextureRefresh(WGPUExternalTexture externalTexture) {}
void wgpuExternalTextureSetLabel(WGPUExternalTexture externalTexture, WGPUStringView label) {}
void wgpuExternalTextureAddRef(WGPUExternalTexture externalTexture) { choya_nulo::sumar(externalTexture); }
void wgpuExternalTextureRelease(WGPUExternalTexture externalTexture) { choya_nulo::soltar(externalTexture); }
void wgpuInstanceAddRef(WGPUInstance instance) { choya_nulo::sumar(instance); }
void wgpuInstanceRelease(WGPUInstance instance) { choya_nulo::soltar(instance); }
void wgpuPipelineLayoutSetLabel(WGPUPipelineLayout pipelineLayout, WGPUStringView label) {}
void wgpuPipelineLayoutAddRef(WGPUPipelineLayout pipelineLayout) { choya_nulo::sumar(pipelineLayout); }
void wgpuPipelineLayoutRelease(WGPUPipelineLayout pipelineLayout) { choya_nulo::soltar(pipelineLayout); }
void wgpuQuerySetDestroy(WGPUQuerySet querySet) {}
uint32_t wgpuQuerySetGetCount(WGPUQuerySet querySet) { return uint32_t{}; }
WGPUQueryType wgpuQuerySetGetType(WGPUQuerySet querySet) { return WGPUQueryType{}; }
void wgpuQuerySetSetLabel(WGPUQuerySet querySet, WGPUStringView label) {}
void wgpuQuerySetAddRef(WGPUQuerySet querySet) { choya_nulo::sumar(querySet); }
void wgpuQuerySetRelease(WGPUQuerySet querySet) { choya_nulo::soltar(querySet); }
void wgpuQueueCopyExternalTextureForBrowser(WGPUQueue queue, WGPUImageCopyExternalTexture const * source, WGPUTexelCopyTextureInfo const * destination, WGPUExtent3D const * copySize, WGPUCopyTextureForBrowserOptions const * options) {}
void wgpuQueueCopyTextureForBrowser(WGPUQueue queue, WGPUTexelCopyTextureInfo const * source, WGPUTexelCopyTextureInfo const * destination, WGPUExtent3D const * copySize, WGPUCopyTextureForBrowserOptions const * options) {}
void wgpuQueueSetLabel(WGPUQueue queue, WGPUStringView label) {}
void wgpuQueueSubmit(WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const * commands) {}
void wgpuQueueWriteTexture(WGPUQueue queue, WGPUTexelCopyTextureInfo const * destination, void const * data, size_t dataSize, WGPUTexelCopyBufferLayout const * dataLayout, WGPUExtent3D const * writeSize) {}
void wgpuQueueAddRef(WGPUQueue queue) { choya_nulo::sumar(queue); }
void wgpuQueueRelease(WGPUQueue queue) { choya_nulo::soltar(queue); }
void wgpuRenderBundleSetLabel(WGPURenderBundle renderBundle, WGPUStringView label) {}
void wgpuRenderBundleAddRef(WGPURenderBundle renderBundle) { choya_nulo::sumar(renderBundle); }
void wgpuRenderBundleRelease(WGPURenderBundle renderBundle) { choya_nulo::soltar(renderBundle); }
void wgpuRenderBundleEncoderDraw(WGPURenderBundleEncoder renderBundleEncoder, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {}
void wgpuRenderBundleEncoderDrawIndexed(WGPURenderBundleEncoder renderBundleEncoder, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) {}
void wgpuRenderBundleEncoderDrawIndexedIndirect(WGPURenderBundleEncoder renderBundleEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset) {}
void wgpuRenderBundleEncoderDrawIndirect(WGPURenderBundleEncoder renderBundleEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset) {}
WGPURenderBundle wgpuRenderBundleEncoderFinish(WGPURenderBundleEncoder renderBundleEncoder, WGPU_NULLABLE WGPURenderBundleDescriptor const * descriptor) { return choya_nulo::nuevo<WGPURenderBundleImpl>(); }
void wgpuRenderBundleEncoderInsertDebugMarker(WGPURenderBundleEncoder renderBundleEncoder, WGPUStringView markerLabel) {}
void wgpuRenderBundleEncoderPopDebugGroup(WGPURenderBundleEncoder renderBundleEncoder) {}
void wgpuRenderBundleEncoderPushDebugGroup(WGPURenderBundleEncoder renderBundleEncoder, WGPUStringView groupLabel) {}
void wgpuRenderBundleEncoderSetBindGroup(WGPURenderBundleEncoder renderBundleEncoder, uint32_t groupIndex, WGPU_NULLABLE WGPUBindGroup group, size_t dynamicOffsetCount, uint32_t const * dynamicOffsets) {}
void wgpuRenderBundleEncoderSetImmediates(WGPURenderBundleEncoder renderBundleEncoder, uint32_t offset, void const * data, size_t size) {}
void wgpuRenderBundleEncoderSetIndexBuffer(WGPURenderBundleEncoder renderBundleEncoder, WGPUBuffer buffer, WGPUIndexFormat format, uint64_t offset, uint64_t size) {}
void wgpuRenderBundleEncoderSetLabel(WGPURenderBundleEncoder renderBundleEncoder, WGPUStringView label) {}
void wgpuRenderBundleEncoderSetPipeline(WGPURenderBundleEncoder renderBundleEncoder, WGPURenderPipeline pipeline) {}
void wgpuRenderBundleEncoderSetResourceTable(WGPURenderBundleEncoder renderBundleEncoder, WGPU_NULLABLE WGPUResourceTable table) {}
void wgpuRenderBundleEncoderSetVertexBuffer(WGPURenderBundleEncoder renderBundleEncoder, uint32_t slot, WGPU_NULLABLE WGPUBuffer buffer, uint64_t offset, uint64_t size) {}
void wgpuRenderBundleEncoderAddRef(WGPURenderBundleEncoder renderBundleEncoder) { choya_nulo::sumar(renderBundleEncoder); }
void wgpuRenderBundleEncoderRelease(WGPURenderBundleEncoder renderBundleEncoder) { choya_nulo::soltar(renderBundleEncoder); }
void wgpuRenderPassEncoderBeginOcclusionQuery(WGPURenderPassEncoder renderPassEncoder, uint32_t queryIndex) {}
void wgpuRenderPassEncoderDraw(WGPURenderPassEncoder renderPassEncoder, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {}
void wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder renderPassEncoder, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) {}
void wgpuRenderPassEncoderDrawIndexedIndirect(WGPURenderPassEncoder renderPassEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset) {}
void wgpuRenderPassEncoderDrawIndirect(WGPURenderPassEncoder renderPassEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset) {}
void wgpuRenderPassEncoderEnd(WGPURenderPassEncoder renderPassEncoder) {}
void wgpuRenderPassEncoderEndOcclusionQuery(WGPURenderPassEncoder renderPassEncoder) {}
void wgpuRenderPassEncoderExecuteBundles(WGPURenderPassEncoder renderPassEncoder, size_t bundleCount, WGPURenderBundle const * bundles) {}
void wgpuRenderPassEncoderInsertDebugMarker(WGPURenderPassEncoder renderPassEncoder, WGPUStringView markerLabel) {}
void wgpuRenderPassEncoderMultiDrawIndexedIndirect(WGPURenderPassEncoder renderPassEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset, uint32_t maxDrawCount, WGPU_NULLABLE WGPUBuffer drawCountBuffer, uint64_t drawCountBufferOffset) {}
void wgpuRenderPassEncoderMultiDrawIndirect(WGPURenderPassEncoder renderPassEncoder, WGPUBuffer indirectBuffer, uint64_t indirectOffset, uint32_t maxDrawCount, WGPU_NULLABLE WGPUBuffer drawCountBuffer, uint64_t drawCountBufferOffset) {}
void wgpuRenderPassEncoderPixelLocalStorageBarrier(WGPURenderPassEncoder renderPassEncoder) {}
void wgpuRenderPassEncoderPopDebugGroup(WGPURenderPassEncoder renderPassEncoder) {}
void wgpuRenderPassEncoderPushDebugGroup(WGPURenderPassEncoder renderPassEncoder, WGPUStringView groupLabel) {}
void wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder renderPassEncoder, uint32_t groupIndex, WGPU_NULLABLE WGPUBindGroup group, size_t dynamicOffsetCount, uint32_t const * dynamicOffsets) {}
void wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder renderPassEncoder, WGPUColor const * color) {}
void wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder renderPassEncoder, uint32_t offset, void const * data, size_t size) {}
void wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder renderPassEncoder, WGPUBuffer buffer, WGPUIndexFormat format, uint64_t offset, uint64_t size) {}
void wgpuRenderPassEncoderSetLabel(WGPURenderPassEncoder renderPassEncoder, WGPUStringView label) {}
void wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder renderPassEncoder, WGPURenderPipeline pipeline) {}
void wgpuRenderPassEncoderSetResourceTable(WGPURenderPassEncoder renderPassEncoder, WGPU_NULLABLE WGPUResourceTable table) {}
void wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder renderPassEncoder, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {}
void wgpuRenderPassEncoderSetStencilReference(WGPURenderPassEncoder renderPassEncoder, uint32_t reference) {}
void wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder renderPassEncoder, uint32_t slot, WGPU_NULLABLE WGPUBuffer buffer, uint64_t offset, uint64_t size) {}
void wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder renderPassEncoder, float x, float y, float width, float height, float minDepth, float maxDepth) {}
void wgpuRenderPassEncoderWriteTimestamp(WGPURenderPassEncoder renderPassEncoder, WGPUQuerySet querySet, uint32_t queryIndex) {}
void wgpuRenderPassEncoderAddRef(WGPURenderPassEncoder renderPassEncoder) { choya_nulo::sumar(renderPassEncoder); }
void wgpuRenderPassEncoderRelease(WGPURenderPassEncoder renderPassEncoder) { choya_nulo::soltar(renderPassEncoder); }
WGPUBindGroupLayout wgpuRenderPipelineGetBindGroupLayout(WGPURenderPipeline renderPipeline, uint32_t groupIndex) { return choya_nulo::nuevo<WGPUBindGroupLayoutImpl>(); }
void wgpuRenderPipelineSetLabel(WGPURenderPipeline renderPipeline, WGPUStringView label) {}
void wgpuRenderPipelineAddRef(WGPURenderPipeline renderPipeline) { choya_nulo::sumar(renderPipeline); }
void wgpuRenderPipelineRelease(WGPURenderPipeline renderPipeline) { choya_nulo::soltar(renderPipeline); }
void wgpuResourceTableDestroy(WGPUResourceTable resourceTable) {}
uint32_t wgpuResourceTableGetSize(WGPUResourceTable resourceTable) { return uint32_t{}; }
uint32_t wgpuResourceTableInsertBinding(WGPUResourceTable resourceTable, WGPUBindingResource const * resource) { return uint32_t{}; }
WGPUStatus wgpuResourceTableRemoveBinding(WGPUResourceTable resourceTable, uint32_t slot) { return WGPUStatus_Success; }
void wgpuResourceTableSetLabel(WGPUResourceTable resourceTable, WGPUStringView label) {}
WGPUStatus wgpuResourceTableUpdate(WGPUResourceTable resourceTable, uint32_t slot, WGPUBindingResource const * resource) { return WGPUStatus_Success; }
void wgpuResourceTableAddRef(WGPUResourceTable resourceTable) { choya_nulo::sumar(resourceTable); }
void wgpuResourceTableRelease(WGPUResourceTable resourceTable) { choya_nulo::soltar(resourceTable); }
void wgpuSamplerSetLabel(WGPUSampler sampler, WGPUStringView label) {}
void wgpuSamplerAddRef(WGPUSampler sampler) { choya_nulo::sumar(sampler); }
void wgpuSamplerRelease(WGPUSampler sampler) { choya_nulo::soltar(sampler); }
void wgpuShaderModuleSetLabel(WGPUShaderModule shaderModule, WGPUStringView label) {}
void wgpuShaderModuleAddRef(WGPUShaderModule shaderModule) { choya_nulo::sumar(shaderModule); }
void wgpuShaderModuleRelease(WGPUShaderModule shaderModule) { choya_nulo::soltar(shaderModule); }
WGPUStatus wgpuSharedBufferMemoryBeginAccess(WGPUSharedBufferMemory sharedBufferMemory, WGPUBuffer buffer, WGPUSharedBufferMemoryBeginAccessDescriptor const * descriptor) { return WGPUStatus_Success; }
WGPUBuffer wgpuSharedBufferMemoryCreateBuffer(WGPUSharedBufferMemory sharedBufferMemory, WGPU_NULLABLE WGPUBufferDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUBufferImpl>(); }
WGPUStatus wgpuSharedBufferMemoryEndAccess(WGPUSharedBufferMemory sharedBufferMemory, WGPUBuffer buffer, WGPUSharedBufferMemoryEndAccessState * descriptor) { return WGPUStatus_Success; }
WGPUStatus wgpuSharedBufferMemoryGetProperties(WGPUSharedBufferMemory sharedBufferMemory, WGPUSharedBufferMemoryProperties * properties) { return WGPUStatus_Success; }
WGPUBool wgpuSharedBufferMemoryIsDeviceLost(WGPUSharedBufferMemory sharedBufferMemory) { return WGPUBool{}; }
void wgpuSharedBufferMemorySetLabel(WGPUSharedBufferMemory sharedBufferMemory, WGPUStringView label) {}
void wgpuSharedBufferMemoryAddRef(WGPUSharedBufferMemory sharedBufferMemory) { choya_nulo::sumar(sharedBufferMemory); }
void wgpuSharedBufferMemoryRelease(WGPUSharedBufferMemory sharedBufferMemory) { choya_nulo::soltar(sharedBufferMemory); }
void wgpuSharedBufferMemoryEndAccessStateFreeMembers(WGPUSharedBufferMemoryEndAccessState sharedBufferMemoryEndAccessState) {}
void wgpuSharedFenceExportInfo(WGPUSharedFence sharedFence, WGPUSharedFenceExportInfo * info) {}
void wgpuSharedFenceSetLabel(WGPUSharedFence sharedFence, WGPUStringView label) {}
void wgpuSharedFenceAddRef(WGPUSharedFence sharedFence) { choya_nulo::sumar(sharedFence); }
void wgpuSharedFenceRelease(WGPUSharedFence sharedFence) { choya_nulo::soltar(sharedFence); }
WGPUStatus wgpuSharedTextureMemoryBeginAccess(WGPUSharedTextureMemory sharedTextureMemory, WGPUTexture texture, WGPUSharedTextureMemoryBeginAccessDescriptor const * descriptor) { return WGPUStatus_Success; }
WGPUTexture wgpuSharedTextureMemoryCreateTexture(WGPUSharedTextureMemory sharedTextureMemory, WGPU_NULLABLE WGPUTextureDescriptor const * descriptor) { return choya_nulo::nuevo<WGPUTextureImpl>(); }
WGPUStatus wgpuSharedTextureMemoryEndAccess(WGPUSharedTextureMemory sharedTextureMemory, WGPUTexture texture, WGPUSharedTextureMemoryEndAccessState * descriptor) { return WGPUStatus_Success; }
WGPUStatus wgpuSharedTextureMemoryGetProperties(WGPUSharedTextureMemory sharedTextureMemory, WGPUSharedTextureMemoryProperties * properties) { return WGPUStatus_Success; }
WGPUBool wgpuSharedTextureMemoryIsDeviceLost(WGPUSharedTextureMemory sharedTextureMemory) { return WGPUBool{}; }
void wgpuSharedTextureMemorySetLabel(WGPUSharedTextureMemory sharedTextureMemory, WGPUStringView label) {}
void wgpuSharedTextureMemoryAddRef(WGPUSharedTextureMemory sharedTextureMemory) { choya_nulo::sumar(sharedTextureMemory); }
void wgpuSharedTextureMemoryRelease(WGPUSharedTextureMemory sharedTextureMemory) { choya_nulo::soltar(sharedTextureMemory); }
void wgpuSharedTextureMemoryEndAccessStateFreeMembers(WGPUSharedTextureMemoryEndAccessState sharedTextureMemoryEndAccessState) {}
void wgpuSupportedFeaturesFreeMembers(WGPUSupportedFeatures supportedFeatures) {}
void wgpuSupportedInstanceFeaturesFreeMembers(WGPUSupportedInstanceFeatures supportedInstanceFeatures) {}
void wgpuSupportedWGSLLanguageFeaturesFreeMembers(WGPUSupportedWGSLLanguageFeatures supportedWGSLLanguageFeatures) {}
void wgpuSurfaceSetLabel(WGPUSurface surface, WGPUStringView label) {}
void wgpuSurfaceAddRef(WGPUSurface surface) { choya_nulo::sumar(surface); }
void wgpuSurfaceRelease(WGPUSurface surface) { choya_nulo::soltar(surface); }
void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities surfaceCapabilities) {}
void wgpuTexelBufferViewSetLabel(WGPUTexelBufferView texelBufferView, WGPUStringView label) {}
void wgpuTexelBufferViewAddRef(WGPUTexelBufferView texelBufferView) { choya_nulo::sumar(texelBufferView); }
void wgpuTexelBufferViewRelease(WGPUTexelBufferView texelBufferView) { choya_nulo::soltar(texelBufferView); }
void wgpuTextureDestroy(WGPUTexture texture) {}
void wgpuTexturePin(WGPUTexture texture, WGPUTextureUsage usage) {}
void wgpuTextureSetLabel(WGPUTexture texture, WGPUStringView label) {}
void wgpuTextureSetOwnershipForMemoryDump(WGPUTexture texture, uint64_t ownerGuid) {}
void wgpuTextureUnpin(WGPUTexture texture) {}
void wgpuTextureAddRef(WGPUTexture texture) { choya_nulo::sumar(texture); }
void wgpuTextureRelease(WGPUTexture texture) { choya_nulo::soltar(texture); }
void wgpuTextureViewSetLabel(WGPUTextureView textureView, WGPUStringView label) {}
void wgpuTextureViewAddRef(WGPUTextureView textureView) { choya_nulo::sumar(textureView); }
void wgpuTextureViewRelease(WGPUTextureView textureView) { choya_nulo::soltar(textureView); }
}  // extern "C"
