#include <atomic>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>

#include "rt.hpp"

namespace ndp {

// TODO make this configurable
constexpr size_t core_count = 2;
constexpr size_t sync_interval = 1000;

struct ThreadData {
    size_t core_id{0};
    size_t cycles{0};
    size_t instructions{0};

	std::atomic_bool running{false}; 

	// std::mutex list_lock;
	ThreadData* next_thread{nullptr};
	ThreadData* prev_thread{nullptr};

	void link_into(ThreadData* td) {
		// std::lock_guard lg{td->list_lock};
		if (td->next_thread != td) {
			// std::lock_guard nlg{td->next_thread->list_lock};

			this->prev_thread = td;
			this->next_thread = td->next_thread;

			td->next_thread->prev_thread = this;
			td->next_thread = this;
		} else {
			// td is alone in linked list
			this->next_thread = td;
			this->prev_thread = td;

			td->next_thread = this;
			td->prev_thread = this;
		}
	}

	auto unlink() -> ThreadData* {
		ThreadData* out{nullptr};
		if (this->next_thread != this) {
			out = this->next_thread;
			if (this->prev_thread != this->next_thread) {
				// td is in list of three or more threads
				// std::lock_guard plg{this->prev_thread->list_lock};
				// std::lock_guard nlg{this->next_thread->list_lock};

				this->prev_thread->next_thread = this->next_thread;
				this->next_thread->prev_thread = this->prev_thread;
			} else {
				// this is in list of two threads
				this->prev_thread->next_thread = this->prev_thread;
				this->prev_thread->prev_thread = this->prev_thread;
			}
		} 
		return out;
	}
};

thread_local ThreadData tdata{};

struct CoreState {
	std::mutex list_lock;
	ThreadData* head_thread;

	void add_thread(ThreadData* td) {
		std::lock_guard lg{this->list_lock};
		if (this->head_thread) {
			td->link_into(this->head_thread);
		} else {
			td->next_thread = td;
			td->prev_thread = td;
			this->head_thread = td;
		}
	}

	void remove_thread(ThreadData* td) {
		std::lock_guard lg{this->list_lock};
		auto* next = td->unlink();
		// if dropped thread is head, change head to next
		// if there is no next, change it to nullptr instead
		if (this->head_thread == td) {
			this->head_thread = next;
		}
	}
};

struct SystemState {
    std::atomic_size_t total_threads{0};

	std::atomic_size_t scheduled_threads{0};
    std::atomic_size_t started_threads{0};
    std::atomic_size_t running_threads{0};

    std::atomic_bool resume1{false};
	std::atomic_bool resume2{false};
    std::vector<CoreState> cores{core_count};
};

SystemState sys_state{};

void thread_sync() {
    sys_state.resume1 = false;
    sys_state.running_threads -= 1;
	// if we are last to sync, notify main thread
	if (sys_state.running_threads == 0) {
		sys_state.running_threads.notify_all();
	}

    sys_state.resume1.wait(false);

    // wait until this core is scheduled
	tdata.running.wait(false);

	// wait until main thread is done updating schedule
	sys_state.resume2.wait(false);

	// reset counters
	tdata.cycles = 0;
	tdata.instructions = 0;

	// start this thread (indicating that we have passed both wait points). If we are last to start, notify waiters
	size_t started = sys_state.started_threads.fetch_add(1);
	if (started + 1 == sys_state.running_threads) {
		sys_state.started_threads.notify_all();
	}

	// wait until all scheduled threads have started (passed both wait points)
    for (size_t started = sys_state.started_threads; started != sys_state.scheduled_threads; started = sys_state.started_threads) {
        sys_state.started_threads.wait(started);
    }

}

void launch_trampoline(size_t this_core_id, void (*func)(void*), void* args) {
	auto& core = sys_state.cores[this_core_id];
    // set up thread local variables
    tdata.core_id = this_core_id;

	// link this thread into core
	core.add_thread(&tdata);

    // wait until other threads reach sync point
    thread_sync();
    // execute function (this will probably call back into thread_sync() at some point
    func(args);

	// unlink this thread from data structures
	core.remove_thread(&tdata);

    size_t prev_running = sys_state.running_threads.fetch_sub(1);
	// if we are last thread to exit, notify main thread
	if (prev_running == 1) {
		sys_state.running_threads.notify_all();
	}
    sys_state.total_threads -= 1;
}

void thread_launch(size_t core_id, void (*func)(void*), void* args) {
    // important to do this now, guarantees that main thread does not accidentally start cleanup
    sys_state.total_threads += 1;
    sys_state.running_threads += 1;
	// sys_state.cores[core_id].total_threads += 1;
    // create thread and detach to run independently of handle
    std::thread t{launch_trampoline, core_id, func, args};
    t.detach();
}

void configure(size_t core_count) {
	// sys_state.cores.resize(core_count);
}

void run() {
	// loop while there are still running threads
    while (sys_state.total_threads > 0) {
		// wait until all running threads have reached sync point
        for (size_t running = sys_state.running_threads; running != 0;
             running = sys_state.running_threads) {
			sys_state.running_threads.wait(running);
        }
		// set barrier to prevent threads woken up during core update from immediately beginning execution
		sys_state.resume2 = false;

		// perform state updates (nothing yet)
		// std::cout << "synchronized!\n";

		// for each core with pending threads, reschedule
		size_t total_populated_cores = 0;
		for (auto& core : sys_state.cores) {
			if (core.head_thread) {
				// reschedule
				core.head_thread->running = false;
				auto* next = core.head_thread->next_thread;
				next->running = true;
				next->running.notify_all();

				core.head_thread = next;

				total_populated_cores += 1;
			}
		}

		sys_state.running_threads = total_populated_cores;
		sys_state.scheduled_threads = total_populated_cores;
		sys_state.started_threads = 0;

		// notify threads that just ran to enter waiting loop to get rescheduled
		sys_state.resume1 = true;
		sys_state.resume1.notify_all();

		// notify previously unscheduled threads to start execution
		sys_state.resume2 = true;
		sys_state.resume2.notify_all();
    }
}

namespace sim {

void dynamic_instr(size_t Count) {
    tdata.cycles += Count;
    tdata.instructions += Count;
    if (tdata.cycles > sync_interval) {
        thread_sync();
    }
}

void memload(void* Addr, size_t Size) {
    // temporary: assume load takes 10 cycles
    tdata.cycles += 10;
    if (tdata.cycles > sync_interval) {
        thread_sync();
    }
}

void memstore(void* Addr, size_t Size) {
    // temporary: assume store takes 10 cycles
    tdata.cycles += 10;
    if (tdata.cycles > sync_interval) {
        thread_sync();
    }
}

};  // namespace sim

};  // namespace ndp

// NOLINTNEXTLINE
extern "C" void _ndp_sim_dynamic_instr(size_t count) {
    ndp::sim::dynamic_instr(count);
}
// NOLINTNEXTLINE
extern "C" void _ndp_sim_memload(void* addr, size_t size) {
    ndp::sim::memload(addr, size);
}
// NOLINTNEXTLINE
extern "C" void _ndp_sim_memstore(void* addr, size_t size) {
    ndp::sim::memstore(addr, size);
}