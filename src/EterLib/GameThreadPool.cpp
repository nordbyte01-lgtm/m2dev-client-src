#include "StdAfx.h"
#include "GameThreadPool.h"

CGameThreadPool::CGameThreadPool()
	: m_bShutdown(false)
	, m_bInitialized(false)
	, m_iNextWorkerIndex(0)
{
}

CGameThreadPool::~CGameThreadPool()
{
	Destroy();
}

void CGameThreadPool::Initialize(int iWorkerCount)
{
	if (m_bInitialized)
	{
		TraceError("CGameThreadPool::Initialize - Already initialized!");
		return;
	}

	// Determine worker count
	if (iWorkerCount <= 0)
	{
		iWorkerCount = static_cast<int>(std::thread::hardware_concurrency());
		if (iWorkerCount <= 0)
			iWorkerCount = 4; // Fallback to 4 workers
	}

	// Clamp worker count to reasonable range
	iWorkerCount = std::max(2, std::min(16, iWorkerCount));

	Tracef("CGameThreadPool::Initialize - Creating %d worker threads\n", iWorkerCount);

	m_bShutdown.store(false, std::memory_order_release);
	m_workers.reserve(iWorkerCount);

	// Initialize each worker
	for (int i = 0; i < iWorkerCount; ++i)
	{
		std::unique_ptr<TWorkerThread> pWorker(new TWorkerThread());
		pWorker->pTaskQueue.reset(new SPSCQueue<TTask>(QUEUE_SIZE));
		pWorker->bBusy.store(false, std::memory_order_relaxed);
		pWorker->uTaskCount.store(0, std::memory_order_relaxed);
		pWorker->thread = std::thread(&CGameThreadPool::WorkerThreadProc, this, i);
		m_workers.push_back(std::move(pWorker));
	}

	m_bInitialized.store(true, std::memory_order_release);
}

void CGameThreadPool::Destroy()
{
	if (!m_bInitialized)
		return;

	Tracef("CGameThreadPool::Destroy - Shutting down %d worker threads\n", GetWorkerCount());

	m_bShutdown.store(true, std::memory_order_release);
	m_bInitialized.store(false, std::memory_order_release);

	// Join all worker threads
	for (auto& pWorker : m_workers)
	{
		if (pWorker->thread.joinable())
			pWorker->thread.join();
	}

	m_workers.clear();
}

void CGameThreadPool::WorkerThreadProc(int iWorkerIndex)
{
	TWorkerThread* pWorker = m_workers[iWorkerIndex].get();
	int iIdleCount = 0;

	while (!m_bShutdown.load(std::memory_order_acquire))
	{
		TTask task;
		if (pWorker->pTaskQueue->Pop(task))
		{
			pWorker->bBusy.store(true, std::memory_order_relaxed);
			iIdleCount = 0;

			// Execute the task
			try
			{
				task();
			}
			catch (const std::exception& e)
			{
				TraceError("CGameThreadPool::WorkerThreadProc - Exception in worker %d: %s", iWorkerIndex, e.what());
			}
			catch (...)
			{
				TraceError("CGameThreadPool::WorkerThreadProc - Unknown exception in worker %d", iWorkerIndex);
			}

			pWorker->uTaskCount.fetch_sub(1, std::memory_order_relaxed);
			pWorker->bBusy.store(false, std::memory_order_relaxed);
		}
		else
		{
			// No work available - idle strategy
			++iIdleCount;

			if (iIdleCount < 100)
			{
				// Spin briefly for immediate work
				std::this_thread::yield();
			}
			else if (iIdleCount < 1000)
			{
				// Short sleep for moderate idle
				std::this_thread::sleep_for(std::chrono::microseconds(10));
			}
			else
			{
				// Longer sleep for extended idle
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}
}

int CGameThreadPool::SelectLeastBusyWorker() const
{
	if (m_workers.empty())
		return 0;

	// Simple load balancing: find worker with least pending tasks
	int iBestWorker = 0;
	uint32_t uMinTasks = m_workers[0]->uTaskCount.load(std::memory_order_relaxed);

	for (size_t i = 1; i < m_workers.size(); ++i)
	{
		uint32_t uTasks = m_workers[i]->uTaskCount.load(std::memory_order_relaxed);
		if (uTasks < uMinTasks)
		{
			uMinTasks = uTasks;
			iBestWorker = static_cast<int>(i);
		}
	}

	return iBestWorker;
}

size_t CGameThreadPool::GetPendingTaskCount() const
{
	size_t uTotal = 0;
	for (const auto& pWorker : m_workers)
	{
		uTotal += pWorker->uTaskCount.load(std::memory_order_relaxed);
	}
	return uTotal;
}
