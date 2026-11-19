/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ACCLAIM_H
#define _LINUX_ACCLAIM_H

#ifdef CONFIG_ACCLAIM_LWP
void acclaim_record_allocation(unsigned int order);
unsigned long acclaim_adjust_kswapd_reclaim(unsigned long high_target,
						     unsigned long low_target);
#else
static inline void acclaim_record_allocation(unsigned int order)
{
	(void)order;
}

static inline unsigned long acclaim_adjust_kswapd_reclaim(
		unsigned long high_target, unsigned long low_target)
{
	(void)low_target;
	return high_target;
}
#endif

#endif /* _LINUX_ACCLAIM_H */
