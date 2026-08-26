/* dotnet_imports.h -- imports the NativeAOT payload needs. See dotnet_imports.c.
 * MIT license; see LICENSE. */

#ifndef __DOTNET_IMPORTS_H__
#define __DOTNET_IMPORTS_H__

#include <stddef.h>
#include <time.h>

// The "environ" import resolves to newlib's own `environ` variable -- see
// imports.c. There is deliberately no wrapper object: a snapshot goes stale
// the moment setenv() reallocates the array, which is what crashed runs 3
// through 10. See the long comment in dotnet_imports.c.

void dotnet_imports_init(void);

void  *sts2_memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen);
void  *sts2_memset_chk(void *dst, int c, size_t len, size_t dstlen);
char  *sts2_strncpy_chk2(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen);
int    sts2_getpagesize(void);
time_t sts2_timegm(struct tm *tm);
int    sts2_uname(void *buf);
void   sts2_exit_now(int status);

int  sts2_epoll_create1(int flags);
int  sts2_epoll_ctl(int e, int op, int fd, void *ev);
int  sts2_epoll_wait(int e, void *ev, int max, int timeout);
long sts2_recvmsg(int fd, void *msg, int flags);
long sts2_sendmsg(int fd, const void *msg, int flags);
int  sts2_getpeername(int fd, void *addr, void *len);
int  sts2_msync(void *addr, size_t len, int flags);
int  sts2_futimens(int fd, const void *times);

int      sts2_sigfillset(void *set);
int      sts2_setgid(unsigned gid);
int      sts2_setuid(unsigned uid);
unsigned sts2_getegid(void);
int      sts2_setgroups(size_t n, const unsigned *list);
int      sts2_getgroups(int n, unsigned *list);
int      sts2_getgrouplist(const char *user, unsigned gid, unsigned *groups, int *ngroups);
int      sts2_getpwnam_r(const char *name, void *pwd, char *buf, size_t buflen, void **result);
int      sts2_execve(const char *path, char *const argv[], char *const envp[]);
int      sts2_waitid(int idtype, unsigned id, void *infop, int options);

#endif
