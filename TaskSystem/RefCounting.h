#pragma once

template<typename ReferencedType>
class TRefCountPtr
{
public:
	TRefCountPtr()
		: Reference(nullptr)
	{

	}

	TRefCountPtr(ReferencedType* InPtr, bool bAddRef = true)
	{
		Reference = InPtr;
		if (Reference && bAddRef)
		{
			Reference->AddRef();
		}
	}

	TRefCountPtr(const TRefCountPtr& Copy)
	{
		Reference = Copy.Reference;
		if (Reference)
		{
			Reference->AddPtr();
		}
	}

	template<typename CopyReferencedType>
	explicit TRefCountPtr(const TRefCountPtr<CopyReferencedType>& Copy)
	{
		Reference = static_cast<ReferencedType*>(Copy.GetReference());
		if (Reference)
		{
			Reference->AddRef();
		}
	}

	TRefCountPtr(TRefCountPtr&& Move)
	{
		Reference = Move.Reference;
		Move.Reference = nullptr;
	}

	template<typename MoveReferencedType>
	explicit TRefCountPtr(TRefCountPtr<MoveReferencedType>&& Move)
	{
		Reference = static_cast<ReferencedType*>(Move.GetReference());
		Move.Reference = nullptr;
	}

	~TRefCountPtr()
	{
		if (Reference)
		{
			Reference->Release();
		}
	}

	TRefCountPtr& operator=(ReferencedType* InReference)
	{
		if (Reference != InReference)
		{
			// Call AddRef before Release, in case the new reference is the same as the old reference.
			ReferencedType* OldReference = Reference;
			Reference = InReference;
			if (Reference)
			{
				Reference->AddRef();
			}
			if (OldReference)
			{
				OldReference->Release();
			}
		}
		return *this;
	}

	TRefCountPtr& operator=(const TRefCountPtr& InPtr)
	{
		return *this = InPtr.Reference;
	}

	template<typename CopyReferencedType>
	TRefCountPtr& operator=(const TRefCountPtr<CopyReferencedType>& InPtr)
	{
		return *this = InPtr.GetReference();
	}

	ReferencedType* operator->() const
	{
		return Reference;
	}

	//operator ReferenceType() const
	//{
	//	return Reference;
	//}

	bool operator==(const TRefCountPtr& B) const
	{
		return GetReference() == B.GetReference();
	}

	ReferencedType** GetInitReference()
	{
		*this = nullptr;
		return &Reference;
	}

	bool operator==(ReferencedType* B) const
	{
		return GetReference() == B;
	}

	TRefCountPtr& operator=(TRefCountPtr&& InPtr)
	{
		if (this != &InPtr)
		{
			ReferencedType* OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = nullptr;
			if (OldReference)
			{
				OldReference->Release();
			}
		}
		return *this;
	}

	ReferencedType* GetReference() const
	{
		return Reference;
	}

	bool IsValid() const
	{
		return Reference != nullptr;
	}
	void SafeRelease()
	{
		*this = nullptr;
	}

	uint32_t GetRefCount()
	{
		uint32_t Result = 0;
		if (Reference)
		{
			Result = Reference->GetRefCount();
		}
		return Result;
	}
private:
	ReferencedType* Reference;
};