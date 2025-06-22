#ifndef BUFFERPOOL_H
#define BUFFERPOOL_H

#include <vector>
#include <mutex>

class BufferPool {
public:
    // Construct a pool that creates buffers of 'bufferSize' bytes.
    // 'poolSize' is the initial number of buffers to pre‑allocate.
    BufferPool(std::size_t bufferSize, std::size_t poolSize)
        : bufferSize_(bufferSize)
    {
        for (std::size_t i = 0; i < poolSize; ++i) {
            pool_.push_back(new char[bufferSize_]);
        }
    }

    ~BufferPool() {
        for (char* buf : pool_) {
            delete[] buf;
        }
    }

    // Acquire a buffer from the pool (or allocate a new one if pool is empty).
    char* acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.empty()) {
            return new char[bufferSize_]; // Fallback allocation.
        }
        char* buf = pool_.back();
        pool_.pop_back();
        return buf;
    }

    // Release a buffer back to the pool.
    void release(char* buf) {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push_back(buf);
    }

    // Get the constant size of buffers in this pool.
    std::size_t buffer_size() const {
        return bufferSize_;
    }

private:
    std::size_t bufferSize_;
    std::vector<char*> pool_;
    mutable std::mutex mutex_;
};

#endif // BUFFERPOOL_H
