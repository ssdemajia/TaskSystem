#include "Scheduler.h"
#include "TaskSystem.h"
#include "Queue.h"
#include "Pipe.h"
#include <iostream>

void TestBasic()
{
	{
		FTaskEvent Event("A");
		assert(!Event.IsCompleted());
		Event.Trigger();
	}

	{
		Launch("A", []() {
			std::cout << "123" << std::endl;
			}).Wait();
	}

	{
		FTaskEvent Event("E1");
		assert(!Event.IsCompleted());
		FTaskHandle Task = Launch("A", [&Event]() {
			Event.Wait();
			std::cout << "after!!!" << std::endl;
			});
		std::this_thread::sleep_for(std::chrono::seconds(1));
		assert(!Event.IsCompleted());

		Event.Trigger();
		assert(Event.IsCompleted());
		Event.Wait(std::chrono::seconds(0));
	}

	{
		std::atomic<bool> bDone{ false };
		Launch("A", [&bDone] { bDone = true; });
		while (!bDone)
		{
			std::this_thread::yield();
		}
	}

	{
		Launch("A", []() {
			FTaskHandle B = Launch("B", [] {
				std::cout << "BB!!" << std::endl;
				});
			AddNested(B);
			std::cout << "AA" << std::endl;
			}).Wait();
	}

	{
		FTaskEvent Event1("E1");
		FTaskEvent Event2("E2");
		FTaskEvent Event3("E3");
		Event1.AddPrerequisites(Event2);
		Event1.AddPrerequisites(Event3);
		std::atomic_int i = 1;
		FTaskHandle A = Launch("A", [&Event2, &i]() {
			i += 1;
			});
		FTaskHandle B = Launch("B", [&Event3, &i]() {
			i += 1;
			});
		Event2.AddPrerequisites(A);
		Event3.AddPrerequisites(B);
		std::this_thread::sleep_for(std::chrono::seconds(1));
		assert(!Event1.IsCompleted());
		Event2.Trigger();
		Event3.Trigger();
		Event1.Trigger();
		Event1.Wait();
		assert(i == 3);

	}

	{
		FTaskEvent Prereq{ "Prereq" };

		FTaskHandle Task = Launch("", [] {}, Prereq);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		assert(!Task.IsCompleted());

		Prereq.Trigger();
		Task.Wait();
	}
}

void TestPipe()
{
	{
		FTaskEvent Prereq{ "Prereq" };

		FPipe Pipe{ "Pipe" };
		std::atomic_int value = 0;
		// 等prereq完成后才放入pipe里
		FTaskHandle Task = Pipe.Launch("Task1", [&value]() {
			value = 1;
		}, Prereq);

		FTaskHandle Task2 = Pipe.Launch("Task2", [&value]() {
			value = 2;
		});
		FTaskHandle Task3 = Pipe.Launch("Task3", [&value]() {
			value = 3;
		});
		assert(!Task3.IsCompleted());
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		Prereq.Trigger();
		Task3.Wait();
		assert(value == 1);
	}
}

template<typename QueueType>
void TestQueue()
{
	const int NUM_ITEMS_PER_PRODUCER = 1000;
	const int NUM_PRODUCERS = 12;
	const int NUM_CONSUMERS = 4;
	QueueType queue;
	std::atomic_int totalSum{ 0 };
	std::atomic_int producedCount{ 0 };
	std::atomic_int consumedCount{ 0 };

	auto producer = [&](int id) {
		for (int i = 0; i < NUM_ITEMS_PER_PRODUCER; i++)
		{
			int* value = new int(id * NUM_ITEMS_PER_PRODUCER + i);
			queue.enqueue(value);
			totalSum.fetch_add(*value);
			producedCount.fetch_add(1);
		}
	};

	auto consumer = [&]()
	{
		int* value;
		while (consumedCount.load() < NUM_PRODUCERS * NUM_ITEMS_PER_PRODUCER)
		{
			if (value = queue.dequeue())
			{
				totalSum.fetch_sub(*value);
				consumedCount.fetch_add(1);
				delete value;
			}
		}
	};
	std::vector<std::thread> threads1;
	std::vector<std::thread> threads2;
	for (int i = 0; i < NUM_PRODUCERS; ++i) {
		threads1.emplace_back(producer, i);
	}
	for (auto& t : threads1) {
		t.join();
	}

	queue.debug();

	for (int i = 0; i < NUM_CONSUMERS; ++i) {
		threads2.emplace_back(consumer);
	}

	for (auto& t : threads2) {
		t.join();
	}

	assert(producedCount.load() == NUM_PRODUCERS * NUM_ITEMS_PER_PRODUCER);
	assert(consumedCount.load() == NUM_PRODUCERS * NUM_ITEMS_PER_PRODUCER);
	assert(totalSum.load() == 0);
	assert(queue.isEmpty());
}

template<typename Q>
void BenchmarkQueue(const std::string& name) {
	const static int NUM_OPERATIONS = 10000000;
	const static int NUM_THREADS = 4;
	Q queue;
	std::atomic<int> operationCount(0);

	auto worker = [&]() {
		while (true) {
			int count = operationCount.fetch_add(1);
			if (count >= NUM_OPERATIONS) break;

			if (count % 2 == 0) {
				int* value = new int(count);
				queue.enqueue(value);
			}
			else {
				int* value;
				if (value = queue.dequeue()) {
					delete value;
				}
			}
		}
		};

	std::vector<std::thread> threads;
	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < NUM_THREADS; ++i) {
		threads.emplace_back(worker);
	}

	for (auto& t : threads) {
		t.join();
	}

	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

	std::cout << name << " took " << duration.count() << " ms" << std::endl;

	// 清理剩余的内存
	int* remainingValue;
	while (remainingValue = queue.dequeue()) {
		delete remainingValue;
	}
}
int main()
{
	FScheduler::Get().StartWorkers(2);// std::thread::hardware_concurrency());

	
	//TestQueue<FOverflowQueue<int>>();
	TestQueue<FLockFreeQueue<int>>();
	//BenchmarkQueue<FOverflowQueue<int>>("OverflowQueue");

	FScheduler::Get().StopWorkers();
}