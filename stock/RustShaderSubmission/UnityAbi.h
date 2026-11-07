#pragma once
#include <cstddef>
#include <cstdint>

namespace ShaderSubmission {
template<class T> struct Array {
    T *data;
    uint64_t label;
    size_t size, capacityTag;
};
struct PropertySheet {
    uint64_t header[2]; int32_t starts[7]; uint32_t alignment;
    Array<uint32_t> names, descriptions; Array<uint8_t> values;
    uint32_t hash, layoutHash; uint8_t allowAdd, padding[7];
};
struct ValueParameter {
    uint32_t name; int32_t offset, arraySize, scalarType;
    uint8_t rows, columns, flags, padding;
};
struct TextureParameter {
    uint32_t name; int32_t textureUnit, samplerUnit;
    uint8_t multisampled, padding[3]; int32_t dimension;
};
struct BufferParameter { uint32_t name; int32_t binding; uint8_t padding[8]; };
struct ConstantParameter {
    uint32_t name, padding; Array<ValueParameter> values; Array<void> structures;
    int32_t size, binding, kind; uint32_t floats, vectors, matrices; void *handle;
};
struct ProgramParameters {
    uint32_t floats, vectors, matrices, maximumBytes;
    Array<ValueParameter> values; Array<TextureParameter> textures; Array<void> uavs;
    Array<BufferParameter> buffers; Array<void> samplers; Array<ConstantParameter> constants;
    Array<void> images, atomicBuffers; uint8_t builtinMatrices[624];
};
struct ConstantBuffer {
    uint8_t dirty, bound, padding[6]; uint8_t *data; void *deviceBuffer;
    uint32_t size, alignment; ConstantBuffer *stereo[2];
};
struct Applier {
    ConstantBuffer *constant; uint8_t *device; int32_t stage; uint32_t padding;
    const Array<ConstantParameter> *parameters;
};
struct TextureBinding { int32_t textureUnit, samplerUnit, multisampled, texture; };
struct Api {
    void (*resize)(Array<uint8_t> *, size_t, uint32_t);
    void (*texture)(const uint8_t *, int32_t, int32_t, uint8_t, TextureBinding *);
    void (*prepare)(const ProgramParameters *, const PropertySheet *, Array<uint8_t> *);
    uint8_t *(*realDevice)();
    const uint8_t *(*applyTextures)(const uint8_t *, uint8_t *, int32_t);
    const uint8_t *(*applySamplers)(const uint8_t *, uint8_t *, int32_t);
    void (*applyConstant)(Applier *, const uint8_t *);
    const void *shaderApply;
};
static_assert(sizeof(Array<uint8_t>) == 32 && sizeof(PropertySheet) == 160);
static_assert(sizeof(ValueParameter) == 20 && sizeof(TextureParameter) == 20 && sizeof(BufferParameter) == 16);
static_assert(sizeof(ConstantParameter) == 104 && sizeof(ProgramParameters) == 896);
static_assert(sizeof(ConstantBuffer) == 48 && sizeof(Applier) == 32);
static_assert(offsetof(ProgramParameters, constants) == 176);
extern "C" int KoeRustPrepare(const ProgramParameters *, const PropertySheet *, Array<uint8_t> *, const Api *);
extern "C" const uint8_t *KoeRustApplyValues(const Array<ValueParameter> *, const uint8_t *, const uint8_t *, ConstantBuffer *);
extern "C" int KoeRustTrySubmit(uint8_t *, const PropertySheet *, const Api *, uint32_t);
extern "C" void KoeRustInitializeParallel(size_t,size_t);
extern "C" void KoeRustParallelInfo(uint64_t *,size_t);
}
