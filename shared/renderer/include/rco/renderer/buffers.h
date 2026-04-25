#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include <glad/glad.h>
#include "rco/renderer/utilities.h"

namespace rco::renderer {

class StaticBuffer {
public:
    StaticBuffer(const void* data, size_t size, uint32_t glflags);
    StaticBuffer(StaticBuffer&& other) noexcept;
    ~StaticBuffer();

    StaticBuffer(const StaticBuffer&)            = delete;
    StaticBuffer& operator=(const StaticBuffer&) = delete;
    StaticBuffer& operator=(StaticBuffer&&)      = delete;
    bool operator==(const StaticBuffer&) const = default;

    void SubData(const void* data, size_t size, size_t offset);
    void Bind(uint32_t target);
    void BindBase(uint32_t target, uint32_t index);
    uint32_t ID() { return id_; }

private:
    uint32_t id_ = 0;
};

class DynamicBuffer {
public:
    struct allocationData {
        uint64_t handle {};
        double   time   {};
        uint32_t flags  {};
        uint32_t _pad   {};
        uint32_t offset {};
        uint32_t size   {};
    };

    DynamicBuffer(uint32_t size, uint32_t alignment);
    ~DynamicBuffer();

    uint64_t Allocate(const void* data, size_t size);
    bool     Free(uint64_t handle);
    void     Clear();
    uint64_t FreeOldest();

    const allocationData& GetAlloc(uint64_t handle);
    const std::vector<allocationData>& GetAllocs() { return allocs_; }
    GLuint   ActiveAllocs()    { return numActiveAllocs_; }
    GLuint   GetBufferHandle() { return buffer; }
    std::pair<uint64_t, GLuint> GetStateInfo() { return { nextHandle, numActiveAllocs_ }; }

    const GLsizei align_;
    constexpr size_t AllocSize() const { return sizeof(allocationData); }

protected:
    std::vector<allocationData> allocs_;
    using Iterator = decltype(allocs_.begin());

    void stateChanged();
    void maybeMerge(Iterator it);

    GLuint   buffer           {};
    uint64_t nextHandle       { 1 };
    GLuint   numActiveAllocs_ { 0 };
    const GLuint capacity_;
    Timer timer;
};

} // namespace rco::renderer
