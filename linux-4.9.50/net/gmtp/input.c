#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/gmtp.h>
#include <linux/net.h>
#include <linux/security.h>
#include <linux/igmp.h>

#include <net/inet_common.h>
#include <net/inet_sock.h>
#include <net/sock.h>

#include <uapi/linux/gmtp.h>
#include "gmtp.h"
#include "hash.h"
#include "mcc.h"

static void gmtp_enqueue_skb(struct sock *sk, struct sk_buff *skb)
{
	__skb_pull(skb, gmtp_hdr(skb)->hdrlen);
	__skb_queue_tail(&sk->sk_receive_queue, skb);
	skb_set_owner_r(skb, sk);
	sk->sk_data_ready(sk);
}

static void gmtp_fin(struct sock *sk, struct sk_buff *skb)
{
	gmtp_print_function();
	sk->sk_shutdown = SHUTDOWN_MASK;
	sock_set_flag(sk, SOCK_DONE);
	gmtp_enqueue_skb(sk, skb);
}

static int gmtp_rcv_close(struct sock *sk, struct sk_buff *skb)
{
	int queued = 0;

	gmtp_print_function();

	switch(sk->sk_state) {
	/*
	 * We ignore Close when received in one of the following states:
	 *  - CLOSED		(may be a late or duplicate packet)
	 *  - PASSIVE_CLOSEREQ	(the peer has sent a CloseReq earlier)
	 */
	case GMTP_CLOSING:
		/*
		 * Simultaneous-close: receiving a Close after sending one. This
		 * can happen if both client and server perform active-close and
		 * will result in an endless ping-pong of crossing and retrans-
		 * mitted Close packets, which only terminates when one of the
		 * nodes times out (min. 64 seconds). Quicker convergence can be
		 * achieved when one of the nodes acts as tie-breaker.
		 * This is ok as both ends are done with data transfer and each
		 * end is just waiting for the other to acknowledge termination.
		 */
		if(gmtp_role_client(sk))
			break;
		/* fall through */
	case GMTP_REQUESTING:
	case GMTP_ACTIVE_CLOSEREQ:
		gmtp_send_reset(sk, GMTP_RESET_CODE_CLOSED);
		gmtp_done(sk);
		break;
	case GMTP_OPEN:
		/* FIXME Close only if gh->flowname == gp->flowname */
		/* Clear hash table */
		if(gmtp_role_client(sk))
			gmtp_del_client_entry(client_hashtable,
					gmtp_sk(sk)->flowname);
		/* FIXME: Implement gmtp_del_server_entry() */
		/*
		else if(gp->role == GMTP_ROLE_SERVER)
			gmtp_print_error("FIXME: Implement gmtp_del_server_entry()");*/

		/* Give waiting application a chance to read pending data */
		queued = 1;
		gmtp_fin(sk, skb);
		gmtp_set_state(sk, GMTP_PASSIVE_CLOSE);
		/* fall through */
	case GMTP_PASSIVE_CLOSE:
		/*
		 * Retransmitted Close: we have already enqueued the first one.
		 */
		sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_HUP);
	}
	return queued;
}

struct sock* gmtp_sock_connect(struct sock *sk, enum gmtp_sock_type type,
		__be32 addr, __be16 port)
{
	struct sock *newsk;

	gmtp_pr_func();

	newsk = sk_clone_lock(sk, GFP_KERNEL);
	if(newsk == NULL)
		return NULL;

	newsk->sk_protocol = sk->sk_protocol;
	newsk->sk_daddr = addr;
	newsk->sk_dport = port;

	gmtp_sk(newsk)->type = type;
	gmtp_sk(newsk)->req_stamp = 0;
	gmtp_sk(newsk)->ack_rx_tstamp = jiffies_to_msecs(jiffies);
	gmtp_sk(newsk)->ack_tx_tstamp = 0;

	pr_info("myself: %p == %p\n", gmtp_sk(newsk), gmtp_sk(sk));

	bh_unlock_sock(newsk);
	gmtp_init_xmit_timers(newsk);

	return newsk;

}
EXPORT_SYMBOL_GPL(gmtp_sock_connect);

struct sock* gmtp_multicast_connect(struct sock *sk, enum gmtp_sock_type type,
		__be32 addr, __be16 port)
{
	struct sock *newsk;
	struct ip_mreqn mreq;
	int ret;

	gmtp_pr_func();

	newsk = sk_clone_lock(sk, GFP_KERNEL);
	if(newsk == NULL)
		return NULL;

	/* FIXME Validate received multicast channel */
	newsk->sk_protocol = sk->sk_protocol;
	newsk->sk_reuse = true; /* SO_REUSEADDR */
	newsk->sk_reuseport = true;
	newsk->sk_rcv_saddr = htonl(INADDR_ANY);

	mreq.imr_multiaddr.s_addr = addr;
	mreq.imr_address.s_addr = htonl(INADDR_ANY);
	/* NS-3 sim0 interface is 7 */
	mreq.imr_ifindex = 7;

	gmtp_pr_debug("Joining the multicast group in %pI4@%-5d",
			&addr, ntohs(port));
	rtnl_lock();
	ret = ip_mc_join_group(newsk, &mreq);
	rtnl_unlock();
	pr_info("ip_mc_join_group returned %d\n", ret);

	inet_ehash_nolisten(newsk, NULL);

	gmtp_sk(newsk)->type = type;

	bh_unlock_sock(newsk);
	gmtp_set_state(newsk, GMTP_OPEN);

	return newsk;
}
EXPORT_SYMBOL_GPL(gmtp_multicast_connect);

/*
 *    Step 10: Process REQUEST state (second part)
 *       If S.state == REQUESTING,
 *	  If we get here, P is a valid Response from the
 *	      relay, and we should move to
 *	      OPEN state.
 *
 *	 Connect to mcst channel (if we received GMTP_PKT_REQUESTNOTIFY)
 *	 Connect to reporter (if i'm not a reporter)
 *
 *	  S.state := OPEN
 *	  / * Step 12 will send the Ack completing the
 *	      three-way handshake * /
 */
static int gmtp_rcv_request_sent_state_process(struct sock *sk,
					       struct sk_buff *skb,
					       const struct gmtp_hdr *gh,
					       const unsigned int len)
{
	struct gmtp_sock *gp = gmtp_sk(sk);
	const struct inet_connection_sock *icsk = inet_csk(sk);
	struct gmtp_client_entry *client_entry;

	gmtp_pr_func();

	if (gh->type != GMTP_PKT_REQUESTNOTIFY &&
				gh->type != GMTP_PKT_REGISTER_REPLY) {
		goto out_invalid_packet;
	}
	gmtp_pr_debug("Packet received: %s", gmtp_packet_name(gh->type));

	client_entry = gmtp_lookup_client(client_hashtable, gh->flowname);
	if(client_entry == NULL)
		goto out_invalid_packet;

	/*** FIXME Check sequence numbers  ***/

	gp->rx_rtt = (u32) gh->server_rtt;
	gmtp_pr_debug("RTT: %u ms", gp->rx_rtt);

	if(gh->type == GMTP_PKT_REQUESTNOTIFY) {
		struct gmtp_hdr_reqnotify *gh_rnotify;

		gh_rnotify = gmtp_hdr_reqnotify(skb);
		pr_info("ReqNotify => Channel: %pI4@%-5d | Code: %d | Cl: %u\n",
				&gh_rnotify->mcst_addr,
				ntohs(gh_rnotify->mcst_port),
				gh_rnotify->rn_code,
				gh_rnotify->max_nclients);

		switch(gh_rnotify->rn_code) {
		case GMTP_REQNOTIFY_CODE_OK: /* Process packet */
			pr_info("Reporter: %pI4@%-5d\n",
					&gh_rnotify->reporter_addr,
					ntohs(gh_rnotify->reporter_port));
			memcpy(gp->relay_id, gh_rnotify->relay_id,
					GMTP_RELAY_ID_LEN);

			gp->myself->max_nclients = gh_rnotify->max_nclients;
			if(gp->myself->max_nclients > 0) {
				gp->role = GMTP_ROLE_REPORTER;
				gp->myself->clients = kmalloc(
						sizeof(struct gmtp_client),
						GFP_ATOMIC);
				INIT_LIST_HEAD(&gp->myself->clients->list);
			}
			break;
		case GMTP_REQNOTIFY_CODE_WAIT: /* Do nothing... */
			return 0;
			/* FIXME Del entry in table when receiving error... */
		case GMTP_REQNOTIFY_CODE_ERROR:
			goto err;
		default:
			goto out_invalid_packet;
		}

		if(gp->role != GMTP_ROLE_REPORTER) {
			gp->myself->reporter = gmtp_create_client(
					gh_rnotify->reporter_addr,
					gh_rnotify->reporter_port, 1);

			gp->myself->rsock = gmtp_sock_connect(sk,
					GMTP_SOCK_TYPE_REPORTER,
					gh_rnotify->reporter_addr,
					gh_rnotify->reporter_port);

			if(gp->myself->rsock == NULL
					|| gp->myself->reporter == NULL)
				goto err;

			gmtp_set_state(gp->myself->rsock, GMTP_REQUESTING);
			gmtp_send_elect_request(gp->myself->rsock,
					GMTP_REQ_INTERVAL);
		}

		/* Inserting information in client table */
		client_entry->channel_addr = gh_rnotify->mcst_addr;
		client_entry->channel_port = gh_rnotify->mcst_port;

		gp->channel_sk = gmtp_multicast_connect(sk,
				GMTP_SOCK_TYPE_DATA_CHANNEL,
				gh_rnotify->mcst_addr, gh_rnotify->mcst_port);
		if(gp->channel_sk == NULL)
			goto err;

	}

	/* Stop the REQUEST timer */
	inet_csk_clear_xmit_timer(sk, ICSK_TIME_RETRANS);
	WARN_ON(sk->sk_send_head == NULL);
	kfree_skb(sk->sk_send_head);
	sk->sk_send_head = NULL;

	gp->gsr = gp->isr = GMTP_SKB_CB(skb)->seq;
	gmtp_sync_mss(sk, icsk->icsk_pmtu_cookie);

	gmtp_set_state(sk, GMTP_OPEN);

	/* Make sure socket is routed, for correct metrics. */
	icsk->icsk_af_ops->rebuild_header(sk);

	if(!sock_flag(sk, SOCK_DEAD)) {
		sk->sk_state_change(sk);
		sk_wake_async(sk, SOCK_WAKE_IO, POLL_OUT);
	}

	if(sk->sk_write_pending || icsk->icsk_ack.pingpong
			|| icsk->icsk_accept_queue.rskq_defer_accept) {
		/* Save one ACK. Data will be ready after
		 * several ticks, if write_pending is set.
		 */
		__kfree_skb(skb);
		return 0;
	}

	if(gh->type == GMTP_PKT_REGISTER_REPLY) {
		gmtp_add_relayid(skb);
		gmtp_send_route_notify(sk, skb);
		gp->role = GMTP_ROLE_REPORTER;
	} else
		gmtp_send_ack(sk);

	if(gp->role == GMTP_ROLE_REPORTER) {
		if(mcc_rx_init(sk))
			goto err;
		inet_csk_reset_keepalive_timer(sk, GMTP_ACK_TIMEOUT);
	}


	return -1;

	/* FIXME Treat invalid responses */
out_invalid_packet:
 	/* gmtp_v4_do_rcv will send a reset */
 	GMTP_SKB_CB(skb)->reset_code = GMTP_RESET_CODE_PACKET_ERROR;
 	return 1;

err:
 	/*
 	 * We mark this socket as no longer usable, so that the loop in
 	 * gmtp_sendmsg() terminates and the application gets notified.
 	 */
	gmtp_del_client_entry(client_hashtable, gp->flowname);
 	gmtp_set_state(sk, GMTP_CLOSED);
 	sk->sk_err = ECOMM;
 	return 1;
}

/**** FIXME Make GMTP reset codes */
static u16 gmtp_reset_code_convert(const u8 code)
{
	const u16 error_code[] = {
	[GMTP_RESET_CODE_CLOSED]	     = 0,	/* normal termination */
	[GMTP_RESET_CODE_UNSPECIFIED]	     = 0,	/* nothing known */
	[GMTP_RESET_CODE_ABORTED]	     = ECONNRESET,

	[GMTP_RESET_CODE_NO_CONNECTION]	     = ECONNREFUSED,
	[GMTP_RESET_CODE_CONNECTION_REFUSED] = ECONNREFUSED,
	[GMTP_RESET_CODE_TOO_BUSY]	     = EUSERS,
	[GMTP_RESET_CODE_AGGRESSION_PENALTY] = EDQUOT,

	[GMTP_RESET_CODE_PACKET_ERROR]	     = ENOMSG,
	[GMTP_RESET_CODE_MANDATORY_ERROR]    = EOPNOTSUPP,

	[GMTP_RESET_CODE_BAD_FLOWNAME]       = EBADR, /* Invalid request
								descriptor */
	};

	return code >= GMTP_MAX_RESET_CODES ? 0 : error_code[code];
}

static void gmtp_rcv_reset(struct sock *sk, struct sk_buff *skb)
{
	u16 err = gmtp_reset_code_convert(gmtp_hdr_reset(skb)->reset_code);

	gmtp_print_function();

	sk->sk_err = err;

	/*Queue the equivalent of TCP fin so that gmtp_recvmsg exits the loop */
	gmtp_fin(sk, skb);

	if (err && !sock_flag(sk, SOCK_DEAD))
		sk_wake_async(sk, SOCK_WAKE_IO, POLL_ERR);
	gmtp_time_wait(sk, GMTP_TIME_WAIT, 0);
}

static void gmtp_deliver_input_to_mcc(struct sock *sk, struct sk_buff *skb)
{
	/*const struct gmtp_sock *gp = gmtp_sk(sk);*/

	/* Don't deliver to RX MCC when node has shut down read end. */
	if (!(sk->sk_shutdown & RCV_SHUTDOWN))
		mcc_rx_packet_recv(sk, skb);
	/*
	 * FIXME Until the TX queue has been drained, we can not honour SHUT_WR,
	 * since we need received feedback as input to adjust congestion control.
	 */
/*	if (sk->sk_write_queue.qlen > 0 || !(sk->sk_shutdown & SEND_SHUTDOWN))
		mcc_tx_packet_recv(sk, skb);*/
}

static int gmtp_rcv_route_notify(struct sock *sk, struct sk_buff *skb,
			 const struct gmtp_hdr *gh)
{
	struct gmtp_hdr_route *route = gmtp_hdr_route(skb);

	print_route_from_skb(skb);

	if(route->nrelays <= 0)
		return 0;

	gmtp_add_server_entry(server_hashtable, sk, skb);

	return 0;
}

static int gmtp_rcv_delegate_reply(struct sock *sk, struct sk_buff *skb,
		 const struct gmtp_hdr *gh)
{
	struct gmtp_hdr_delegate *dh = gmtp_hdr_delegate(skb);
	struct gmtp_server_entry *s;
	struct gmtp_relay_entry *r;

	print_gmtp_hdr_relay(&dh->relay);

	s = (struct gmtp_server_entry*) gmtp_lookup_entry(server_hashtable,
				gh->flowname);
	if(s != NULL) {
		r = (struct gmtp_relay_entry*) gmtp_lookup_entry(s->relay_hashtable,
						dh->relay.relay_id);
		pr_info("Found: %pI4\n", &r->relay.relay_ip);
		gmtp_set_state(r->sk, GMTP_DELEGATED);
	}

	return 0;
}

/* TODO Implement check sequence number */
static int gmtp_check_seqno(struct sock *sk, struct sk_buff *skb)
{
	struct gmtp_hdr *gh = gmtp_hdr(skb);
	struct gmtp_sock *gp = gmtp_sk(sk);

	if(gh->type == GMTP_PKT_DATA && gp->role == GMTP_ROLE_REPORTER) {
		if(unlikely(gp->rx_state == MCC_RSTATE_NO_DATA)) {
			pr_info("Setting first seqno to %u \n", gh->seq);
			gp->gsr = gh->seq;
			gp->isr = gh->seq;
			gp->iss = gh->seq;
			gp->gss = gh->seq;
			return 0;
		} /*else if(gh->seq < gp->gsr) {
			pr_info("Seqno error => Received: %u. GSR: %u.\n",
					gh->seq, gp->gsr);
			return 1;
		}*/
	}

	return 0;
}

static int __gmtp_rcv_established(struct sock *sk, struct sk_buff *skb,
		const struct gmtp_hdr *gh, const unsigned int len)
{
	struct gmtp_sock *gp = gmtp_sk(sk);

	switch (gh->type) {
	case GMTP_PKT_DATAACK:
	case GMTP_PKT_DATA:
		gmtp_enqueue_skb(sk, skb);
		return 0;
	case GMTP_PKT_ACK:
	case GMTP_PKT_FEEDBACK:
		if(gp->role == GMTP_ROLE_SERVER) {
			__be32 otstamp;
			__be32 new_tx = gh->transm_r;

			if(gh->type == GMTP_PKT_ACK)
				otstamp = gmtp_hdr_ack(skb)->orig_tstamp;
			else
				otstamp = gmtp_hdr_feedback(skb)->orig_tstamp;

			gp->tx_rtt = jiffies_to_msecs(jiffies) - otstamp;
			gp->tx_avg_rtt = rtt_ewma(gp->tx_avg_rtt, gp->tx_rtt,
					GMTP_RTT_WEIGHT);

			/* Avoid super TX reduction */
			if(new_tx < DIV_ROUND_CLOSEST(gp->tx_media_rate, 4)) {
				pr_info("Avoiding super TX reduction...\n");
				pr_info("gh->tx = %u\n", gh->transm_r);
				pr_info("max_tx: %lu\n", gp->tx_media_rate);
				new_tx = DIV_ROUND_CLOSEST(gp->tx_media_rate, 4);
			}

			gp->tx_ucc_rate = min((__be32 )gp->tx_max_rate, new_tx);

			pr_info("cong. control tx: %lu B/s, from: %pI4\n", gp->tx_ucc_rate,
					&ip_hdr(skb)->saddr);
		}
		goto discard;
	case GMTP_PKT_ROUTE_NOTIFY:
		gp->tx_rtt = jiffies_to_msecs(jiffies) - gmtp_sk(sk)->reply_stamp;
		gp->tx_rtt = gp->tx_rtt > 0? gp->tx_rtt : 1;
		gp->tx_avg_rtt = rtt_ewma(gp->tx_avg_rtt, gp->tx_rtt,
		GMTP_RTT_WEIGHT);

		gmtp_pr_debug("RTT: %u ms | RTT_AVG: %u ms", gp->tx_rtt,
				gp->tx_avg_rtt);

		/** TODO Update routes */
		gmtp_rcv_route_notify(sk, skb, gh);

		goto discard;
	case GMTP_PKT_DELEGATE_REPLY:
		gmtp_rcv_delegate_reply(sk, skb, gh);
		goto discard;
	case GMTP_PKT_REGISTER: {
		struct sk_buff *new_skb = gmtp_make_register_reply_open(sk, skb);
		if(new_skb != NULL) {
			gmtp_sk(sk)->reply_stamp = jiffies_to_msecs(jiffies);
			return gmtp_transmit_built_skb(sk, new_skb);
		}
	}
		break;
	case GMTP_PKT_RESET:
		/*
		 *  Step 9: Process Reset
		 *	If P.type == Reset,
		 *		Tear down connection
		 *		S.state := TIMEWAIT
		 *		Set TIMEWAIT timer
		 *		Drop packet and return
		 */
		gmtp_rcv_reset(sk, skb);
		return 0;
	case GMTP_PKT_CLOSE:
		if (gmtp_rcv_close(sk, skb))
			return 0;
		goto discard;
	}
discard:
	__kfree_skb(skb);
	return 0;
}

int gmtp_rcv_established(struct sock *sk, struct sk_buff *skb,
			 const struct gmtp_hdr *gh, const unsigned int len)
{
	struct gmtp_sock *gp = gmtp_sk(sk);

	/* Check sequence numbers... */
	if(gmtp_check_seqno(sk, skb)) {
		goto discard;
	}

	gp->gsr = gh->seq;
	gp->rx_rtt = (u32) gh->server_rtt;
	if(likely(gh->type == GMTP_PKT_DATA))
		gp->rx_last_orig_tstamp = gmtp_hdr_data(skb)->tstamp;
	else
		gp->ndp_count++;

	if(gp->role == GMTP_ROLE_REPORTER) {
		gmtp_deliver_input_to_mcc(sk, skb);
	}

	return __gmtp_rcv_established(sk, skb, gh, len);
discard:
	__kfree_skb(skb);
	return 0;
}
EXPORT_SYMBOL_GPL(gmtp_rcv_established);

static int gmtp_rcv_request_rcv_state_process(struct sock *sk,
						   struct sk_buff *skb,
						   const struct gmtp_hdr *gh,
						   const unsigned int len)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct gmtp_sock *gp = gmtp_sk(sk);
	int queued = 0;

	gmtp_print_function();

	switch (gh->type) {
	case GMTP_PKT_RESET:
		inet_csk_clear_xmit_timer(sk, ICSK_TIME_DACK);
		break;
	case GMTP_PKT_DATA:
		if (sk->sk_state == GMTP_REQUEST_RECV)
			break;
	/* ROUTE_NOTIFY is a special ack */
	case GMTP_PKT_ROUTE_NOTIFY:
	case GMTP_PKT_DATAACK:
	case GMTP_PKT_ACK:
		gp->tx_rtt = jiffies_to_msecs(jiffies) - gmtp_sk(sk)->reply_stamp;
		gp->tx_avg_rtt = rtt_ewma(gp->tx_avg_rtt, gp->tx_rtt,
							GMTP_RTT_WEIGHT);
		gmtp_pr_debug("RTT: %u ms | RTT_AVG: %u ms", gp->tx_rtt,
				gp->tx_avg_rtt);

		inet_csk_clear_xmit_timer(sk, ICSK_TIME_DACK);

		icsk->icsk_af_ops->rebuild_header(sk);
		smp_mb();
		gmtp_set_state(sk, GMTP_OPEN);
		sk->sk_state_change(sk);
		sk_wake_async(sk, SOCK_WAKE_IO, POLL_OUT);

		if(gh->type == GMTP_PKT_ROUTE_NOTIFY)
			gmtp_rcv_route_notify(sk, skb, gh);

		if (gh->type == GMTP_PKT_DATAACK)
		{
			__gmtp_rcv_established(sk, skb, gh, len);
			queued = 1;
		}

		break;
	}
	return queued;
}
EXPORT_SYMBOL_GPL(gmtp_rcv_request_rcv_state_process);

int gmtp_rcv_state_process(struct sock *sk, struct sk_buff *skb,
			   struct gmtp_hdr *gh, unsigned int len)
{
	struct gmtp_skb_cb *gcb = GMTP_SKB_CB(skb);
	int queued = 0;

	gmtp_pr_func();
	gmtp_pr_debug("State: %s | Packet: %s", gmtp_state_name(sk->sk_state),
			gmtp_packet_name(gh->type));

	print_gmtp_packet(ip_hdr(skb), gh);

	/*
	 *  Step 3: Process LISTEN state
	 *
	 *  If S.state == LISTEN,
	 *
	 *  If P.type == Request
	 *  (* Generate a new socket and switch to that socket *)
	 *	      Set S := new socket for this port pair
	 *	      S.state = GMTP_REQUEST_RECV
	 *	      Continue with S.state == REQ_RECV
	 *	      (* A REQUEST_REPLY packet will be generated in Step 11 (GMTP) *)
	 *	 Otherwise,
	 *	      Generate Reset(No Connection) unless P.type == Reset
	 *	      Drop packet and return
	 */

	if (sk->sk_state == GMTP_LISTEN)  {

		if(gh->type == GMTP_PKT_REQUEST
				|| gh->type == GMTP_PKT_REGISTER) {
			if(inet_csk(sk)->icsk_af_ops->conn_request(sk, skb) < 0)
				return 1;
			goto discard;
		}
		if(gh->type == GMTP_PKT_RESET)
			goto discard;

		/* Avoid GMTP-Inter problems */
		if(gmtp_info->relay_enabled)
			goto discard;

		/* Caller (gmtp_v4_do_rcv) will send Reset */
		gcb->reset_code = GMTP_RESET_CODE_NO_CONNECTION;
		return 1;
	} else if (sk->sk_state == GMTP_CLOSED || sk->sk_state == GMTP_ACTIVE_CLOSEREQ) {
		gcb->reset_code = GMTP_RESET_CODE_NO_CONNECTION;
		return 1;
	}

	/* Step 6: Check sequence numbers (omitted in LISTEN/REQUEST state) */
	if (sk->sk_state != GMTP_REQUESTING && gmtp_check_seqno(sk, skb))
		goto discard;

	/*
	 *   Step 7: Check for unexpected packet types
	 */
	if ((!gmtp_role_client(sk) && gh->type == GMTP_PKT_REQUESTNOTIFY) ||
		(gmtp_role_client(sk) && gh->type == GMTP_PKT_REQUEST) ||
		(sk->sk_state == GMTP_REQUEST_RECV && gh->type == GMTP_PKT_DATA))
	{
		gmtp_print_error("Unexpected packet type");
		goto discard;
	}

	/*
	 *  Step 9: Process Reset
	 *	If P.type == Reset,
	 *		Tear down connection
	 *		S.state := TIMEWAIT
	 *		Set TIMEWAIT timer
	 *		Drop packet and return
	 */
	if (gh->type == GMTP_PKT_RESET) {
		gmtp_rcv_reset(sk, skb);
		return 0;
	} else if (gh->type == GMTP_PKT_CLOSE) {		/* Step 14 */
		if (gmtp_rcv_close(sk, skb))
			return 0;
		goto discard;
	}

	switch (sk->sk_state) {
	case GMTP_REQUESTING: /* client */
		queued = gmtp_rcv_request_sent_state_process(sk, skb, gh, len);
		if (queued >= 0)
			return queued;

		__kfree_skb(skb);
		return 0;

	case GMTP_REQUEST_RECV: /* Request or Register was received. */
		queued = gmtp_rcv_request_rcv_state_process(sk, skb, gh, len);
		break;
	}

discard:
	__kfree_skb(skb);
	return 0;

}
EXPORT_SYMBOL_GPL(gmtp_rcv_state_process);
