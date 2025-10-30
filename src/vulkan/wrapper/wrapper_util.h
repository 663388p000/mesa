#ifndef __WRAPPER_UTIL_H
#define __WRAPPER_UTIL_H

#include <sys/stat.h>

#define CREATE_FOLDER(folder) \
({ \
   struct stat sb; \
   if (stat(folder, &sb) != 0 || !S_ISDIR(sb.st_mode)) \
      mkdir(folder, 770); \
})

char *
get_executable_name(void);

#endif
