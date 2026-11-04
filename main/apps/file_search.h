/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：可取消、分片的只读多级目录搜索；回调只借用当前路径。
 * English: Cancellable, sliced, read-only recursive search; callbacks borrow the current path.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
typedef struct file_search file_search_t;
typedef struct { unsigned visited, matched, errors; bool done; } file_search_stats_t;
typedef void (*file_search_match_t)(const char *path,const struct stat *info,void *ctx);
/// 创建独立查询，最多32层；失败不返回悬空句柄。/ Own a query of up to32 levels; failure returns no live handle.
file_search_t *file_search_begin(const char *root,const char *query,file_search_match_t match,void *ctx);
/// 无关键词的目录遍历，复用相同分片/深度/错误与关闭规则。/ Enumerate without a query using the same slicing/depth/error/close contract.
file_search_t *file_search_begin_all(const char *root,file_search_match_t visit,void *ctx);
/// 每次有界条目数；设备上还在IO之间检查6ms预算。/ Bound entries per call and check a6ms device budget between IO operations.
void file_search_step(file_search_t *search,unsigned entries);
file_search_stats_t file_search_stats(const file_search_t *search);
/// 同步关闭所有目录并释放查询；可传NULL。/ Close every directory and free the query; NULL is accepted.
void file_search_close(file_search_t *search);
