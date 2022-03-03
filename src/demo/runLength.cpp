#include <algorithms/example.h>
#include <stdio.h>
#include <iostream>

int main(int argc, char* argv[]) {
    int len = 7;

    int h_in[7] = {0, 1, 1, 2, 3, 3, 3};
    int* h_unique = new int[7];
    int* h_count = new int[7];
    int* h_run_out = new int[7];

    cubIntRunLength(h_in, 7, h_unique, h_count, h_run_out);

    std::cout << "h_unique: " << std::endl;
    for (int i = 0; i < len; i++) {
        std::cout << h_unique[i] << " ";
    }
    std::cout << std::endl;

    std::cout << "h_count: " << std::endl;
    for (int i = 0; i < len; i++) {
        std::cout << h_count[i] << " ";
    }
    std::cout << std::endl;

    std::cout << "h_run_out: " << std::endl;
    for (int i = 0; i < len; i++) {
        std::cout << h_run_out[i] << " ";
    }
    std::cout << std::endl;

    return 0;
}