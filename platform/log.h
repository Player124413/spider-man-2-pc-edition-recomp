#pragma once
#include <stdio.h>

// На Switch stdout уходит в nxlink (если запущено через `nxlink -s`), на mac — в терминал.
#define SNX_LOG(...) do { fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); fflush(stdout); } while (0)
