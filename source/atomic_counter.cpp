/** 
 * FiberTaskingLib - A tasking library that uses fibers for efficient task switching
 *
 * This library was created as a proof of concept of the ideas presented by
 * Christian Gyrling in his 2015 GDC Talk 'Parallelizing the Naughty Dog Engine Using Fibers'
 *
 * http://gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine
 *
 * FiberTaskingLib is the legal property of Adrian Astley
 * Copyright Adrian Astley 2015 - 2018
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ftl/atomic_counter.h"

#include "ftl/task_scheduler.h"


namespace ftl {

bool AtomicCounter::AddFiberToWaitingList(std::size_t fiberIndex, uint targetValue, std::atomic<bool> *fiberStoredFlag, std::size_t pinnedThreadIndex) {
	for (uint i = 0; i < NUM_WAITING_FIBER_SLOTS; ++i) {
		bool expected = true;
		// Try to acquire the slot
		if (!std::atomic_compare_exchange_strong_explicit(&m_freeSlots[i], &expected, false, std::memory_order_seq_cst, std::memory_order_relaxed)) {
			// Failed the race or the slot was already full
			continue;
		}

		// We found a free slot
		m_waitingFibers[i].FiberIndex = fiberIndex;
		m_waitingFibers[i].TargetValue = targetValue;
		m_waitingFibers[i].FiberStoredFlag = fiberStoredFlag;
		m_waitingFibers[i].PinnedThreadIndex = pinnedThreadIndex;
		// We have to use memory_order_seq_cst here instead of memory_order_acquire to prevent
		// later loads from being re-ordered before this store
		m_waitingFibers[i].InUse.store(false, std::memory_order_seq_cst);
		
		// Events are now being tracked


		// Now we do a check of the waiting fiber, to see if we reached the target value while we were storing everything
		uint value = m_value.load(std::memory_order_relaxed);
		if (m_waitingFibers[i].InUse.load(std::memory_order_acquire)) {
			return false;
		}

		if (m_waitingFibers[i].TargetValue == value) {
			expected = false;
			// Try to acquire InUse
			if (!std::atomic_compare_exchange_strong_explicit(&m_waitingFibers[i].InUse, &expected, true, std::memory_order_seq_cst, std::memory_order_relaxed)) {
				// Failed the race. Another thread got to it first.
				return false;
			}
			// Signal that the slot is now free
			// Leave IneUse == true
			m_freeSlots[i].store(true, std::memory_order_release);

			return true;
		}

		return false;
	}


	// BARF. We ran out of slots
	printf("All the waiting fiber slots are full. Not able to add another wait.\nIncrease the value of NUM_WAITING_FIBER_SLOTS or modify your algorithm to use less waits on the same counter");
	assert(false);
	return false;
}

void AtomicCounter::CheckWaitingFibers(uint value) {
	// Enter the shared section
	++m_lock;

	uint readyFiberIndices[NUM_WAITING_FIBER_SLOTS];
	uint nextIndex = 0;

	for (uint i = 0; i < NUM_WAITING_FIBER_SLOTS; ++i) {
		// Check if the slot is full
		if (m_freeSlots[i].load(std::memory_order_acquire)) {
			continue;
		}
		// Check if the slot is being modified by another thread
		if (m_waitingFibers[i].InUse.load(std::memory_order_acquire)) {
			continue;
		}

		// Do the actual value check
		if (m_waitingFibers[i].TargetValue == value) {
			bool expected = false;
			// Try to acquire InUse
			if (!std::atomic_compare_exchange_strong_explicit(&m_waitingFibers[i].InUse, &expected, true, std::memory_order_seq_cst, std::memory_order_relaxed)) {
				// Failed the race. Another thread got to it first
				continue;
			}
			readyFiberIndices[nextIndex++] = i;
		}
	}
	// Exit shared section
	--m_lock;
	// Wait for all threads to exit the shared section if there are fibers to ready
	if (nextIndex > 0) {
		while (m_lock.load() > 0) {
			// Spin
		}

		for (uint i = 0; i < nextIndex; ++i) {
			// Add the fiber to the TaskScheduler's ready list
			m_taskScheduler->AddReadyFiber(m_waitingFibers[i].PinnedThreadIndex, m_waitingFibers[i].FiberIndex, m_waitingFibers[i].FiberStoredFlag);
			// Signal that the slot is free
			// Leave InUse == true
			m_freeSlots[i].store(true, std::memory_order_release);
		}
	}	
}


} // End of namespace ftl
