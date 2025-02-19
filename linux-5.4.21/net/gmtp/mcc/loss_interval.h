#ifndef _MCC_LI_HIST
#define _MCC_LI_HIST_
/*
 *  Copyright (c) 2007   The University of Aberdeen, Scotland, UK
 *  Copyright (c) 2005-7 The University of Waikato, Hamilton, New Zealand.
 *  Copyright (c) 2005-7 Ian McDonald <ian.mcdonald@jandi.co.nz>
 *  Copyright (c) 2005 Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 *  Adapted to GMTP by
 *  Copyright (c) 2015   Federal University of Alagoas, Maceió, Brazil
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 */
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/gmtp.h>

/**
 *  mcc_loss_interval  -  Loss history record for TFRC-based protocols
 *  @li_seqno:		Highest received seqno before the start of loss
 *  @li_is_closed:	Whether @li_seqno is older than 1 RTT
 *  @li_length:		Loss interval sequence length
 *  @li_tstamp:		Time stamp of packet with seqno.
 */
struct mcc_loss_interval {
	__be32		 li_seqno;
	__be32		 li_is_closed:1;
	__u32		 li_length;
	__u32		 li_tstamp;
	__u32		 li_rtt;
};

static inline void mcc_lh_init(struct mcc_loss_hist *lh)
{
	memset(lh, 0, sizeof(struct mcc_loss_hist));
}

static inline u8 mcc_lh_is_initialised(struct mcc_loss_hist *lh)
{
	return lh->counter > 0;
}

static inline u8 mcc_lh_length(struct mcc_loss_hist *lh)
{
	return min(lh->counter, (u8)LIH_SIZE);
}

struct mcc_rx_hist;

int mcc_lh_interval_add(struct mcc_loss_hist *, struct mcc_rx_hist *,
			 u32 (*first_li)(struct sock *), struct sock *);
u8 mcc_lh_update_i_mean(struct mcc_loss_hist *lh, struct sk_buff *);
void mcc_lh_cleanup(struct mcc_loss_hist *lh);

#endif /* _MCC_LI_HIST */
