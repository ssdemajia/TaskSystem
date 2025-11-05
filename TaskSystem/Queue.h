#pragma once
#include <deque>
#include <mutex>
#include <atomic>
#include <iostream>
template<typename T>
class FOverflowQueue
{
public:
	void enqueue(T* Item);

	T* dequeue();

	bool isEmpty();

	void debug() {}
private:
	std::mutex Mtx;
	std::deque<T*> Items;
};

template<typename T>
inline void FOverflowQueue<T>::enqueue(T* item)
{
	std::lock_guard guard(Mtx);
	Items.push_back(item);
}

template<typename T>
inline T* FOverflowQueue<T>::dequeue()
{
	std::lock_guard guard(Mtx);
	if (Items.size() == 0)
		return nullptr;
	T* item = Items.front();
	Items.pop_front();
	return item;
}

template<typename T>
bool FOverflowQueue<T>::isEmpty()
{
	std::lock_guard guard(Mtx);
	return Items.size() == 0;
}

template<typename T>
class FLockFreeQueue
{
	struct Node {
		T* Value = nullptr;
		std::atomic<Node*> Next{ nullptr };
	};

public:
	FLockFreeQueue() {
		Tail = Head = new Node();
	}

	void enqueue(T* Item)
	{
		Node* NewNode = new Node();
		NewNode->Value = Item;
		Node* Empty = nullptr;

		while (true)
		{
			Node* Tail_Local = Tail.load();
			Node* TailNext_Local = Tail_Local->Next.load();

			if (Tail_Local == Tail.load())
			{
				if (TailNext_Local == nullptr)
				{
					if (Tail_Local->Next.compare_exchange_strong(TailNext_Local, NewNode))
					{
						Tail.compare_exchange_strong(Tail_Local, NewNode);
						return;
					}
				}
				else
				{

				}
				
			}

			
		}
	}

	T* dequeue()
	{
		Node* Head_Local = Head.load();
		while (true)
		{
			//Node* Head_Local = Head.load();
			Node* HeadNext_Local = Head_Local->Next.load();
			Node* TailLocal = Tail.load();
			if (Head_Local = Head.load())
			{
				if (Head_Local == TailLocal)
				{
					return nullptr;
				}
				else
				{
					T* Value = HeadNext_Local->Value;
					if (Head.compare_exchange_strong(Head_Local, HeadNext_Local))
					{
						//delete Head_Local;
						return Value;
					}
				}

			}
		}
		
		return nullptr;
	}

	bool isEmpty()
	{
		return Head == Tail;
	}

	void debug() {
		Node* P = Head.load()->Next;
		int i = 0;
		while (P != Tail)
		{
			if (P->Next == nullptr)
				P = P;
			P = P->Next.load();
			i += 1;
		}
		std::cout << "i: " << i << std::endl;
	}
private:
	std::atomic<Node*> Head;
	std::atomic<Node*> Tail;
};