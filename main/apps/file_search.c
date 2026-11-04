/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：增量深度优先搜索，只枚举名称/属性，不解码文件，不写存储卡。
 * English: Incremental depth-first name/stat search without decoding files or writing the card.
 */
#include "file_search.h"
#include "read_pico_search.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif
#define SEARCH_DEPTH 32
#define SEARCH_PATH 288
struct file_search {
    struct { DIR *dir; char path[SEARCH_PATH]; } stack[SEARCH_DEPTH];
    unsigned depth;
    char query[65];
    file_search_match_t match;
    void *ctx;
    file_search_stats_t stats;
};
void file_search_close(file_search_t *s){
    if(!s)return;
    while(s->depth)closedir(s->stack[--s->depth].dir);
    free(s);
}
static file_search_t *search_begin(const char *root,const char *query,file_search_match_t match,void *ctx){
    if(!root||!query||strlen(root)>=SEARCH_PATH||strlen(query)>64||!match)return NULL;
#ifdef ESP_PLATFORM
    file_search_t *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    file_search_t *s=calloc(1,sizeof(*s));
#endif
    if(!s)return NULL;
    s->stack[0].dir=opendir(root);
    if(!s->stack[0].dir){free(s);return NULL;}
    snprintf(s->stack[0].path,SEARCH_PATH,"%s",root);snprintf(s->query,sizeof(s->query),"%s",query);
    s->depth=1;s->match=match;s->ctx=ctx;return s;
}
file_search_t *file_search_begin(const char *root,const char *query,file_search_match_t match,void *ctx){
    return query&&*query?search_begin(root,query,match,ctx):NULL;
}
file_search_t *file_search_begin_all(const char *root,file_search_match_t visit,void *ctx){
    return search_begin(root,"",visit,ctx);
}
file_search_stats_t file_search_stats(const file_search_t *s){return s?s->stats:(file_search_stats_t){.done=true};}
void file_search_step(file_search_t *s,unsigned entries){
    if(!s||s->stats.done||!entries)return;
#ifdef ESP_PLATFORM
    const int64_t start=esp_timer_get_time();
#endif
    unsigned read=0;
    while(s->depth&&read<entries){
#ifdef ESP_PLATFORM
        if(read&&esp_timer_get_time()-start>=6000)break;
#endif
        unsigned level=s->depth-1;errno=0;struct dirent *entry=readdir(s->stack[level].dir);
        if(!entry){if(errno)++s->stats.errors;closedir(s->stack[level].dir);--s->depth;continue;}
        ++read;
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")||!strncmp(entry->d_name,"._",2))continue;
        char path[SEARCH_PATH];int n=snprintf(path,sizeof(path),"%s/%s",s->stack[level].path,entry->d_name);
        if(n<0||(size_t)n>=sizeof(path)){++s->stats.errors;continue;}
        struct stat info;
#ifdef ESP_PLATFORM
        int error=stat(path,&info);
#else
        int error=lstat(path,&info);
#endif
        if(error){++s->stats.errors;continue;}
        bool directory=S_ISDIR(info.st_mode);
        if(!directory&&!S_ISREG(info.st_mode))continue;
        ++s->stats.visited;
        if(!s->query[0]||read_pico_search_match(entry->d_name,s->query)){++s->stats.matched;s->match(path,&info,s->ctx);}
        if(!directory)continue;
        if(s->depth>=SEARCH_DEPTH){++s->stats.errors;continue;}
        DIR *next=opendir(path);if(!next){++s->stats.errors;continue;}
        s->stack[s->depth].dir=next;snprintf(s->stack[s->depth].path,SEARCH_PATH,"%s",path);++s->depth;
    }
    s->stats.done=s->depth==0;
}
