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

#define WRAPPER_LOG_LEVEL(s) (wrapper_log_level && strstr(wrapper_log_level, #s))

#define WRAPPER_LOG(level, fmt, ...) \
do {\
   if (WRAPPER_LOG_LEVEL(level)) {\
      fprintf(wrapper_log_file, "[%s]: " fmt "\n", #level, ##__VA_ARGS__);\
      fflush(wrapper_log_file);\
   }\
} while (0)

#endif
