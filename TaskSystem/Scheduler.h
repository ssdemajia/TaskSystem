#pragma once
#include <cassert>
#include <atomic>
#include <vector>
#include <thread>
#include "Queue.h"

class FLowLevelTask;
class FLowLevelTask;

class FSchedulerTls
{
public:
	static thread_local FSchedulerTls* ActiveScheduler;
};

class FScheduler : public FSchedulerTls
{
public:
	static FScheduler& Get()
	{
		static FScheduler Scheduler;
		return Scheduler;
	}

	~FScheduler();

	bool TryLaunch(FLowLevelTask* Task, bool bWakeUpWorker);

	void StartWorkers(uint32_t NumWorkers);
	void StopWorkers();
private:
	FLowLevelTask* ExecuteTask(FLowLevelTask* InTask);

	void LaunchInternal(FLowLevelTask* Task, bool bWakeUpWorker);

	bool TryExecuteTaskFrom(FOverflowQueue<FLowLevelTask>* Queue);

	void WorkerMain();
private:
	// Worker线程数量
	std::atomic_uint NumActiveWorkers{ 0 };

	std::vector<std::thread> WorkerThreads;

	FOverflowQueue<FLowLevelTask> OverflowQueue;
};