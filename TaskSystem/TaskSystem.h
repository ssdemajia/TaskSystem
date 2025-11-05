#pragma once
#include <atomic>
#include <vector>
#include <mutex>
#include <cassert>
#include <functional>
#include "RefCounting.h"
#include "Timeout.h"

#define PLATFORM_CACHE_LINE_SIZE	64
#define LOWLEVEL_TASK_SIZE PLATFORM_CACHE_LINE_SIZE

enum ETaskFlags
{

};

enum ETaskPriority
{

};
// special task priorities for tasks that are never sent to the scheduler
enum class EExtendedTaskPriority
{
	None,
	Inline, // a task priority for "inline" task execution - a task is executed "inline" by the thread that unlocked it, w/o scheduling
	TaskEvent, // a task priority used by task events, allows to shortcut task execution
	Count
};

enum ETaskState
{
	Ready = 0,
	Canceled = 1 << 0,
	Scheduled = 1 << 1,
	Running = 1 << 2,
	Completed = 1 << 3
};

class FTask;
class FPipe;
class FLowLevelTask;

namespace TTaskDelegate_Impl
{
	template<typename ReturnType>
	inline ReturnType MakeDummyValue()
	{
		return *(reinterpret_cast<ReturnType*>(uintptr_t(1)));
	}

	template<>
	inline void MakeDummyValue<void>()
	{
		return;
	}
}

template<typename = void(), uint32_t = PLATFORM_CACHE_LINE_SIZE>
class TTaskDelegate;


template<uint32_t TotalSize, typename ReturnType, typename... ParamTypes>
class TTaskDelegate<ReturnType(ParamTypes...), TotalSize>
{
	using ThisClass = TTaskDelegate<ReturnType(ParamTypes...), TotalSize>;


	struct TTaskDelegateBase
	{
		virtual void Destroy(void*)
		{
		};

		virtual ReturnType Call(void*, ParamTypes...) const
		{
			return TTaskDelegate_Impl::MakeDummyValue<ReturnType>();
		};

		virtual void Move(TTaskDelegateBase&, void*, void*, uint32_t)
		{
		};

		virtual ReturnType CallAndMove(ThisClass&, void*, uint32_t, ParamTypes...)
		{
			return TTaskDelegate_Impl::MakeDummyValue<ReturnType>();
		};
	};

	struct TTaskDelegateDummy final : TTaskDelegateBase
	{
		void Move(TTaskDelegateBase&, void*, void*, uint32_t) override
		{
		}

		ReturnType Call(void*, ParamTypes...) const override
		{
			return TTaskDelegate_Impl::MakeDummyValue<ReturnType>();
		}

		ReturnType CallAndMove(ThisClass&, void*, uint32_t, ParamTypes...) override
		{
			return TTaskDelegate_Impl::MakeDummyValue<ReturnType>();
		}

		void Destroy(void*) override
		{
		}

	};
	template<typename TCallableType>
	struct TTaskDelegateImpl : public TTaskDelegateBase
	{
		template<typename CallableT>
		TTaskDelegateImpl(CallableT&& Callable, void* InlineData)
		{
			new (InlineData) TCallableType(std::forward<CallableT>(Callable));
		}

		void Destroy(void* InlineData) override
		{
			TCallableType* LocalPtr = reinterpret_cast<TCallableType*>(InlineData);
			LocalPtr->~TCallableType();
		}

		inline void Move(TTaskDelegateBase& DstWrapper, void* DstData, void* SrcData, uint32_t DestInlineSize) override
		{
			TCallableType* SrcPtr = reinterpret_cast<TCallableType*>(SrcData);
			if ((sizeof(TCallableType) <= DestInlineSize) && (uintptr_t(DstData) % alignof(TCallableType)) == 0)
			{
				new (&DstWrapper) TTaskDelegateImpl<TCallableType>(std::move(*SrcPtr), DstData);
			}
			//else
			//{
			//	new (&DstWrapper) TTaskDelegateImpl<TCallableType, true>(MoveTemp(*SrcPtr), DstData);
			//}
			new (this) TTaskDelegateDummy();
		}

		ReturnType Call(void* InlineData, ParamTypes... Params) const override
		{
			TCallableType* LocalPtr = reinterpret_cast<TCallableType*>(InlineData);
			return (*LocalPtr)(Params...);
		}

		ReturnType CallAndMove(ThisClass& Destination, void* InlineData, uint32_t DestInlineSize, ParamTypes... Params) override
		{
			ReturnType Result = Call(InlineData, Params...);
			{
				Move(Destination.CallableWrapper, Destination.InlineStorage, InlineData, DestInlineSize);
			};
			return std::move(Result);
		}

	};

public:
	
	TTaskDelegate() {}

	~TTaskDelegate()
	{
		GetWrapper()->Destroy(InlineStorage);
	}

	template<typename CallableT>
	TTaskDelegate(CallableT&& Callable)
	{
		using TCallableType = std::decay_t<CallableT>;
		if constexpr ((sizeof(TCallableType) <= InlineStorageSize) && ((uintptr_t(InlineStorageSize) % alignof(TCallableType)) == 0))
		{
			new (&CallableWrapper) TTaskDelegateImpl<TCallableType>(std::forward<CallableT>(Callable), InlineStorage);
		}
	}
	
	template<typename CallableT>
	ThisClass& operator= (CallableT&& Callable)
	{
		using TCallableType = std::decay_t<CallableT>;
		GetWrapper()->Destroy(InlineStorage);
		if constexpr ((sizeof(TCallableType) <= InlineStorageSize) && ((uintptr_t(InlineStorageSize) % alignof(TCallableType)) == 0))
		{
			new (&CallableWrapper) TTaskDelegateImpl<TCallableType>(std::forward<CallableT>(Callable), InlineStorage);
		}
		//else
		//{
		//	new (&CallableWrapper) TTaskDelegateImpl<TCallableType, true>(Forward<CallableT>(Callable), InlineStorage);
		//}
		return *this;
	}

	ReturnType operator()(ParamTypes... Params) const
	{
		return GetWrapper()->Call(InlineStorage, Params...);
	}

	template<uint32_t DestTotalSize>
	ReturnType CallAndMove(TTaskDelegate<ReturnType(ParamTypes...), DestTotalSize>& Destination, ParamTypes... Params)
	{
		return GetWrapper()->CallAndMove(Destination, InlineStorage, TTaskDelegate<ReturnType(ParamTypes...), DestTotalSize>::InlineStorageSize, Params...);
	}

	TTaskDelegateBase* GetWrapper()
	{
		return static_cast<TTaskDelegateBase*>(&CallableWrapper);
	}

	const TTaskDelegateBase* GetWrapper() const
	{
		return static_cast<const TTaskDelegateBase*>(&CallableWrapper);
	}

	TTaskDelegateBase CallableWrapper;
	static constexpr uint32_t InlineStorageSize = TotalSize - sizeof(TTaskDelegateBase);
	mutable char InlineStorage[InlineStorageSize];
};

using FTaskDelegate = TTaskDelegate<FLowLevelTask* (), LOWLEVEL_TASK_SIZE - sizeof(uintptr_t) - sizeof(void*)>;

class FLowLevelTask
{
public:
	static thread_local FLowLevelTask* ActiveTask;

	bool IsCompleted() const
	{
		return PackedData.load(std::memory_order_seq_cst) & ETaskState::Completed;
	}

	static FLowLevelTask* GetActiveTask()
	{
		return FLowLevelTask::ActiveTask;
	}

	template<typename Runnable>
	void Init(const char* InDebugName, Runnable&& InRunnable)
	{
		Delegate = [LocalRunnable = std::forward<Runnable>(InRunnable)]() mutable -> FLowLevelTask* {
			LocalRunnable();
			return nullptr;
		};

		DebugName = InDebugName;
		PackedData.store(ETaskState::Ready, std::memory_order_release);
	}

	bool TryPrepareLaunch()
	{
		return (PackedData.fetch_or(ETaskState::Scheduled, std::memory_order_acq_rel) & ETaskState::Scheduled) == 0;
	}
	bool TryCancel();

	FLowLevelTask* ExecuteTask();
private:
	union FPackedData
	{
		uintptr_t PackedData;
		struct
		{
			uintptr_t State : 6;
			uintptr_t DebugName : 53;
			uintptr_t Priority : 3;
			uintptr_t Flags : 2;
		};
	};

	const char* DebugName = nullptr;
	FTaskDelegate Delegate;
	std::atomic<uintptr_t> PackedData;
};

template<typename Type, void (Type::*DeleteFunction)()>
class TDeleter
{
	Type* Value;
public:
	inline TDeleter(Type* InValue) : Value(InValue) {}
	inline TDeleter(const TDeleter&) = delete;
	inline TDeleter(TDeleter&& Other) : Value(Other.Value) {
		Other.Value = nullptr;
	}
	inline Type* operator ->() const { return Value; }
	~TDeleter() { 
		if (Value) {
			(Value->*DeleteFunction)();
		}
	}
};

class FTask
{
	// 先决条件
	class FPrerequisites
	{
	public:
		void Push(FTask* Prerequisite)
		{
			std::lock_guard guard(mtx);
			Prerequisites.push_back(Prerequisite);
		}

		void PushNoLock(FTask* Prerequisite)
		{
			Prerequisites.push_back(Prerequisite);
		}

		std::vector<FTask*> PopAll()
		{
			std::lock_guard guard(mtx);
			return std::move(Prerequisites);
		}
	private:
		std::mutex mtx;
		std::vector<FTask*> Prerequisites;
	};

	// 后续任务
	class FSubsequents
	{
	public:
		bool PushIfNotClosed(FTask* NewTask)
		{
			if (bIsClosed.load(std::memory_order_relaxed))
			{
				return false;
			}
			std::lock_guard guard(mtx);
			if (bIsClosed)
			{
				return false;
			}

			Subsequents.push_back(NewTask);
			return true;
		}

		std::vector<FTask*> Close()
		{
			std::lock_guard guard(mtx);
			bIsClosed.store(true, std::memory_order_seq_cst);
			return std::move(Subsequents);
		}

		bool IsClosed() const { return bIsClosed; }
	private:
		std::atomic_bool bIsClosed{ false };
		std::mutex mtx;
		std::vector<FTask*> Subsequents;
	};

public:
	FTask() = default;

	FTask(uint32_t InitRefCount)
		: RefCount(InitRefCount)
	{

	}
	~FTask() { assert(IsCompleted()); }

	void Init(const char* DebugName, EExtendedTaskPriority InExtendedTaskPriority);

	bool TrySetExecutionFlag()
	{
		uint32_t ExpectedUnlocked = 0;
		return NumLocks.compare_exchange_strong(ExpectedUnlocked, ExecutionFlag + 1, std::memory_order_acq_rel, std::memory_order_relaxed);
	}

	bool TryExecuteTask();
	// 子任务都完成了
	bool IsCompleted() const
	{
		return Subsequents.IsClosed();
	}

	bool IsAwaitable() const
	{
		return std::this_thread::get_id() != ExecutingThreadId.load(std::memory_order_relaxed);
	}

	// 同个pipe发起的任务会按顺序执行，不要求同一个线程
	void SetPipe(FPipe& InPipe)
	{
		NumLocks.fetch_add(1, std::memory_order_acq_rel);
		Pipe = &InPipe;
	}

	FPipe* GetPipe() const
	{
		return Pipe;
	}

	bool TryLaunch()
	{
		bool bWakeUpWorker = true;
		return TryUnlock(bWakeUpWorker);
	}

	void AddRef()
	{
		RefCount.fetch_add(1, std::memory_order_relaxed);
	}

	void Release()
	{
		uint32_t LocalRefCount = RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (LocalRefCount == 0)
		{
			delete this;
		}
	}

	// 尝试执行Task，如果Task在等前向依赖任务完成，那么递归的执行前向任务
	bool TryRetractAndExecute(FTimeout Timeout, uint32_t RecursionDepth = 0);

	uint32_t GetRefCount()const { return RefCount.load(std::memory_order_relaxed); }

	void AddNested(FTask& Nested);

	void Wait();
	bool Wait(FTimeout timeout);

	static FTask* GetCurrentTask()
	{
		return CurrentTask;
	}

	template<typename HigherLevelTaskType, decltype(std::declval<HigherLevelTaskType>().Pimpl)* = nullptr>
	bool AddPrerequisites(const HigherLevelTaskType& Prerequisite)
	{
		return Prerequisite.IsValid() ? AddPrerequisite(*Prerequisite.Pimpl.GetReference()) : false;
	}


	// 不能并发执行
	bool AddPrerequisite(FTask& Prerequisite);

	bool AddSubsequent(FTask& Subsequent)
	{
		return Subsequents.PushIfNotClosed(&Subsequent);
	}
private:
	bool WaitImpl(FTimeout Timeout);

	void ReleaseInternalReference()
	{
		LowLevelTask.TryCancel();
	}

	bool TryUnlock(bool& bWakeUpWorker);

	void Schedule(bool& bWakeUpWorker);

	// unlock后续任务，同时设置flag为完成态
	void Close()
	{
		assert(!IsCompleted());

		bool bWakeUpWorker = false;
		for (FTask* Subsequent : Subsequents.Close())
		{
			Subsequent->TryUnlock(bWakeUpWorker);
		}

		if (GetPipe() != nullptr)
		{
			ClearPipe();
		}
		ReleasePrerequisites();
	}

	void ClearPipe();

	void ReleasePrerequisites()
    	{
		for (FTask* Prerequisite : Prerequisites.PopAll())
		{
			Prerequisite->Release();
		}
	}

	static thread_local FTask* CurrentTask;

	FTask* ExchangeCurrentTask(FTask* Task)
	{
		FTask* PrevTask = CurrentTask;
		CurrentTask = Task;
		return PrevTask;
	}

	virtual void ExecuteTask() = 0;

	
private:
	std::atomic_uint32_t RefCount{ 0 };

	// 最高位用来区分NumLocks里保存的是未完成prerequistites任务还是未完成的nested任务的数量
	static constexpr uint32_t ExecutionFlag = 0x80000000;

	static constexpr uint32_t NumInitialLocks = 1;  // 默认为1，说明launch前都不能触发执行
	std::atomic_uint32_t NumLocks{ NumInitialLocks };

	FPipe* Pipe{ nullptr };

	FPrerequisites Prerequisites;
	FSubsequents Subsequents;
	FLowLevelTask LowLevelTask;

	// 当前执行的线程id
	std::atomic<std::thread::id> ExecutingThreadId;

	EExtendedTaskPriority ExtendedTaskPriority = EExtendedTaskPriority::None;
};


template<typename TaskBodyType>
class TExecutableTask : public FTask
{
public:
	TExecutableTask(const char* InDebugName, EExtendedTaskPriority InExtendedTaskPriority, TaskBodyType&& InTaskBody)
		: FTask(2)
		, TaskBody(std::move(InTaskBody))
	{
		Init(InDebugName, InExtendedTaskPriority);
	}

	virtual void ExecuteTask() override
	{
		TaskBody();
	}

	TaskBodyType TaskBody;
};

class FTaskHandle
{
public:
	FTaskHandle() = default;
	template<typename TaskBodyType>
	void Launch(const char* InDebugName, EExtendedTaskPriority InExtendedTaskPriority, TaskBodyType&& TaskBody)
	{
		TExecutableTask<TaskBodyType>* Task = new TExecutableTask(InDebugName, InExtendedTaskPriority, std::forward<TaskBodyType>(TaskBody));
		*(Pimpl.GetInitReference()) = Task;
		Task->TryLaunch();
	}

	template<typename TaskBodyType, typename PrerequisitesCollectionType>
	void Launch(const char* InDebugName, PrerequisitesCollectionType&& Prereq, EExtendedTaskPriority InExtendedTaskPriority, TaskBodyType&& TaskBody)
	{
		TExecutableTask<TaskBodyType>* Task = new TExecutableTask(InDebugName, InExtendedTaskPriority, std::forward<TaskBodyType>(TaskBody));
		Task->AddPrerequisites(Prereq);
		*(Pimpl.GetInitReference()) = Task;
		Task->TryLaunch();
	}

	bool IsValid() const { return Pimpl.IsValid(); }
	// checks if task's execution is done
	bool IsCompleted() const
	{
		return !IsValid() || Pimpl->IsCompleted();
	}
	bool Wait(std::chrono::system_clock::duration duration)
	{
		return !IsValid() || Pimpl->Wait(FTimeout{ duration });
	}

	bool Wait() const
	{
		if (IsValid())
		{
			Pimpl->Wait();
		}
		return true;
	}

	explicit FTaskHandle(FTask* Other)
		: Pimpl(Other, false)
	{
	}
	TRefCountPtr<FTask> Pimpl;
};

template<typename ResultType>
class TTask : public FTaskHandle
{

};


class FTaskEventBase : public FTask
{
public:
	static FTaskEventBase* Create(const char* DebugName) { return new FTaskEventBase(DebugName); }

private:
	FTaskEventBase(const char* DebugName)
		: FTask(1)
	{
		Init(DebugName, EExtendedTaskPriority::TaskEvent);
	}

	virtual void ExecuteTask() override final
	{
	}

};
class FTaskEvent : public FTaskHandle
{
public:
	FTaskEvent(const char* DebugName)
		: FTaskHandle(FTaskEventBase::Create(DebugName))
	{

	}

	// all prerequisites must be added before triggering the event
	template<typename PrerequisitesType>
	void AddPrerequisites(const PrerequisitesType& Prerequisites)
	{
		Pimpl->AddPrerequisites(Prerequisites);
	}

	void Trigger()
	{
		if (!IsCompleted()) // event can be triggered multiple times
		{
			// An event is not "in the system" until it's triggered, and should be kept alive only by external references. Once it's triggered it's in the system 
			// and can outlive external references, so we need to keep it alive by holding an internal reference. It will be released when the event is signalled
			Pimpl->AddRef();
			Pimpl->TryLaunch();
		}
	}
};
template<typename TaskBodyType>
FTaskHandle Launch(const char* InDebugName, TaskBodyType&& TaskBody, EExtendedTaskPriority InExtendedTaskPriority = EExtendedTaskPriority::None)
{
	FTaskHandle Handle;
	Handle.Launch(InDebugName, InExtendedTaskPriority, std::forward<TaskBodyType>(TaskBody));
	return Handle;
}

template<typename TaskBodyType, typename PrerequisitesCollectionType>
FTaskHandle Launch(const char* InDebugName, TaskBodyType&& TaskBody, PrerequisitesCollectionType&& Prerequisites, EExtendedTaskPriority InExtendedTaskPriority = EExtendedTaskPriority::None)
{
	FTaskHandle Handle;
	Handle.Launch(InDebugName, Prerequisites, InExtendedTaskPriority, std::forward<TaskBodyType>(TaskBody));
	return Handle;
}

template<typename TaskType>
void AddNested(const TaskType& Nested)
{
	FTask* Parent = FTask::GetCurrentTask();
	assert(Parent != nullptr);
	Parent->AddNested(*Nested.Pimpl.GetReference());
}