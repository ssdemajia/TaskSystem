#include "Scheduler.h"
#include "TaskSystem.h"

thread_local FSchedulerTls* FSchedulerTls::ActiveScheduler = nullptr;

FScheduler::~FScheduler()
{
	for (int32_t Index = 0; Index < WorkerThreads.size(); Index += 1)
	{
		std::thread& t = WorkerThreads[Index];
	
		if (t.joinable()) {
			t.join();    // 等待线程结束
		}
	}
	
}

bool FScheduler::TryLaunch(FLowLevelTask* Task, bool bWakeUpWorker)
{
	if (Task->TryPrepareLaunch())
	{
		LaunchInternal(Task, bWakeUpWorker);
		return true;
	}
	return false;
}

void FScheduler::StartWorkers(uint32_t NumWorkers)
{
	NumActiveWorkers.store(NumWorkers, std::memory_order_relaxed);

	for (uint32_t Index = 0; Index < NumWorkers; Index += 1)
	{
		std::thread t([this]() {
			WorkerMain();
		});
		WorkerThreads.push_back(std::move(t));
	}
}

void FScheduler::StopWorkers()
{
	uint32_t OldActiveWorkers = NumActiveWorkers.load(std::memory_order_relaxed);
	if (OldActiveWorkers != 0 && NumActiveWorkers.compare_exchange_strong(OldActiveWorkers, 0, std::memory_order_relaxed))
	{

	}
}


void FScheduler::WorkerMain()
{
	FSchedulerTls::ActiveScheduler = this;
	bool bPreparingWait = false;
	while (true)
	{
		bool bExecutedSomething = false;
		while (TryExecuteTaskFrom(&OverflowQueue))
		{
			bPreparingWait = false;
			bExecutedSomething = true;
		}
		if (NumActiveWorkers.load(std::memory_order_relaxed) == 0)
		{
			break;
		}

	}

	FSchedulerTls::ActiveScheduler = nullptr;
}

bool FScheduler::TryExecuteTaskFrom(FOverflowQueue<FLowLevelTask>* Queue)
{
	bool AnyExecuted = false;
	FLowLevelTask* Task = Queue->dequeue();
	while (Task)
	{
		AnyExecuted = true;

		// Executing a task can return a continuation.
		if ((Task = ExecuteTask(Task)) != nullptr)
		{
			assert(Task->TryPrepareLaunch());
		}

	}
	return AnyExecuted;
}

FLowLevelTask* FScheduler::ExecuteTask(FLowLevelTask* InTask)
{
	FLowLevelTask* ParentTask = FLowLevelTask::ActiveTask;
	FLowLevelTask::ActiveTask = InTask;
	FLowLevelTask* OutTask;

	OutTask = InTask->ExecuteTask();

	FLowLevelTask::ActiveTask = ParentTask;
	return OutTask;
}

void FScheduler::LaunchInternal(FLowLevelTask* Task, bool bWakeUpWorker)
{
	if (NumActiveWorkers.load(std::memory_order_acquire) > 0)
	{
		OverflowQueue.enqueue(Task);
	}
	else
	{
		FLowLevelTask* TaskPtr = Task;
		while (TaskPtr)
		{
			if ((TaskPtr = ExecuteTask(TaskPtr)) != nullptr)
			{
				assert(TaskPtr->TryPrepareLaunch());
			}
		}
	}
}