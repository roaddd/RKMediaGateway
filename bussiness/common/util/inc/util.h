#ifndef UTIL_H
#define UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set the current Linux thread name. Keep names within the kernel thread-name limit. */
void util_set_thread_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
