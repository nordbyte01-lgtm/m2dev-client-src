#pragma once

#include "SPSCQueue.h"
#include "EterBase/Singleton.h"
#include <thread>
#include <vector>
#include <functional>
#include <future>
#include <atomic>
#include <memory>

class CGameThreadPool : public CSingleton<CGameThreadPool>
{
public:
	using TTask = std::function<void()>;

	CGameThreadPool();
	~CGameThreadPool();

	// Initialize thread pool with specified worker count
	// If count <= 0, uses hardware_concurrency
	void Initialize(int iWorkerCount = -1);

	// Shutdown and join all worker threads
	void Destroy();

	// Enqueue a task and get a future to track completion
	template<typename TFunc>
	std::future<void> Enqueue(TFunc&& func);

	// Get number of active workers
	int GetWorkerCount() const { return static_cast<int>(m_workers.size()); }

	// Get approximate number of pending tasks across all queues
	size_t GetPendingTaskCount() const;

	// Check if pool is initialized
	bool IsInitialized() const { return m_bInitialized; }

private:
	struct TWorkerThread
	{
		std::thread thread;
		std::unique_ptr<SPSCQueue<TTask>> pTaskQueue;
		std::atomic<bool> bBusy;
		std::atomic<uint32_t> uTaskCount;

		TWorkerThread()
			: bBusy(false)
			, uTaskCount(0)
		{
		}
	};

	void WorkerThreadProc(int iWorkerIndex);
	int SelectLeastBusyWorker() const;

	std::vector<std::unique_ptr<TWorkerThread>> m_workers;
	std::atomic<bool> m_bShutdown;
	std::atomic<bool> m_bInitialized;
	std::atomic<int> m_iNextWorkerIndex; // For round-robin distribution

	static const size_t QUEUE_SIZE = 8192;
};

// Template implementation
template<typename TFunc>
std::future<void> CGameThreadPool::Enqueue(TFunc&& func)
{
	if (!m_bInitialized)
	{
		// If not initialized, execute on calling thread
		auto promise = std::make_shared<std::promise<void>>();
		auto future = promise->get_future();
		try
		{
			func();
			promise->set_value();
		}
		catch (...)
		{
			promise->set_exception(std::current_exception());
		}
		return future;
	}

	// Create a promise to track task completion
	auto promise = std::make_shared<std::promise<void>>();
	auto future = promise->get_future();

	// Wrap function in shared_ptr to avoid move issues with std::function
	auto pFunc = std::make_shared<typename std::decay<TFunc>::type>(std::forward<TFunc>(func));

	// Wrap the task with promise completion
	TTask task = [promise, pFunc]()
	{
		try
		{
			(*pFunc)();
			promise->set_value();
		}
		catch (...)
		{
			promise->set_exception(std::current_exception());
		}
	};

	// Select worker with least load
	int iWorkerIndex = SelectLeastBusyWorker();
	TWorkerThread* pWorker = m_workers[iWorkerIndex].get();

	// Try to enqueue the task
	if (!pWorker->pTaskQueue->Push(std::move(task)))
	{
		// Queue is full, execute on calling thread as fallback
		try
		{
			(*pFunc)();
			promise->set_value();
		}
		catch (...)
		{
			promise->set_exception(std::current_exception());
		}
	}
	else
	{
		pWorker->uTaskCount.fetch_add(1, std::memory_order_relaxed);
	}

	return future;
}
