#pragma once

#include <cstddef>
#include <thread>
#include <utility>

namespace ndp {

void thread_launch(size_t core_id, void (*func)(void*), void* args);
void run();

};