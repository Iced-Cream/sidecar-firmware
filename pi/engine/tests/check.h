// pi/engine/tests/check.h
#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
