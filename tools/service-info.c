/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/shared-internal.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <inttypes.h>
int main(void) {
 char root[160],path[220];if(hd_shared_paths(root,sizeof root))return 1;
 snprintf(path,sizeof path,"%s/stream-v1",root);int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0){perror("Shared service");return 1;}
 struct stat st;if(fstat(fd,&st)||st.st_size!=sizeof(hd_shared_map)||st.st_uid!=getuid())return 1;
 hd_shared_map *map=mmap(NULL,sizeof *map,PROT_READ,MAP_SHARED,fd,0);close(fd);if(map==MAP_FAILED)return 1;
 if(atomic_load(&map->magic)!=HD_SHARED_MAGIC || map->version!=HD_SHARED_VERSION)return 1;
 printf("Service PID: %d\nActive consumers: %u\nStatus: %s\nPublished video frames: %" PRIu64 "\nPublished audio blocks: %" PRIu64 "\nVideo dropped by service: %" PRIu64 "\n",atomic_load(&map->service_pid),atomic_load(&map->active_clients),hd_shared_status_text(atomic_load(&map->status)),atomic_load(&map->video_token)/4,atomic_load(&map->audio_serial),atomic_load(&map->dropped_video));
 for(unsigned i=0;i<HD_SHARED_CLIENTS;i++){int pid=atomic_load(&map->clients[i].pid);if(pid>0)printf("Consumer PID: %d\n",pid);}
 munmap(map,sizeof *map);return 0;
}
