/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WORKAROUNDS_H
#define _WORKAROUNDS_H

#include <linux/jump_label.h>

int is_bpf_spoof_enabled(void);
const char *get_bpf_spoof_version(void);

#if defined(CONFIG_DEFAULT_SUPPORT_AOSP)
static inline bool is_aosp_mode(void)
{
	return true;
}

static inline bool is_aosp_mode_fast(void)
{
	return true;
}
#else
bool is_aosp_mode(void);
bool is_usb_sl_disabled(void);

/* Optimized hot path version using static branch */
extern struct static_key_false aosp_mode_key;
static inline bool is_aosp_mode_fast(void)
{
	return static_branch_unlikely(&aosp_mode_key);
}
#endif

#endif /* _WORKAROUNDS_H */
