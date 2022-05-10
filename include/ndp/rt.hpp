#pragma once

#include <cstddef>
#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <utility>

namespace ndp {

void thread_launch(size_t core_id, auto func, auto&&... args);
void configure(size_t core_count);
void run();

namespace internal {

inline constexpr size_t sync_interval = 1000;

struct ThreadData {
    size_t core_id{0};
    size_t cycles{0};
    size_t instructions{0};

	std::atomic_bool running{false}; 

	// std::mutex list_lock;
	ThreadData* next_thread{nullptr};
	ThreadData* prev_thread{nullptr};

	void link_into(ThreadData* td);
	auto unlink() -> ThreadData*;
};


struct CoreState {
	std::mutex list_lock;
	ThreadData* head_thread;

	void add_thread(ThreadData* td);
	void remove_thread(ThreadData* td);
};

struct SystemState {
    std::atomic_size_t total_threads{0};

	std::atomic_size_t scheduled_threads{0};
    std::atomic_size_t started_threads{0};
    std::atomic_size_t running_threads{0};

    std::atomic_bool resume1{false};
	std::atomic_bool resume2{false};
    std::vector<std::unique_ptr<CoreState>> cores;
};

inline thread_local ThreadData tdata{};
inline SystemState sys_state{};

void thread_sync();

void launch_trampoline(size_t this_core_id, auto func, auto&&... args) {
	auto& core = *sys_state.cores[this_core_id];
    // set up thread local variables
    tdata.core_id = this_core_id;

	// link this thread into core
	core.add_thread(&tdata);

    // wait until other threads reach sync point
    thread_sync();
    // execute function (this will probably call back into thread_sync() at some point
    func(std::forward<decltype(args)>(args)...);

	// unlink this thread from data structures
	core.remove_thread(&tdata);

    size_t prev_running = sys_state.running_threads.fetch_sub(1);
	// if we are last thread to exit, notify main thread
	if (prev_running == 1) {
		sys_state.running_threads.notify_all();
	}
    sys_state.total_threads -= 1;
}


namespace sim {

void dynamic_instr(size_t Count);
void memload(void* Addr, size_t Size);
void memstore(void* Addr, size_t Size);

};  // namespace sim

};

void thread_launch(size_t core_id, auto func, auto&&... args) {
    using namespace internal;
    // important to do this now, guarantees that main thread does not accidentally start cleanup
    sys_state.total_threads += 1;
    sys_state.running_threads += 1;
	// sys_state.cores[core_id].total_threads += 1;
    // create thread and detach to run independently of handle
    std::thread t{launch_trampoline<decltype(func), decltype(args)...>, core_id, func, std::forward<decltype(args)>(args)...};
    t.detach();
}

};  // namespace ndp