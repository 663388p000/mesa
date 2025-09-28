#ifndef WRAPPER_LOG_H
#define WRAPPER_LOG_H

#include <unistd.h>
#include <libgen.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

extern char *wrapper_log_level;
extern FILE *wrapper_log_file;

void
init_wrapper_logging(void);

void
stop_wrapper_logging(void);

void                                                                         
dump_shader_code(const uint32_t *code, size_t size);

VKAPI_ATTR VkBool32 VKAPI_CALL 
wrapper_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                              void *userData);

#define WRAPPER_LOG_LEVEL(s) (wrapper_log_level && strcmp(#s, wrapper_log_level) == 0)

#define WRAPPER_LOGE(...) \
do {\
   if (WRAPPER_LOG_LEVEL(error) || \
      WRAPPER_LOG_LEVEL(validation) || \
      WRAPPER_LOG_LEVEL(shader)) {\
      fprintf(wrapper_log_file, __VA_ARGS__);\
   }\
} while (0)
#define WRAPPER_LOGI(...) \
do {\
   if (WRAPPER_LOG_LEVEL(info)) {\
      fprintf(wrapper_log_file, __VA_ARGS__);\
   }\
} while (0)
#define WRAPPER_LOGS(code, size) \
do {\
   if (WRAPPER_LOG_LEVEL(shader)) { \
      dump_shader_code(code, size); \
   } \
} while (0)

#endif
