#include"ThreadPool.h"
#include <Windows.h>
#include <codecvt>
using namespace std;

//#define THREADPOOL_VERBOSE

void ThreadPool::ThreadLoop(uint32_t thdIdx)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wide = converter.from_bytes(m_threadName);
    LPCWSTR result = wide.c_str();
    HRESULT r = SetThreadDescription(GetCurrentThread(), result);

    while (true)
    {
        isBusyArray[thdIdx] = false;
        function<void(uint32_t)> job;
        {
            unique_lock<mutex> lock(queue_mutex);
            mutex_condition.wait
            (lock, [this]
                {
                    return !jobs.empty() || should_terminate;
                }
            );
            if (should_terminate)
            {
                return;
            }
            isBusyArray[thdIdx] = true;
            job = jobs.front();
            jobs.pop();
        }
        queue_not_full.notify_one();

        job(thdIdx);
    }

}

int ThreadPool::NumIdle()
{
    int idleCnt = 0;
    if (!isStarted)
        return idleCnt;

    for (uint32_t idx = 0; idx < num_running; idx++)
    {
        if (!isBusyArray[idx])
            idleCnt++;
    }

    return idleCnt;

}

bool ThreadPool::IsStarted()
{
    return isStarted;
}


void ThreadPool::Start()
{
    isBusyArray = (bool*)MemMalloc((char*)"bool.Array.ThreadPool", sizeof(bool) * num_threads);
    memset(isBusyArray, 0, sizeof(bool) * num_threads);

    threads.resize(num_threads);
    for (uint32_t i = 0; i < num_threads; i++)
    {
        threads.at(i) = thread([this, i] { this->ThreadLoop(i); });
        num_running++;
    }
    isStarted = true;
}

void ThreadPool::QueueJob(const std::function<void(uint32_t)>& job)
{
    {
        unique_lock<mutex> lock(queue_mutex);
        queue_not_full.wait(lock, [this] { return jobs.size() < MAX_QUEUE_DEPTH || should_terminate; });
        if (should_terminate)
            return;
        jobs.push(job);
    }
    mutex_condition.notify_one();
}

size_t ThreadPool::QueueDepth()
{
    size_t result = 0;

    {
        unique_lock<mutex> lock(queue_mutex);

        result = jobs.size();
    }

    return result;
}

bool ThreadPool::IsBusy()
{
    bool poolbusy;
    if (!isStarted)
        return false;

    {
        unique_lock<mutex> lock(queue_mutex);
        int numIdle = NumIdle();
        poolbusy = !jobs.empty();

        if (!poolbusy)
        {
            if (numIdle != num_threads)
                poolbusy = true;
        }
    }
    return poolbusy;
}

void ThreadPool::Stop()
{
#ifdef THREADPOOL_VERBOSE
    printf("Stopping thread pool:\n");
    fflush(stdout);
#endif
    isStarted = false;
    {
#ifdef THREADPOOL_VERBOSE
        printf("Popping jobs: \n");
        fflush(stdout);
#endif
        unique_lock<mutex> lock(queue_mutex);
        should_terminate = true;
        while (!jobs.empty()) jobs.pop();
    }
    mutex_condition.notify_all();
    queue_not_full.notify_all();

#ifdef THREADPOOL_VERBOSE
    printf("Joining threads: \n");
    fflush(stdout);
#endif
    for (thread& active_thread : threads) {
        active_thread.join();
        num_running--;
    }
    threads.clear();
    if (isBusyArray)
    {
        MemFree(isBusyArray);
        isBusyArray = NULL;
    }
#ifdef THREADPOOL_VERBOSE
    printf("Done Stopping: \n");
    fflush(stdout);
#endif
}
