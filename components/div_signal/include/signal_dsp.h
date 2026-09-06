#include "c_util.h"
#pragma once

#include <stddef.h>

enum { SIGNAL_DSP_MAX_SAMPLES = 256 };

/* Shared scratch buffer: serialize calls from different tasks. */
bool signalDspComputeMagnitude(const double* realIn, double* realOut,
                               size_t samples);
