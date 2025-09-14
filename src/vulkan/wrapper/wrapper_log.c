#include "wrapper_log.h"

#define PATH_MAX_SIZE 1024
#define WRAPPER_LOG_PATH "/sdcard/Wrapper"
#define WRAPPER_SHADER_LOG_PATH "/sdcard/Wrapper/shaders"

char *get_executable_name() {
   char *path = malloc(PATH_MAX);

   int fd = open("/proc/self/cmdline", O_RDONLY);

   if (fd != -1) {
      read(fd, path, PATH_MAX_SIZE);
      close(fd);
   }
   
   return path;
}

char* get_wrapper_log_level() {
   char *wrapper_log_level = getenv("WRAPPER_LOG_LEVEL");
   if (!wrapper_log_level)
      wrapper_log_level = "none";

   return wrapper_log_level;
}

void dump_shader_code(const uint32_t *code, size_t size) {
   char *file; 
   static int index = 0;
   
   asprintf(&file, "%s/%s_shader_%d.spv", WRAPPER_SHADER_LOG_PATH, get_executable_name(), index); 

   FILE *fp = fopen(file, "wb"); 
   fwrite(code, 1, size, fp); 
   fclose(fp); 

   index++;
}
