#include "rt.hpp"
#include <iostream>


[[ndp::sim]]
void func(void* args) {
    int id = *(int*)args;
    for (;;) {
        std::cout << id << std::endl;
    }
}

int main() {
    for (int i = 0; i < 4; ++i) {
        ndp::thread_launch(i % 2, func, &i);
    }
    ndp::run();
}