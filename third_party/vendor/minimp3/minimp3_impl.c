/*
 * Single translation unit holding the minimp3 / minimp3_ex implementation.
 * Everything else includes minimp3_ex.h as a plain header (declarations
 * only); this keeps the decoder out of every C++ file that merely calls it.
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
