#include "TaskSystem.h"
#include "Scheduler.h"
#include "Pipe.h"

thread_local FLowLevelTask* FLowLevelTask::ActiveTask = nullptr;
thread_local FTask* FTask::CurrentTask = nullptr;

bool FLowLevelTask::TryCancel()
{
	bool bTryLaunchOnCancelSuccess = true;  // 被cancel也要执行
	uintptr_t LocalPackedData = PackedData.load(std::memory_order_relaxed);
	uintptr_t ReadyState = ETaskState::Ready;
	uintptr_t ScheduledState = ETaskState::Scheduled;
	bool bWasCanceled = PackedData.compare_exchange_strong(ReadyState, ETaskState::Canceled, std::memory_order_acq_rel)
		|| PackedData.compare_exchange_strong(ScheduledState, ETaskState::Canceled, std::memory_order_acq_rel);

	if (bWasCanceled && bTryLaunchOnCancelSuccess && TryPrepareLaunch())
	{
		ExecuteTask();
	}
	return bWasCanceled;
}

FLowLevelTask* FLowLevelTask::ExecuteTask()
{

	ETaskState PreviousState = (ETaskState)PackedData.fetch_or(uintptr_t(ETaskState::Running), std::memory_order_acq_rel);
	assert((PreviousState & ETaskState::Scheduled) != 0);

	FLowLevelTask* ContinueTask = nullptr;

	if ((PreviousState & ETaskState::Running) == 0)
	{
		FTaskDelegate LocalDelegate;
		ContinueTask = Delegate.CallAndMove(LocalDelegate);
		PreviousState = (ETaskState)PackedData.fetch_or(uintptr_t(ETaskState::Completed), std::memory_order_seq_cst);
		assert((PreviousState & ETaskState::Running) != 0);
	}

	return ContinueTask;
}

void FTask::Init(const char* DebugName, EExtendedTaskPriority InExtendedTaskPriority)
{
	LowLevelTask.Init(DebugName, 
		[
			this,
			Deleter = TDeleter<FTask, &FTask::Release>{ this } // 用来释放scheduler的引用，这样FTask才能正确析构
		]() {
		TryExecuteTask();
	});
	ExtendedTaskPriority = InExtendedTaskPriority;
}

bool FTask::Wait(FTimeout Timeout)
{
	if (IsCompleted() || Timeout.IsExpired())
		return IsCompleted();

	return WaitImpl(Timeout);
}

bool FTask::AddPrerequisite(FTask& Prerequisite)
{
	// 需要Task还没执行
	assert(NumLocks.load(std::memory_order_relaxed) >= NumInitialLocks && NumLocks.load(std::memory_order_relaxed) < ExecutionFlag);

	uint32_t PrevNumLocks = NumLocks.fetch_add(1, std::memory_order_relaxed);

	if (!Prerequisite.AddSubsequent(*this))
	{
		NumLocks.fetch_sub(1, std::memory_order_relaxed);
		return false;
	}

	Prerequisite.AddRef();
	Prerequisites.Push(&Prerequisite);
	return true;
}

bool FTask::WaitImpl(FTimeout Timeout)
{
	while (true)
	{
		TryRetractAndExecute(Timeout);

		const uint32_t MaxSpinCount = 40;
		for (uint32_t SpinCount = 0; SpinCount != MaxSpinCount && !IsCompleted() && !Timeout.IsExpired(); ++SpinCount)
		{
			std::this_thread::yield();
		}

		if (IsCompleted() || Timeout.IsExpired())
			return IsCompleted();

	}
}

void FTask::AddNested(FTask& Nested)
{
	uint32_t PrevNumLocks = NumLocks.fetch_add(1, std::memory_order_relaxed);
	if (Nested.AddSubsequent(*this))
	{
		Nested.AddRef();
		Prerequisites.Push(&Nested);
	}
	else
	{
		NumLocks.fetch_sub(1, std::memory_order_relaxed);
	}
}

void FTask::Wait()
{
	WaitImpl(FTimeout::Never());
}

bool FTask::TryRetractAndExecute(FTimeout Timeout, uint32_t RecursionDepth/* = 0*/)
{
	if (IsCompleted() || Timeout.IsExpired())
	{
		return IsCompleted();
	}

	if (!IsAwaitable())
	{
		assert(0);  // dead lock
		return false;
	}

	// avoid stack overflow. is not expected in a real-life cases but happens in stress tests
	if (RecursionDepth == 200)
	{
		return false;
	}
	++RecursionDepth;

	// returns false if the task has passed "pre-scheduling" state: all (if any) prerequisites are completed
	auto IsLockedByPrerequisites = [this]
	{
		uint32_t LocalNumLocks = NumLocks.load(std::memory_order_relaxed); // the order doesn't matter as this "happens before" task execution
		return LocalNumLocks != 0 && LocalNumLocks < ExecutionFlag;
	};

	if (IsLockedByPrerequisites())
	{
		// try to unlock the task. even if (some or all) prerequisites retraction fails we still proceed to try helping with other prerequisites or this task execution

		// prerequisites are "consumed" here even if their retraction fails. this means that once prerequisite retraction failed, it won't be performed again. 
		// this can be potentially improved by using a different container for prerequisites
		for (FTask* Prerequisite : Prerequisites.PopAll())
		{
			// ignore if retraction failed, as this thread still can try to help with other prerequisites instead of being blocked in waiting
			Prerequisite->TryRetractAndExecute(Timeout, RecursionDepth);
			Prerequisite->Release();
		}
	}

	if (Timeout.IsExpired())
	{
		return IsCompleted();
	}

	{
		// next we try to execute the task, despite we haven't verified that the task is unlocked. trying to obtain execution permission will fail in this case

		if (!TryExecuteTask())
		{
			return false; // still locked by prerequisites, or another thread managed to set execution flag first, or we're inside this task execution
			// we could try to help with nested tasks execution (the task execution could already spawned a couple of nested tasks sitting in the queue). 
			// it's unclear how important this is, but this would definitely lead to more complicated impl. we can revisit this once we see such instances in profiler captures
		}
	}

	// the task was launched so the scheduler will handle the internal reference held by low-level task

	// retract nested tasks, if any
	{
		// keep trying retracting all nested tasks even if some of them fail, so the current worker can contribute instead of being blocked
		bool bSucceeded = true;
		// prerequisites are "consumed" here even if their retraction fails. this means that once prerequisite retraction failed, it won't be performed again. 
		// this can be potentially improved by using a different container for prerequisites
		for (FTask* Prerequisite : Prerequisites.PopAll())
		{
			if (!Prerequisite->TryRetractAndExecute(Timeout, RecursionDepth))
			{
				bSucceeded = false;
			}
			Prerequisite->Release();
		}

		if (!bSucceeded)
		{
			return false;
		}
	}

	// at this point the task is executed and has no pending nested tasks, but still can be "not completed" (nested tasks can be 
	// in the process of completing it (setting the flag) concurrently), so the caller still has to wait for completion
	return true;
}

bool FTask::TryUnlock(bool& bWakeUpWorker)
{
	FPipe* LocalPipe = GetPipe();
	uint32_t PrevNumLocks = NumLocks.fetch_sub(1, std::memory_order_acq_rel);

	uint32_t LocalNumLocks = PrevNumLocks - 1;
	if (PrevNumLocks < ExecutionFlag)
	{
		// 任务放入pipe时也加了ref
		bool bPrerequistesCompleted = LocalPipe == nullptr ? LocalNumLocks == 0 : LocalNumLocks <= 1;
		if (!bPrerequistesCompleted)
			return false;

		if (LocalPipe != nullptr)
		{
			bool bFirstPipingAttempt = LocalNumLocks == 1;
			if (bFirstPipingAttempt)
			{
				FTask* PrevPipedTask = LocalPipe->PushIntoPipe(*this);
				if (PrevPipedTask != nullptr)
				{
					Prerequisites.Push(PrevPipedTask);
					return false;
				}

				NumLocks.store(0, std::memory_order_release);
			}
		}

		if (ExtendedTaskPriority == EExtendedTaskPriority::TaskEvent)
		{
			if (TrySetExecutionFlag())
			{
				// task events are used as an empty prerequisites/subsequents
				ReleasePrerequisites();
				Close();
				ReleaseInternalReference();

				// Use-after-free territory, do not touch any of the task's properties here.
			}
		}
		else if (ExtendedTaskPriority == EExtendedTaskPriority::Inline)
		{
			// 直接运行
			TryExecuteTask();
			ReleaseInternalReference();
		}
		else
		{
			Schedule(bWakeUpWorker);
		}
		return true;
	}

	if (LocalNumLocks != ExecutionFlag)
		return false;

	Close();
	Release();
	return true;
}


void FTask::Schedule(bool& bWakeUpWorker)
{
	bWakeUpWorker |= FScheduler::Get().TryLaunch(&LowLevelTask, bWakeUpWorker);
}

void FTask::ClearPipe()
{
	GetPipe()->ClearTask(*this);
}

bool FTask::TryExecuteTask()
{
	if (!TrySetExecutionFlag())
	{
		return false;
	}

	AddRef();
	
	ReleasePrerequisites();

	FTask* PrevTask = ExchangeCurrentTask(this);

	ExecutingThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
	if (GetPipe() != nullptr)
	{
		GetPipe()->ExecutionStarted();
	}

	ExecuteTask();

	if (GetPipe() != nullptr)
	{
		GetPipe()->ExecutionFinished();
	}
	ExecutingThreadId.store(std::thread::id(), std::memory_order_relaxed);

	ExchangeCurrentTask(PrevTask);

	uint32_t LocalNumLocks = NumLocks.fetch_sub(1, std::memory_order_acq_rel) - 1;
	if (LocalNumLocks == ExecutionFlag)
	{
		Close();
		Release();
	}
	return true;
}
