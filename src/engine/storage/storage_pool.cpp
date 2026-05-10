#include "engine/storage/storage.hpp"
#include "engine/buffer_pool.hpp"

namespace db {

void Storage::flushAllPools() const {
    std::lock_guard<std::mutex> lock(pools_latch_);
    for (auto& [_, pool] : pools_) {
        if (pool) pool->flushAll();
    }
}

BufferPool& Storage::getPool(const std::string& path) const {
    std::lock_guard<std::mutex> lock(pools_latch_);
    if (pools_.find(path) == pools_.end()) {
        pools_[path] = std::make_unique<BufferPool>(path, POOL_SIZE, wal_mgr_.get());
    }
    return *pools_[path];
}

void Storage::closePool(const std::string& path) const {
    std::lock_guard<std::mutex> lock(pools_latch_);
    pools_.erase(path);
}

} // namespace db
