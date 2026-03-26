/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WORKAROUNDS_H
#define _WORKAROUNDS_H

#include <linux/jump_label.h>

int is_bpf_spoof_enabled(void);
const char *get_bpf_spoof_version(void);

#endif /* _WORKAROUNDS_H */
