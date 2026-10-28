/* SPDX-License-Identifier: GPL-2.0-only
 * The identity file is how a client tells an older service from a current one.
 * Its format must survive every future build, so it is pinned here. */
#include "../src/shared-internal.h"
#include <assert.h>

static void write_raw(const char *root,const char *text) {
 char path[220];snprintf(path,sizeof path,"%s/service.id",root);
 int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(fd>=0);
 assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));close(fd);
}
int main(void) {
 char root[160];
 setenv("HD60S_TEST_NAMESPACE","identity",1);
 assert(!hd_shared_paths(root,sizeof root));

 unsigned build=0;int pid=0;
 char path[220];snprintf(path,sizeof path,"%s/service.id",root);unlink(path);
 assert(hd_service_identity_read(root,&build,&pid)==-1); // Absent reads as unknown.

 assert(!hd_service_identity_write(root,405,4321));
 assert(!hd_service_identity_read(root,&build,&pid));
 assert(build==405 && pid==4321);

 // A later build overwrites rather than appends, so the file never grows stale tails.
 assert(!hd_service_identity_write(root,1000,7));
 assert(!hd_service_identity_read(root,&build,&pid));
 assert(build==1000 && pid==7);

 write_raw(root,"garbage\n");
 assert(hd_service_identity_read(root,&build,&pid)==-1);
 write_raw(root,"405\n"); // Build without a pid is not an identity.
 assert(hd_service_identity_read(root,&build,&pid)==-1);
 write_raw(root,"405 0\n"); // Pid 0 would signal the whole process group.
 assert(hd_service_identity_read(root,&build,&pid)==-1);
 write_raw(root,"405 -1\n");
 assert(hd_service_identity_read(root,&build,&pid)==-1);

 unlink(path);rmdir(root);
 puts("Service identity passed: round trip, overwrite, and every malformed form rejected");
 return 0;
}
