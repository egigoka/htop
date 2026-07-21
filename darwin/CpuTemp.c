#include "config.h" // IWYU pragma: keep

#include "darwin/CpuTemp.h"

#ifdef CPUTEMP_SUPPORT

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

enum {
   SMC_SELECTOR = 2,
   SMC_CMD_READ_BYTES = 5,
   SMC_CMD_READ_KEYINFO = 9,
   SMC_MAX_BYTES = 32,
};

typedef struct {
   uint8_t major;
   uint8_t minor;
   uint8_t build;
   uint8_t reserved;
   uint16_t release;
} SMCVersion;

typedef struct {
   uint16_t version;
   uint16_t length;
   uint32_t cpuPLimit;
   uint32_t gpuPLimit;
   uint32_t memPLimit;
} SMCLimitData;

typedef struct {
   uint32_t dataSize;
   uint32_t dataType;
   uint8_t dataAttributes;
} SMCKeyInfo;

typedef struct {
   uint32_t key;
   SMCVersion version;
   SMCLimitData limits;
   SMCKeyInfo keyInfo;
   uint8_t result;
   uint8_t status;
   uint8_t data8;
   uint32_t data32;
   uint8_t bytes[SMC_MAX_BYTES];
} SMCKeyData;

static const char* const cpuTempKeysM1[] = {
   "Tp09", "Tp0T", "Tp01", "Tp05", "Tp0D", "Tp0H", "Tp0L", "Tp0P", "Tp0X", "Tp0b",
};

static const char* const cpuTempKeysM2[] = {
   "Tp1h", "Tp1t", "Tp1p", "Tp1l", "Tp01", "Tp05", "Tp09", "Tp0D", "Tp0X", "Tp0b", "Tp0f", "Tp0j",
};

static const char* const cpuTempKeysM3[] = {
   "Te05", "Te0L", "Te0P", "Te0S", "Tf04", "Tf09", "Tf0A", "Tf0B",
   "Tf0D", "Tf0E", "Tf44", "Tf49", "Tf4A", "Tf4B", "Tf4D", "Tf4E",
};

static const char* const cpuTempKeysM4[] = {
   "Te05", "Te09", "Te0H", "Te0S", "Tp01", "Tp05", "Tp09", "Tp0D", "Tp0V", "Tp0Y", "Tp0b", "Tp0e",
};

static const char* const cpuTempKeysM5[] = {
   "Tp00", "Tp04", "Tp08", "Tp0C", "Tp0G", "Tp0K", "Tp0O", "Tp0R", "Tp0U",
   "Tp0X", "Tp0a", "Tp0d", "Tp0g", "Tp0j", "Tp0m", "Tp0p", "Tp0u", "Tp0y",
};

static uint32_t CpuTemp_fourCC(const char value[4]) {
   return (uint32_t)(uint8_t)value[0] << 24 |
          (uint32_t)(uint8_t)value[1] << 16 |
          (uint32_t)(uint8_t)value[2] << 8 |
          (uint32_t)(uint8_t)value[3];
}

static kern_return_t CpuTemp_readKey(io_connect_t connection, const char key[4], SMCKeyData* output) {
   SMCKeyData input = {0};
   size_t outputSize = sizeof(*output);

   input.key = CpuTemp_fourCC(key);
   input.data8 = SMC_CMD_READ_KEYINFO;
   kern_return_t result = IOConnectCallStructMethod(connection, SMC_SELECTOR, &input, sizeof(input), output, &outputSize);
   if (result != kIOReturnSuccess || output->keyInfo.dataSize == 0 || output->keyInfo.dataSize > SMC_MAX_BYTES)
      return result != kIOReturnSuccess ? result : kIOReturnBadArgument;

   SMCKeyInfo keyInfo = output->keyInfo;
   input.keyInfo.dataSize = keyInfo.dataSize;
   input.data8 = SMC_CMD_READ_BYTES;
   memset(output, 0, sizeof(*output));
   outputSize = sizeof(*output);
   result = IOConnectCallStructMethod(connection, SMC_SELECTOR, &input, sizeof(input), output, &outputSize);
   output->keyInfo = keyInfo;
   return result;
}

static double CpuTemp_keyValue(const SMCKeyData* data) {
   if (data->keyInfo.dataType == CpuTemp_fourCC("flt ") && data->keyInfo.dataSize >= sizeof(float)) {
      float value;
      memcpy(&value, data->bytes, sizeof(value));
      return value;
   }

   if (data->keyInfo.dataType == CpuTemp_fourCC("sp78") && data->keyInfo.dataSize >= 2)
      return ((int8_t)data->bytes[0] * 256 + data->bytes[1]) / 256.0;

   return NAN;
}

static int CpuTemp_selectKeys(CpuTempData* data, unsigned int generation) {
   switch (generation) {
      case 1:
         data->keys = cpuTempKeysM1;
         data->keyCount = sizeof(cpuTempKeysM1) / sizeof(cpuTempKeysM1[0]);
         return 0;
      case 2:
         data->keys = cpuTempKeysM2;
         data->keyCount = sizeof(cpuTempKeysM2) / sizeof(cpuTempKeysM2[0]);
         return 0;
      case 3:
         data->keys = cpuTempKeysM3;
         data->keyCount = sizeof(cpuTempKeysM3) / sizeof(cpuTempKeysM3[0]);
         return 0;
      case 4:
         data->keys = cpuTempKeysM4;
         data->keyCount = sizeof(cpuTempKeysM4) / sizeof(cpuTempKeysM4[0]);
         return 0;
      case 5:
         data->keys = cpuTempKeysM5;
         data->keyCount = sizeof(cpuTempKeysM5) / sizeof(cpuTempKeysM5[0]);
         return 0;
      default:
         return -1;
   }
}

int CpuTemp_init(CpuTempData* data) {
   data->connection = IO_OBJECT_NULL;
   data->keys = NULL;
   data->keyCount = 0;
   data->temperature = NAN;

   char brand[128];
   size_t brandSize = sizeof(brand);
   if (sysctlbyname("machdep.cpu.brand_string", brand, &brandSize, NULL, 0) != 0 || brandSize == 0)
      return -1;
   brand[sizeof(brand) - 1] = '\0';

   unsigned int generation;
   if (sscanf(brand, "Apple M%u", &generation) != 1 || CpuTemp_selectKeys(data, generation) != 0)
      return -1;

   io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
   if (service == IO_OBJECT_NULL)
      return -1;

   kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &data->connection);
   IOObjectRelease(service);
   return result == kIOReturnSuccess ? 0 : -1;
}

void CpuTemp_update(CpuTempData* data) {
   double sum = 0.0;
   size_t count = 0;

   for (size_t i = 0; i < data->keyCount; i++) {
      SMCKeyData keyData = {0};
      if (CpuTemp_readKey(data->connection, data->keys[i], &keyData) != kIOReturnSuccess)
         continue;

      double value = CpuTemp_keyValue(&keyData);
      if (value > 0.0 && value <= 150.0) {
         sum += value;
         count++;
      }
   }

   data->temperature = count > 0 ? sum / count : NAN;
}

void CpuTemp_cleanup(CpuTempData* data) {
   if (data->connection != IO_OBJECT_NULL) {
      IOServiceClose(data->connection);
      data->connection = IO_OBJECT_NULL;
   }
}

#endif
