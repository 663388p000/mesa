#ifndef WRAPPER_LOG_H
#define WRAPPER_LOG_H

#include <unistd.h>
#include <libgen.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

char *get_wrapper_log_level(void);

#ifdef __cplusplus
}
#endif

char *
get_executable_name(void);

void                                                                         
dump_shader_code(const uint32_t *code, size_t size);

#define WRAPPER_LOG_LEVEL(s) (get_wrapper_log_level() && strcmp(#s, get_wrapper_log_level()) == 0)

#define WRAPPER_LOGE(...) \
do {\
   if (WRAPPER_LOG_LEVEL(error) || \
      WRAPPER_LOG_LEVEL(validation) || \
      WRAPPER_LOG_LEVEL(shader)) {\
      dprintf(STDERR_FILENO, __VA_ARGS__);\
   }\
} while (0)
#define WRAPPER_LOGI(...) \
do {\
   if (WRAPPER_LOG_LEVEL(info)) {\
      dprintf(STDERR_FILENO, __VA_ARGS__);\
   }\
} while (0)
#define WRAPPER_LOGS(code, size) \
do {\
   if (WRAPPER_LOG_LEVEL(shader)) { \
      dump_shader_code(code, size); \
   } \
} while (0)

#endif
