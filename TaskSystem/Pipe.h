#pragma once
#include <atomic>
#include <cassert>

#include "TaskSystem.h"

class FPipe
{
public:
	FPipe(const char* InDebugName)
		: DebugName(InDebugName)
	{ }

	~FPipe()
	{
		assert(!HasWork());
	}
	bool HasWork() const
	{
		return TaskCount.load(std::memory_order_relaxed) != 0;
	}
	bool WaitUntilEmpty(std::chrono::system_clock::duration Timeout = std::chrono::system_clock::duration::max());

	template<typename TaskBodyType>
	FTaskHandle Launch(
		const char* InDebugName,
		TaskBodyType&& TaskBody,
		EExtendedTaskPriority InExtendedTaskPriority = EExtendedTaskPriority::None
		)
	{
		
		TExecutableTask<TaskBodyType>* Task = new TExecutableTask(InDebugName, InExtendedTaskPriority, std::forward<TaskBodyType>(TaskBody));
		TaskCount.fetch_add(1, std::memory_order_acq_rel);
		Task->SetPipe(*this);
		Task->TryLaunch();
		return FTaskHandle(Task);
	}

	template<typename TaskBodyType, typename PrerequisitesCollectionType>
	FTaskHandle Launch(
		const char* InDebugName,
		TaskBodyType&& TaskBody,
		PrerequisitesCollectionType&& Prerequisites,
		EExtendedTaskPriority InExtendedTaskPriority = EExtendedTaskPriority::None
	)
	{
		TExecutableTask<TaskBodyType>* Task = new TExecutableTask(InDebugName, InExtendedTaskPriority, std::forward<TaskBodyType>(TaskBody));
		TaskCount.fetch_add(1, std::memory_order_acq_rel);
		Task->AddPrerequisites(Prerequisites);
		Task->SetPipe(*this);
		Task->TryLaunch();
		return FTaskHandle(Task);
	}

	// checks if pipe's task is being executed by the current thread. Allows to check if accessing a resource protected by a pipe
	// is thread-safe
	bool IsInContext() const;
private:
	friend class FTask;
	FTask* PushIntoPipe(FTask& Task);
	void ClearTask(FTask& Task);
	// notifications about pipe's task execution
	void ExecutionStarted();
	void ExecutionFinished();
private:
	const char* DebugName;
	std::atomic_uint64_t TaskCount{ 0 };
	std::atomic<FTask*> LastTask{ nullptr };
	std::condition_variable EmptyCV;
	std::mutex EmptyMtx;
};