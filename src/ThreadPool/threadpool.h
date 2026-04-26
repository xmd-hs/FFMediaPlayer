/**
 * @file threadpool.h
 * @brief 高性能动态线程池，支持 Cached/Fixed 双模式
 *
 * ThreadPool 提供两种工作模式：
 * - MODE_FIXED:  固定线程数模式，线程在 start() 时全部创建，适用于负载稳定的场景
 * - MODE_CACHED: 动态线程数模式，根据任务负载动态创建/回收线程，适用于突发负载场景
 *
 * 核心特性：
 * - 基于 C++11 标准，纯头文件/源文件实现，无第三方依赖
 * - 支持 Future/Promise 模式，通过 submitTask() 获取任务返回值
 * - 任务队列带容量限制，防止内存过度占用
 * - Cached 模式下空闲线程超时自动回收（60秒）
 * - 优雅关闭：stop() 等待所有任务完成后退出
 * - 线程安全：所有公共接口均为线程安全
 *
 * 使用示例：
 * @code
 *   ThreadPool pool;
 *   pool.setMode(PoolMode::MODE_CACHED);
 *   pool.start(4);
 *   auto future = pool.submitTask([]() { return 42; });
 *   int result = future.get();
 *   pool.stop();
 * @endcode
 *
 * @author FFMediaPlayer Team
 * @version 2.0
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

/**
 * @brief 线程池工作模式枚举
 */
enum class PoolMode
{
    MODE_FIXED,  ///< 固定线程数模式：线程数在 start() 时确定，运行期间不变
    MODE_CACHED  ///< 动态线程数模式：根据任务队列负载动态增减线程数量
};

/**
 * @brief 高性能线程池类
 *
 * 支持 Cached/Fixed 双模式，提供 Future/Promise 任务提交、
 * 任务队列容量限制、线程数量限制、优雅关闭等功能。
 *
 * Fixed 模式下线程数量固定，适用于可预测的负载；
 * Cached 模式下线程数量动态调整，空闲线程超时自动回收，
 * 任务繁忙时自动扩容（不超过 threadSizeThreshHold_）。
 */
class ThreadPool
{
public:
    ThreadPool();
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 设置线程池工作模式
     * @param mode 目标工作模式
     * @note 仅在线程池未启动时设置有效
     */
    void setMode(PoolMode mode);

    /**
     * @brief 设置任务队列最大容量阈值
     * @param threshhold 最大任务数量
     * @note 仅在线程池未启动时设置有效，默认 1024
     */
    void setTaskQueMaxThreshHold(int threshhold);

    /**
     * @brief 设置 Cached 模式下的最大线程数量阈值
     * @param threshhold 最大线程数
     * @note 仅对 MODE_CACHED 模式生效，且仅在线程池未启动时设置有效
     */
    void setThreadSizeThreshHold(int threshhold);

    /**
     * @brief 向线程池提交任务（可变参模板）
     * @tparam Func 可调用对象类型
     * @tparam Args 参数类型
     * @param func 任务函数
     * @param args 函数参数
     * @return std::future 用于获取任务返回值
     *
     * 若任务队列已满（等待1秒后仍无空间），返回一个默认值的 future。
     * Cached 模式下，若任务数超过空闲线程数且未达线程上限，将自动创建新线程。
     */
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>;

    /**
     * @brief 启动线程池
     * @param initThreadSize 初始线程数量，默认为 CPU 核心数
     *
     * 创建 initThreadSize 个工作线程，所有线程立即开始等待任务。
     */
    void start(int initThreadSize = std::thread::hardware_concurrency());

    /**
     * @brief 优雅关闭线程池
     *
     * 设置停止标志，通知所有线程退出，等待所有线程结束。
     * 已提交但未执行的任务将被丢弃。
     */
    void stop();

    /**
     * @brief 查询线程池是否正在运行
     * @return true 表示线程池正在运行
     */
    bool checkRunningState() const;

private:
    using Task = std::function<void()>;
    using ThreadFunc = std::function<void(int)>;

    /**
     * @brief 线程工作循环主函数
     * @param threadid 线程 ID
     *
     * Fixed 模式：线程持续等待任务，直到线程池停止。
     * Cached 模式：线程在等待超时后自动退出（当线程数超过初始数量时）。
     */
    void threadFunc(int threadid);

    /**
     * @brief 提交任务辅助函数
     * @param task 待执行的任务
     * @return true 表示提交成功，false 表示任务队列已满
     */
    bool submitTaskHelper(Task task);

    /**
     * @brief 内部线程包装类
     *
     * 封装线程函数和线程 ID，使用 detach 方式运行。
     * 线程 ID 由静态计数器自动分配。
     */
    struct Thread
    {
        ThreadFunc func_;
        static int generateId_;
        int threadId_;

        Thread(ThreadFunc func)
            : func_(func)
            , threadId_(generateId_++)
        {}

        void start()
        {
            std::thread t(func_, threadId_);
            t.detach();
        }

        int getId() const { return threadId_; }
    };

    std::unordered_map<int, std::unique_ptr<Thread>> threads_;  ///< 工作线程集合

    int initThreadSize_;                  ///< 初始线程数量
    int threadSizeThreshHold_;            ///< Cached 模式最大线程数量
    std::atomic_int curThreadSize_;       ///< 当前存活线程数量
    std::atomic_int idleThreadSize_;      ///< 当前空闲线程数量

    std::queue<Task> taskQue_;            ///< 任务队列
    std::atomic_int taskSize_;            ///< 任务队列当前大小
    int taskQueMaxThreshHold_;            ///< 任务队列最大容量

    std::mutex taskQueMtx_;               ///< 任务队列互斥锁
    std::condition_variable notFull_;     ///< 任务队列非满条件变量
    std::condition_variable notEmpty_;    ///< 任务队列非空条件变量
    std::condition_variable exitCond_;    ///< 线程退出条件变量

    PoolMode poolMode_;                   ///< 当前工作模式
    std::atomic_bool isPoolRunning_;      ///< 线程池运行标志

    static const int TASK_MAX_THRESHHOLD = 1024;   ///< 任务队列默认最大容量
    static const int THREAD_MAX_THRESHHOLD = 1024; ///< 线程数默认最大阈值
    static const int THREAD_MAX_IDLE_TIME = 60;    ///< Cached 模式空闲线程最大等待时间（秒）
};

template<typename Func, typename... Args>
auto ThreadPool::submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
{
    using RType = decltype(func(args...));
    auto task = std::make_shared<std::packaged_task<RType()>>(
        std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
    std::future<RType> result = task->get_future();

    bool success = submitTaskHelper([task]() { (*task)(); });
    if (!success)
    {
        auto fallback = std::make_shared<std::packaged_task<RType()>>(
            []()->RType { return RType(); });
        (*fallback)();
        return fallback->get_future();
    }

    return result;
}

#endif // THREADPOOL_H
