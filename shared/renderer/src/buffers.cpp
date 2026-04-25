#include "rco/renderer/buffers.h"
#include <algorithm>

namespace rco::renderer {

// === StaticBuffer ===
StaticBuffer::StaticBuffer(const void* data, size_t size, uint32_t glflags) {
    glCreateBuffers(1, &id_);
    glNamedBufferStorage(id_, static_cast<GLsizeiptr>(size), data,
                         static_cast<GLbitfield>(glflags));
}

StaticBuffer::StaticBuffer(StaticBuffer&& other) noexcept {
    id_ = std::exchange(other.id_, 0);
}

StaticBuffer::~StaticBuffer() { glDeleteBuffers(1, &id_); }

void StaticBuffer::Bind(uint32_t target)                     { glBindBuffer(target, id_); }
void StaticBuffer::BindBase(uint32_t target, uint32_t index) {
    glBindBuffer(target, id_);
    glBindBufferBase(target, index, id_);
}
void StaticBuffer::SubData(const void* data, size_t size, size_t offset) {
    glNamedBufferSubData(id_, static_cast<GLintptr>(offset),
                         static_cast<GLsizeiptr>(size), data);
}

// === DynamicBuffer ===
DynamicBuffer::DynamicBuffer(uint32_t size, uint32_t alignment)
    : align_(alignment), capacity_(size) {
    size += (align_ - (size % align_)) % align_;
    glCreateBuffers(1, &buffer);
    glNamedBufferStorage(buffer, std::max(uint32_t(1), size), nullptr, GL_DYNAMIC_STORAGE_BIT);
    Clear();
}

DynamicBuffer::~DynamicBuffer() {
    glDeleteBuffers(1, &buffer);
}

uint64_t DynamicBuffer::Allocate(const void* data, size_t size) {
    size += (align_ - (size % align_)) % align_;
    Iterator small = allocs_.end();
    for (int i = 0; i < (int)allocs_.size(); i++) {
        if (allocs_[i].handle == 0 && allocs_[i].size >= size) {
            if (small == allocs_.end())
                small = allocs_.begin() + i;
            else if (allocs_[i].size < small->size)
                small = allocs_.begin() + i;
        }
    }
    if (small == allocs_.end()) return 0;

    allocationData newAlloc {
        .handle = nextHandle++,
        .time   = timer.elapsed(),
        .flags  = 0,
        .offset = small->offset,
        .size   = static_cast<uint32_t>(size),
    };

    small->offset += newAlloc.size;
    small->size   -= newAlloc.size;

    if (small->size == 0)
        *small = newAlloc;
    else
        allocs_.insert(small, newAlloc);

    glNamedBufferSubData(buffer, newAlloc.offset, newAlloc.size, data);
    ++numActiveAllocs_;
    stateChanged();
    return newAlloc.handle;
}

bool DynamicBuffer::Free(uint64_t handle) {
    if (handle == 0) return false;
    auto it = std::find_if(allocs_.begin(), allocs_.end(),
                           [&](const auto& a) { return a.handle == handle; });
    if (it == allocs_.end()) return false;

    it->handle = 0;
    maybeMerge(it);
    --numActiveAllocs_;
    stateChanged();
    return true;
}

void DynamicBuffer::Clear() {
    numActiveAllocs_ = 0;
    allocs_.clear();
    allocationData falloc {
        .handle = 0,
        .time   = 0,
        .offset = 0,
        .size   = capacity_,
    };
    allocs_.push_back(falloc);
}

uint64_t DynamicBuffer::FreeOldest() {
    Iterator old = allocs_.end();
    for (int i = 0; i < (int)allocs_.size(); i++) {
        if (allocs_[i].handle != 0) {
            if (old == allocs_.end())
                old = allocs_.begin() + i;
            else if (allocs_[i].time < old->time)
                old = allocs_.begin() + i;
        }
    }
    if (old == allocs_.end()) return 0;

    auto retval = old->handle;
    old->handle = 0;
    maybeMerge(old);
    --numActiveAllocs_;
    stateChanged();
    return retval;
}

void DynamicBuffer::stateChanged() {}

void DynamicBuffer::maybeMerge(Iterator it) {
    bool removeIt   = false;
    bool removeNext = false;

    if (it != allocs_.end() - 1) {
        Iterator next = it + 1;
        if (next->handle == 0) {
            it->size += next->size;
            removeNext = true;
        }
    }

    if (it != allocs_.begin()) {
        Iterator prev = it - 1;
        if (prev->handle == 0) {
            prev->size += it->size;
            removeIt = true;
        }
    }

    if (removeIt && removeNext)
        allocs_.erase(it, it + 2);
    else if (removeIt)
        allocs_.erase(it);
    else if (removeNext)
        allocs_.erase(it + 1);
}

const DynamicBuffer::allocationData& DynamicBuffer::GetAlloc(uint64_t handle) {
    return *std::find_if(allocs_.begin(), allocs_.end(),
                         [=](const auto& a) { return a.handle == handle; });
}

} // namespace rco::renderer
