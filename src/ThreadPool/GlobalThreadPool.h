#pragma once

#include "threadpool.h"
#include <mutex>

class GlobalThreadPool
{
public:
    static ThreadPool& Instance()
    {
        static ThreadPool pool;
        static std::once_flag flag;
        std::call_once(flag, []() {
            pool.setMode(PoolMode::MODE_CACHED);
            pool.setThreadSizeThreshHold(16);
            pool.setTaskQueMaxThreshHold(2048);
            pool.start(std::max(4u, std::thread::hardware_concurrency() / 2));
        });
        return pool;
    }

    GlobalThreadPool() = delete;
};
