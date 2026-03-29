/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSHRINK_LRUVECD_H
#define _LINUX_KSHRINK_LRUVECD_H

#include <linux/list.h>
#include <linux/types.h>

struct page;
struct page_ext_operations;
struct rw_semaphore;

#ifdef CONFIG_KSHRINK_LRUVECD
void kshrink_lruvecd_page_trylock_set(struct page *page);
void kshrink_lruvecd_page_trylock_clear(struct page *page);
bool kshrink_lruvecd_page_trylock_get_result(struct page *page);
bool kshrink_lruvecd_do_page_trylock(struct page *page,
				     struct rw_semaphore *sem,
				     bool *got_lock);
void kshrink_lruvecd_handle_failed_page_trylock(struct list_head *page_list);

#ifdef CONFIG_PAGE_EXTENSION
extern struct page_ext_operations kshrink_lruvecd_page_ext_ops;
#endif
#else
static inline void kshrink_lruvecd_page_trylock_set(struct page *page)
{
}

static inline void kshrink_lruvecd_page_trylock_clear(struct page *page)
{
}

static inline bool kshrink_lruvecd_page_trylock_get_result(struct page *page)
{
	return false;
}

static inline bool kshrink_lruvecd_do_page_trylock(struct page *page,
						   struct rw_semaphore *sem,
						   bool *got_lock)
{
	return false;
}

static inline void
kshrink_lruvecd_handle_failed_page_trylock(struct list_head *page_list)
{
}
#endif

#endif /* _LINUX_KSHRINK_LRUVECD_H */
