#include "util.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

void util_set_thread_name(const char *name)
{
#ifdef __linux__
    if (name && name[0] != '\0')
        prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
#else
    (void)name;
#endif
}
