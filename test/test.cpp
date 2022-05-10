#include "rt.hpp"
#include <iostream>


[[ndp::sim]]
void func(void* args) {
    size_t volatile* ptr = (size_t volatile*)args;
    for (size_t i = 0; i < 1000; i++) {
        *ptr = i;
    }
    std::cout << "ok\n";
}

int main() {
    for (size_t i = 0; i < 64; ++i) {
        ndp::thread_launch(i % 2, func, new size_t{i});
    }
    ndp::run();
}