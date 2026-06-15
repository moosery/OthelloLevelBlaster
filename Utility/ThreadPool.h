#pragma once
#include<vector>
#include<queue>
#include<functional>
#include "Mem.h"
#include <memory.h>
#include <string>
#include <thread>
#include <mutex>

#define MAX_QUEUE_DEPTH 5000

class ThreadPool
{
public:
    ThreadPool(uint32_t numberThreadsNeeded, std::string threadName)
    {
        isStarted = false;
        num_threads = numberThreadsNeeded;
        m_threadName = threadName;
        num_running = 0;
    }

    ~ThreadPool() { if (isStarted) Stop(); }
    void Start();
    void QueueJob(const std::function<void(uint32_t)>& job);
    void Stop();
    bool IsBusy();
    int NumIdle();
    bool IsStarted();

    uint32_t NumThreads()
    {
        return num_running;
    }
    size_t QueueDepth();

private:
    std::string m_threadName;
    void ThreadLoop(uint32_t idx);
    int numIdle = 0;
    uint32_t num_threads;
    bool* isBusyArray = NULL;
    bool isStarted;
    uint32_t num_running;

    bool should_terminate = false;          // Tells threads to stop looking for jobs
    std::mutex queue_mutex;                 // Prevents data races to the job queue
    std::condition_variable mutex_condition;     // Allows threads to wait on new jobs or termination
    std::condition_variable queue_not_full;      // Allows producers to wait when queue is at MAX_QUEUE_DEPTH
    std::vector<std::thread> threads;
    std::queue<std::function<void(uint32_t)>> jobs;
};