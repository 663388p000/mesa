#include "wrapper_log.h"

#define PATH_MAX_SIZE 1024
#define WRAPPER_SHADER_LOG_PATH "/sdcard/wrapper/shaders"
#define WRAPPER_LOG_PATH "/sdcard/wrapper/logs"
#define WRAPPER_VALIDATION_LOG_PATH "/sdcard/wrapper/validation"

char *wrapper_log_level;
FILE *wrapper_log_file;

static FILE *vvl_log_file;

static void get_formatted_date_time(char *buf, size_t length) 
{  
   time_t rawtime = time(NULL);
   struct tm *ptm = localtime(&rawtime);

   strftime(buf, 256, "%F_%H-%M-%S", ptm);
}

static char *get_executable_name() {
   char *path = malloc(PATH_MAX);

   int fd = open("/proc/self/cmdline", O_RDONLY);

   if (fd != -1) {
      read(fd, path, PATH_MAX_SIZE);
      char *ptr = strrchr(path, '/');
      if (ptr)
         path = ptr + 1;
      ptr = strrchr(path, '\\');
      if (ptr)
         path = ptr + 1;
      close(fd);
   }
   
   return path;
}

static int is_log_enabled() {
   if (WRAPPER_LOG_LEVEL(info) || WRAPPER_LOG_LEVEL(error))
      return 1;

   return 0;
}

void init_wrapper_logging()
{
   char date[256];
   
   if (!wrapper_log_level) {
      wrapper_log_level = getenv("WRAPPER_LOG_LEVEL");
      if (!wrapper_log_level)
         wrapper_log_level = "none";
   }
                                                                 
   get_formatted_date_time(date, 256);

   if (!wrapper_log_file && is_log_enabled()) {
      char *wrapper_log_filename;

      wrapper_log_filename = getenv("WRAPPER_LOG_FILE");

      if (!wrapper_log_filename)
         asprintf(&wrapper_log_filename, "%s/%s_%s", WRAPPER_LOG_PATH, get_executable_name(), date);

      if (!strcmp("stdout", wrapper_log_filename)) {
         wrapper_log_file = stdout;
      }
      else {
         wrapper_log_file = fopen(wrapper_log_filename, "w");
      }
   }

   if (!vvl_log_file && WRAPPER_LOG_LEVEL(validation)) {
      char *vvl_log_filename;
      
      asprintf(&vvl_log_filename, "%s/%s_%s", WRAPPER_VALIDATION_LOG_PATH, get_executable_name(), date);
      vvl_log_file = fopen(vvl_log_filename, "w");
   }
}

void  stop_wrapper_logging()
{
   if (vvl_log_file)
      fclose(vvl_log_file);

   if (wrapper_log_file)
      fclose(wrapper_log_file);
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

VKAPI_ATTR VkBool32 VKAPI_CALL
wrapper_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                              void *userData)
{
   const char* messageIdName = callbackData->pMessageIdName;
   int32_t messageIdNumber = callbackData->messageIdNumber;
   const char* message = callbackData->pMessage;

   fprintf(vvl_log_file, "[%s] Code %i : %s\n", messageIdName, messageIdNumber, message);
   fflush(vvl_log_file);

   return 0;
}
