#include "Pipe.h"

bool FPipe::WaitUntilEmpty(std::chrono::system_clock::duration InTimeout)
{
	if (TaskCount.load(std::memory_order_relaxed) == 0)
		return true;

	FTimeout Timeout(InTimeout);
	while (true)
	{
		if (TaskCount.load(std::memory_order_relaxed) == 0)
		{
			return true;
		}

		if (Timeout.IsExpired())
		{
			return false;
		}

		//UE::FEventCountToken Token = EmptyEvent.PrepareWait();

		if (TaskCount.load(std::memory_order_relaxed) == 0)
		{
			return true;
		}
		std::unique_lock lock(EmptyMtx);
		if (EmptyCV.wait_for(lock, Timeout.GetRemainingTime()) == std::cv_status::timeout)
		{
			break;
		}
		//if (!EmptyEvent.WaitFor(Token, UE::FMonotonicTimeSpan::FromMilliseconds(Timeout.GetRemainingRoundedUpMilliseconds())))
		//{
		//	break;
		//}
	}

	return false;
}

FTask* FPipe::PushIntoPipe(FTask& Task)
{
	Task.AddRef();
	FTask* LastTask_Local = LastTask.exchange(&Task, std::memory_order_acq_rel);
	assert(LastTask_Local != &Task); // Dependency cycle : adding itself as a prerequisite(or use after destruction)

	if (LastTask_Local == nullptr)
		return nullptr;

	if (!LastTask_Local->AddSubsequent(Task))
	{
		// 已经完成了
		LastTask_Local->Release();
		return nullptr;
	}

	return LastTask_Local;
}

void FPipe::ClearTask(FTask& Task)
{
	FTask* Task_Local = &Task;
	if (LastTask.compare_exchange_strong(Task_Local, nullptr, std::memory_order_acq_rel, std::memory_order_acquire))
	{
		// 当前还是最新一个任务，只能pipe来代为释放
		Task.Release();
	}

	if (TaskCount.fetch_sub(1, std::memory_order_relaxed) == 1)
	{
		std::unique_lock lock(EmptyMtx);
		EmptyCV.notify_all();
	}
}

// Maintains pipe callstack. Due to busy waiting tasks from multiple pipes can be executed nested.
class FPipeCallStack
{
public:
	static void Push(const FPipe& Pipe)
	{
		CallStack.push_back(&Pipe);
	}

	static void Pop(const FPipe& Pipe)
	{
		assert(CallStack.back() == &Pipe);
		CallStack.pop_back();
	}

	// returns true if a task from the given pipe is being being executed on the top of the stack.
	// the method deliberately doesn't look deeper because even if the pipe is there and technically it's safe to assume
	// accessing a resource protected by a pipe is thread-safe, logically it's a bug because it's an accidental condition
	static bool IsOnTop(const FPipe& Pipe)
	{
		return CallStack.size() != 0 && CallStack.back() == &Pipe;
	}

private:
	static thread_local std::vector<const FPipe*> CallStack;
};

thread_local std::vector<const FPipe*> FPipeCallStack::CallStack;

void FPipe::ExecutionStarted()
{
	FPipeCallStack::Push(*this);
}

void FPipe::ExecutionFinished()
{
	FPipeCallStack::Pop(*this);
}

bool FPipe::IsInContext() const
{
	return FPipeCallStack::IsOnTop(*this);
}