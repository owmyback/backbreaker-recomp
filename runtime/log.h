#pragma once
#include <cstdio>
#include <cstdlib>

#define info(fmt, ...) std::fprintf(stderr, "[rt] " fmt "\n", ##__VA_ARGS__)
#define warn(fmt, ...) std::fprintf(stderr, "[rt] WARNING: " fmt "\n", ##__VA_ARGS__)
#define fatal(fmt, ...) do { std::fprintf(stderr, "[rt] FATAL: " fmt "\n", ##__VA_ARGS__); std::abort(); } while (0)
