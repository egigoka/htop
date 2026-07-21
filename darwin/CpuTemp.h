#ifndef HEADER_CpuTemp
#define HEADER_CpuTemp

#if defined(BUILD_WITH_CPU_TEMP) && defined(HTOP_AARCH64)
#define CPUTEMP_SUPPORT

#include <stddef.h>

#include <IOKit/IOKitLib.h>

typedef struct {
   io_connect_t connection;
   const char* const* keys;
   size_t keyCount;
   double temperature;
} CpuTempData;

int CpuTemp_init(CpuTempData* data);
void CpuTemp_update(CpuTempData* data);
void CpuTemp_cleanup(CpuTempData* data);
#endif

#endif
