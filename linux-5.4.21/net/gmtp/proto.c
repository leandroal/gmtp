// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  net/gmtp/proto.c
 *
 *  An implementation of the GMTP protocol
 *  Wendell Silva Soares <wendell@ic.ufal.br>
 */
#include <asm/ioctls.h>
#include <asm-generic/unaligned.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/memblock.h>
#include <linux/swap.h>
#include <linux/dirent.h>
#include <linux/inetdevice.h>
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/poll.h>

#include <net/inet_hashtables.h>
#include <net/sock.h>
#include <net/tcp.h>

#include <uapi/linux/gmtp.h>
#include <linux/gmtp.h>
#include "gmtp.h"
#include "gmtp_hashtables.h"
#include "mcc.h"

struct percpu_counter gmtp_orphan_count;
EXPORT_SYMBOL_GPL(gmtp_orphan_count);

struct inet_hashinfo gmtp_inet_hashinfo;
EXPORT_SYMBOL_GPL(gmtp_inet_hashinfo);

struct gmtp_sk_hashtable gmtp_sk_hash;
EXPORT_SYMBOL_GPL(gmtp_sk_hash);

struct gmtp_info* gmtp_info;
EXPORT_SYMBOL_GPL(gmtp_info);

struct gmtp_hashtable client_hashtable;
EXPORT_SYMBOL_GPL(client_hashtable);

struct gmtp_hashtable server_hashtable;
EXPORT_SYMBOL_GPL(server_hashtable);

const char *gmtp_packet_name(const __u8 type)
{
    static const char *const gmtp_packet_names[] = {
        [GMTP_PKT_REQUEST]  = "REQUEST",
        [GMTP_PKT_REQUESTNOTIFY]  = "REQUESTNOTIFY",
        [GMTP_PKT_RESPONSE] = "RESPONSE",
        [GMTP_PKT_REGISTER] = "REGISTER",
        [GMTP_PKT_REGISTER_REPLY] = "REGISTER_REPLY",
        [GMTP_PKT_ROUTE_NOTIFY] = "ROUTE_NOTIFY",
        [GMTP_PKT_RELAYQUERY] = "RELAYQUERY",
        [GMTP_PKT_RELAYQUERY_REPLY] = "RELAYQUERY_REPLY",
        [GMTP_PKT_DATA]     = "DATA",
        [GMTP_PKT_ACK]      = "ACK",
        [GMTP_PKT_DATAACK]  = "DATAACK",
        [GMTP_PKT_MEDIADESC]  = "MEDIADESC",
        [GMTP_PKT_DATAPULL_REQUEST]  = "DATAPULL_REQUEST",
        [GMTP_PKT_DATAPULL_RESPONSE]  = "DATAPULL_RESPONSE",
        [GMTP_PKT_ELECT_REQUEST]  = "ELECT_REQUEST",
        [GMTP_PKT_ELECT_RESPONSE]  = "ELECT_RESPONSE",
        [GMTP_PKT_CLOSE]    = "CLOSE",
        [GMTP_PKT_RESET]    = "RESET",
        [GMTP_PKT_FEEDBACK]    = "REPORTER_FEEDBACK",
        [GMTP_PKT_DELEGATE]    = "DELEGATE",
        [GMTP_PKT_DELEGATE_REPLY]    = "DELEGATE_REPLY",
    };

    if (type >= GMTP_NR_PKT_TYPES)
        return "INVALID";
    else
        return gmtp_packet_names[type];
}
EXPORT_SYMBOL_GPL(gmtp_packet_name);

void gmtp_done(struct sock *sk)
{
    gmtp_print_function();

    gmtp_set_state(sk, GMTP_CLOSED);
    gmtp_clear_xmit_timers(sk);

    sk->sk_shutdown = SHUTDOWN_MASK;

    if(!sock_flag(sk, SOCK_DEAD))
        sk->sk_state_change(sk);
    else
        inet_csk_destroy_sock(sk);
}
EXPORT_SYMBOL_GPL(gmtp_done);

const char *gmtp_state_name(const int state)
{
    static const char *const gmtp_state_names[] = {
    [GMTP_OPEN]     = "OPEN",
    [GMTP_REQUESTING]   = "REQUESTING",
    [GMTP_LISTEN]       = "LISTEN",
    [GMTP_REQUEST_RECV]     = "REQUEST/REGISTER_RECEIVED",
    [GMTP_ACTIVE_CLOSEREQ]  = "ACTIVE_CLOSEREQ",
    [GMTP_PASSIVE_CLOSE]    = "PASSIVE_CLOSE",
    [GMTP_CLOSING]      = "CLOSING",
    [GMTP_TIME_WAIT]    = "TIME_WAIT",
    [GMTP_CLOSED]       = "CLOSED",
    [GMTP_NEW_SYN_RECV]    = "NEW_SYN_RECV",
    };

    if (state >= GMTP_PKT_INVALID)
        return "INVALID STATE!";
    else
        return gmtp_state_names[state];
}
EXPORT_SYMBOL_GPL(gmtp_state_name);

/**
 * @str size MUST HAVE len >= GMTP_FLOWNAME_STR_LEN
 */
void flowname_str(__u8* str, const __u8 *flowname)
{
    int i;
    for(i = 0; i < GMTP_FLOWNAME_LEN; ++i)
        sprintf(&str[i*2], "%02x", flowname[i]);
}
EXPORT_SYMBOL_GPL(flowname_str);

void flowname_strn(__u8* str, const __u8 *buffer, int length)
{
    int i;
    for(i = 0; i < length; ++i)
        sprintf(&str[i*2], "%02x", buffer[i]);
}
EXPORT_SYMBOL_GPL(flowname_strn);

/*
 * Print IP packet basic information
 */
void print_ipv4_packet(struct sk_buff *skb, bool in)
{
    struct iphdr *iph = ip_hdr(skb);
    const char *type = in ? "IN" : "OUT";
    pr_info("%s: Src=%pI4 | Dst=%pI4 | TTL=%u | Proto: %d | Len: %d B\n",
            type,
            &iph->saddr, &iph->daddr,
            iph->ttl,
            iph->protocol,
            ntohs(iph->tot_len));
}
EXPORT_SYMBOL_GPL(print_ipv4_packet);

/*
 * Print GMTP packet basic information
 */
void print_gmtp_packet(const struct iphdr *iph, const struct gmtp_hdr *gh)
{
    __u8 flowname[GMTP_FLOWNAME_STR_LEN];
    flowname_str(flowname, gh->flowname);

    pr_info("%s (%u) src=%pI4@%-5d, dst=%pI4@%-5d, ttl=%u, len=%u B, seq=%u, "
            "rtt=%u ms, tx=%u B/s, P=%s\n",
                gmtp_packet_name(gh->type), gh->type,
                &iph->saddr, ntohs(gh->sport),
                &iph->daddr, ntohs(gh->dport),
                iph->ttl, ntohs(iph->tot_len),
                gh->seq, gh->server_rtt, gh->transm_r,
                flowname);
}
EXPORT_SYMBOL_GPL(print_gmtp_packet);

/*
 * Print Data of GMTP-Data packets
 */
void print_gmtp_data(struct sk_buff *skb, char* label)
{
    __u8* data = gmtp_data(skb);
    __u32 data_len = gmtp_data_len(skb);
    char *lb = (label != NULL) ? label : "Data";
    if(data_len > 0) {
        unsigned char *data_str = kmalloc(data_len+1, GFP_KERNEL);
        memcpy(data_str, data, data_len);
        data_str[data_len] = '\0';
        pr_info("%s: %s\n", lb, data_str);
        kfree(data_str);
    } else {
        pr_info("%s: <empty>\n", lb);
    }
}
EXPORT_SYMBOL_GPL(print_gmtp_data);

void print_gmtp_hdr_relay(const struct gmtp_hdr_relay *relay)
{
    unsigned char relayid[GMTP_FLOWNAME_STR_LEN];
    flowname_str(relayid, relay->relay_id);
    pr_info("\t%s :: %pI4\n", relayid, &relay->relay_ip);
}
EXPORT_SYMBOL_GPL(print_gmtp_hdr_relay);

void print_route_from_skb(struct sk_buff *skb)
{
    int i;
    struct gmtp_hdr_route *route = gmtp_hdr_route(skb);
    struct gmtp_hdr_relay *relay_list = gmtp_hdr_relay(skb);

    pr_info("GMTP_ROUTE_NOTIFY -> Path to %pI4: \n", &(ip_hdr(skb)->saddr));
    if(route->nrelays <= 0) {
        pr_info("\tEmpty route.\n");
        return;
    }

    for(i = route->nrelays - 1; i >= 0; --i)
        print_gmtp_hdr_relay(&relay_list[i]);
}
EXPORT_SYMBOL_GPL(print_route_from_skb);

void print_route_from_list(struct gmtp_relay_entry *relay_list)
{
    struct gmtp_relay_entry *relay;

    pr_info("GMTP_ROUTE -> Path to %pI4: \n", &relay_list->relay.relay_ip);
    if(relay_list->nrelays <= 0) {
        pr_info("\tEmpty route.\n");
        return;
    }

    print_gmtp_hdr_relay(&relay_list->relay);
    list_for_each_entry(relay, &relay_list->path_list, path_list) {
        print_gmtp_hdr_relay(&relay->relay);
    }
}
EXPORT_SYMBOL_GPL(print_route_from_list);

const char *gmtp_sock_type_name(const int type)
{
    static const char *const gmtp_sock_type_names[] = {
    [GMTP_SOCK_TYPE_REGULAR]     = "REGULAR",
    [GMTP_SOCK_TYPE_REPORTER]    = "TO_REPORTER",
    [GMTP_SOCK_TYPE_CONTROL_CHANNEL] = "CONTROL_CHANNEL",
    [GMTP_SOCK_TYPE_DATA_CHANNEL]    = "DATA_CHANNEL"
    };

    if(type > GMTP_SOCK_TYPE_DATA_CHANNEL)
        return "INVALID TYPE!";
    else
        return gmtp_sock_type_names[type];
}
EXPORT_SYMBOL_GPL(gmtp_sock_type_name);

/*
 * Print GMTP sock basic information
 */
void print_gmtp_sock(struct sock *sk)
{
    pr_info("Socket (%s) - dst=%pI4@%-5d [%s]\n",
            gmtp_sock_type_name(gmtp_sk(sk)->type), &sk->sk_daddr,
            ntohs(sk->sk_dport), gmtp_state_name(sk->sk_state));
}
EXPORT_SYMBOL_GPL(print_gmtp_sock);

/**
 * Build a MD5 uuid from data in buf
 * Requires CONFIG_CRYPTO_MD5 module
 */
bool gmtp_build_md5(unsigned char *result, unsigned char* data, size_t len) {

	int ret = 0;
    struct shash_desc *desc;

    gmtp_pr_func();

    desc = kmalloc(sizeof(*desc), GFP_KERNEL);
    if(desc == NULL)
    	return false;

    desc->tfm = crypto_alloc_shash("md5", 0, CRYPTO_ALG_ASYNC);
    if(desc->tfm == NULL)
        return false;

    crypto_shash_init(desc);
    crypto_shash_update(desc, data, len);
    ret = crypto_shash_final(desc, result);
    crypto_free_shash(desc->tfm);

    if (ret != 0)
        return false;

    return true;
}
EXPORT_SYMBOL_GPL(gmtp_build_md5);

/*
unsigned char *gmtp_build_md5(unsigned char *buf)
{
    struct scatterlist sg;
    struct crypto_shash *tfm;
    struct shash_desc desc;
    unsigned char *output = NULL;
    size_t buf_size = sizeof(buf) - 1;
     __u8 md5[21];

    gmtp_pr_func();

    output = kmalloc(MD5_LEN * sizeof(unsigned char), GFP_KERNEL);
    if(output == NULL)
    	return 0;

    gmtp_pr_info("After kmaloc output");
    tfm = crypto_alloc_shash("md5", 0, CRYPTO_ALG_ASYNC);
    if(output == NULL || IS_ERR(tfm)) {
        gmtp_pr_warning("Allocation failed...");
        goto out;
    }
    gmtp_pr_info("After tfm = crypto_alloc_shash");
    desc.tfm = tfm;

    gmtp_pr_info("Before crypto_shash_init");
    crypto_shash_init(&desc);
    gmtp_pr_info("After crypto_shash_init");

    gmtp_pr_info("Before sg_init_one");
    sg_init_one(&sg, buf, buf_size);
    gmtp_pr_info("After sg_init_one");

    gmtp_pr_info("Before crypto_shash_update");
    crypto_shash_update(&desc, &sg, buf_size);
    gmtp_pr_info("After crypto_shash_update");


    crypto_shash_final(&desc, output);

    if(output == NULL)
    	goto out;

    flowname_strn(md5, output, MD5_LEN);
    printk("Output md5 = %s\n", md5);

out:
    crypto_free_shash(tfm);
    return output;
}
EXPORT_SYMBOL_GPL(gmtp_build_md5);
*/

static inline int bytes_added(int sprintf_return)
{
    return (sprintf_return > 0) ? sprintf_return : 0;
}

unsigned char *gmtp_build_relay_id(void)
{
    struct socket *sock = NULL;
    struct net_device *dev = NULL;
    struct net *net;
    bool success = false;

    int i, counter, retval, length = 0;
    /*unsigned char mac_address[6];*/
    size_t buff_size = MD5_LEN * 10 * sizeof(unsigned char);
    char buffer[buff_size];

    unsigned char *relay_id;

    gmtp_pr_func();

    retval = sock_create(AF_INET, SOCK_STREAM, 0, &sock);
    if (retval != 0 || sock == NULL)
    	goto failure;

    net = sock_net(sock->sk);

	for (i = 2, counter = 0;
			(dev = dev_get_by_index_rcu(net, i)) != NULL && counter < 10;
			++i, ++counter) {
        /* FIXME Last dev->dev_addr is 00:00:00:00:00:00 (loopback?) */
        gmtp_pr_info("MAC ADDR %d: %pM", counter, dev->dev_addr);
        memcpy(&buffer[counter*6], dev->dev_addr, 6);
    }

    sock_release(sock);

    /*relay_id = gmtp_build_md5(buffer);*/
    relay_id = kmalloc(MD5_LEN * sizeof(unsigned char), GFP_KERNEL);
    if(relay_id == NULL)
    	goto failure;

    success = gmtp_build_md5(relay_id, buffer, buff_size);
    if(!success)
    	goto failure;

    return relay_id;

failure:
	return NULL;
}
EXPORT_SYMBOL_GPL(gmtp_build_relay_id);

__be32 gmtp_dev_ip(struct net_device *dev)
{
    struct in_device *in_dev;
    struct in_ifaddr *if_info;

    if(dev == NULL)
        return 0;

    in_dev = (struct in_device *)dev->ip_ptr;
    if_info = in_dev->ifa_list;
    for(; if_info; if_info = if_info->ifa_next) {
        /* just return the first entry for now */
        return if_info->ifa_address;
    }

    return 0;
}
EXPORT_SYMBOL_GPL(gmtp_dev_ip);

bool gmtp_local_ip(__be32 ip)
{
    struct socket *sock = NULL;
    struct net_device *dev = NULL;
    struct net *net;

    int i, length = 0;
    char mac_address[6];

    char buffer[50];
    u8 *str[30];

    bool ret = false;

    sock_create(AF_INET, SOCK_STREAM, 0, &sock);
    net = sock_net(sock->sk);

    for(i = 2; (dev = dev_get_by_index_rcu(net, i)) != NULL; ++i) {
        __be32 dev_ip = gmtp_dev_ip(dev);
        if(ip == dev_ip) {
            ret = true;
            break;
        }
    }

    sock_release(sock);
    return ret;
}
EXPORT_SYMBOL_GPL(gmtp_local_ip);

void gmtp_add_relayid(struct sk_buff *skb)
{
    struct iphdr *iph = ip_hdr(skb);
    struct gmtp_hdr *gh = gmtp_hdr(skb);
    struct gmtp_hdr_register_reply *gh_rply = gmtp_hdr_register_reply(skb);
    struct gmtp_hdr_relay *relay;
    int relay_len = sizeof(struct gmtp_hdr_relay);

    gmtp_print_function();

    relay = (struct gmtp_hdr_relay*) skb_put(skb, relay_len);
    memcpy(relay->relay_id, gmtp_info->relay_id, sizeof(gmtp_info->relay_id));
    relay->relay_ip =  gmtp_dev_ip(skb->dev);
    ++gh_rply->nrelays;

    gh->hdrlen += relay_len;
    put_unaligned(htons(skb->len), &(iph->tot_len));
    ip_send_check(iph);

    print_route_from_skb(skb);
}
EXPORT_SYMBOL_GPL(gmtp_add_relayid);

void gmtp_set_state(struct sock *sk, const int state)
{
    const int oldstate = sk->sk_state;

    print_gmtp_sock(sk);
    gmtp_pr_info("(%s --> %s)", gmtp_state_name(oldstate),
                gmtp_state_name(state));

    WARN_ON(state == oldstate);

    if (state == GMTP_CLOSED)
    {
        /* TODO Implement protocol stats
        if(oldstate == GMTP_OPEN || oldstate == GMTP_CLOSING)
            DCCP_INC_STATS(DCCP_MIB_ESTABRESETS); */

    	if (oldstate == GMTP_LISTEN)
    		gmtp_del_sk_lhash(&gmtp_sk_hash, sk);
    	else
    		gmtp_del_sk_ehash(&gmtp_sk_hash, sk);

        sk->sk_prot->unhash(sk);
        if(inet_csk(sk)->icsk_bind_hash != NULL
                && !(sk->sk_userlocks & SOCK_BINDPORT_LOCK))
            inet_put_port(sk);
    }

    /* Change state AFTER socket is unhashed to avoid closed
     * socket sitting in hash tables.
     */
    sk->sk_state = state;
}
EXPORT_SYMBOL_GPL(gmtp_set_state);

static void gmtp_finish_passive_close(struct sock *sk)
{
    gmtp_pr_func();
    if(sk->sk_state == GMTP_PASSIVE_CLOSE) {
       /* Node (client or server) has received Close packet. */

    	/*
    	 *                    (3) Termination
    	 *
    	 *   Client State                             Server State
    	 *
    	 *       OPEN                                     OPEN
    	 * 1.             <--        Close         <--   CLOSEREQ
    	 * 2.   CLOSING   -->        Close         -->
    	 * 3.             <--        Reset         <--   CLOSED (LISTEN)
    	 * 4.   TIMEWAIT
    	 * 5.   CLOSED
    	 *
    	 * @param sk - struct sock
    	 * @param active:
    	 *      0: send close and finish | 1: send close until receive reset
    	 */
        gmtp_send_close(sk, 1);
        gmtp_set_state(sk, GMTP_CLOSING);
    }
}

int gmtp_init_sock(struct sock *sk)
{
    struct gmtp_sock *gp = gmtp_sk(sk);
    struct inet_connection_sock *icsk = inet_csk(sk);
    int ret = 0;

    gmtp_pr_func();

    gmtp_pr_info("Family: %u", sk->sk_family);

    gmtp_init_xmit_timers(sk);

    icsk->icsk_rto      = GMTP_TIMEOUT_INIT;
    icsk->icsk_syn_retries  = GMTP_SYN_RETRIES;
    sk->sk_state        = GMTP_CLOSED;
    sk->sk_write_space  = gmtp_write_space;
    icsk->icsk_sync_mss = gmtp_sync_mss;

    memset(gp->flowname, 0, GMTP_FLOWNAME_LEN);

    gp->mss         = GMTP_DEFAULT_MSS;
    gp->type        = GMTP_SOCK_TYPE_REGULAR;
    gp->role        = GMTP_ROLE_UNDEFINED;

    gp->req_stamp       = 0;
    gp->ack_rx_tstamp   = 0;
    gp->ack_tx_tstamp   = 0;
    gp->tx_rtt          = 0;
    gp->tx_avg_rtt      = 0;

    gp->rx_max_rate     = 0;

    gp->tx_dpkts_sent   = 0;
    gp->tx_data_sent    = 0;
    gp->tx_bytes_sent   = 0;

    gp->tx_sample_len   = GMTP_DEFAULT_SAMPLE_LEN;
    gp->tx_time_sample  = 0;
    gp->tx_byte_sample  = 0;

    gp->tx_sample_rate  = 0;
    gp->tx_total_rate   = 0;

    gp->tx_first_stamp  = 0UL;
    gp->tx_last_stamp   = 0UL;
    gp->tx_media_rate   = UINT_MAX;
    gp->tx_max_rate     = UINT_MAX; /* Unlimited */
    gp->tx_ucc_rate     = UINT_MAX; /* Unlimited */
    gp->tx_byte_budget  = INT_MIN;
    gp->tx_adj_budget   = 0;

    return ret;
}
EXPORT_SYMBOL_GPL(gmtp_init_sock);

void gmtp_destroy_sock(struct sock *sk)
{
    struct gmtp_sock *gp = gmtp_sk(sk);

    gmtp_pr_func();

    switch(gp->role)
    {
    	case GMTP_ROLE_REPORTER:
    	case GMTP_ROLE_CLIENT_RELAY:
    		 mcc_rx_exit(sk);
    		 /* fall through */
		case GMTP_ROLE_CLIENT:
			if (gp->myself != NULL)
				if (gp->myself->rsock != NULL)
					inet_csk_clear_xmit_timers(gp->myself->rsock);
			break;
    }

	__skb_queue_purge(&sk->sk_write_queue);
	if (sk->sk_send_head != NULL) {
		kfree_skb(sk->sk_send_head);
		sk->sk_send_head = NULL;
	}

	/* Clean up a referenced GMTP bind bucket. */
	if (inet_csk(sk)->icsk_bind_hash != NULL)
		inet_put_port(sk);
}
EXPORT_SYMBOL_GPL(gmtp_destroy_sock);

static void gmtp_terminate_connection(struct sock *sk)
{
    u8 next_state = GMTP_CLOSED;

    gmtp_pr_func();

    switch (sk->sk_state) {
    case GMTP_PASSIVE_CLOSE:
        gmtp_finish_passive_close(sk);
        break;
    case GMTP_OPEN:
        gmtp_send_close(sk, 1);
        if (gmtp_sk(sk)->role == GMTP_ROLE_SERVER &&
        		!gmtp_sk(sk)->server_timewait)
            next_state = GMTP_ACTIVE_CLOSEREQ;
        else
            next_state = GMTP_CLOSING;
        /* fall through */
    default:
        gmtp_set_state(sk, next_state);
    }
}

void gmtp_close(struct sock *sk, long timeout)
{
    struct sk_buff *skb;
    u32 data_was_unread = 0;
    int state;

    gmtp_pr_func();

    lock_sock(sk);

    sk->sk_shutdown = SHUTDOWN_MASK;
    if(sk->sk_state == GMTP_LISTEN) {
        gmtp_set_state(sk, GMTP_CLOSED);

        /* Special case. */
        inet_csk_listen_stop(sk);

        goto adjudge_to_death;
    }

    /*
     * We need to flush the recv. buffs.  We do this only on the
     * descriptor close, not protocol-sourced closes, because the
     * reader process may not have drained the data yet!
     */
    while((skb = __skb_dequeue(&sk->sk_receive_queue)) != NULL) {
        data_was_unread += skb->len;
        __kfree_skb(skb);
    }

    gmtp_pr_info("Data was unread: %u bytes", data_was_unread);
    if(data_was_unread) {
        /* Unread data was tossed, send an appropriate Reset Code */
        gmtp_pr_warning("ABORT with %u bytes unread", data_was_unread);
        gmtp_send_reset(sk, GMTP_RESET_CODE_ABORTED);
        gmtp_set_state(sk, GMTP_CLOSED);
    } else if(sock_flag(sk, SOCK_LINGER) && !sk->sk_lingertime) {
        /* Check zero linger _after_ checking for unread data. */
        sk->sk_prot->disconnect(sk, 0);
    } else if(sk->sk_state != GMTP_CLOSED) {
        /*
         * May need to wait if there are still packets in the
         * TX queue that are delayed by the CCID.
         */
        gmtp_terminate_connection(sk);
    }

    /*
     * Flush write queue. This may be necessary in several cases:
     * - we have been closed by the peer but still have application data;
     * - abortive termination (unread data or zero linger time),
     * - normal termination but queue could not be flushed within time limit
     */
    __skb_queue_purge(&sk->sk_write_queue);
    sk_stream_wait_close(sk, timeout);

adjudge_to_death:
    state = sk->sk_state;
    sock_hold(sk);
    sock_orphan(sk);

    /*
     * It is the last release_sock in its life. It will remove backlog.
     */
    release_sock(sk);
    /*
     * Now socket is owned by kernel and we acquire BH lock
     * to finish close. No need to check for user refs.
     */
    local_bh_disable();
    bh_lock_sock(sk);
    WARN_ON(sock_owned_by_user(sk));

    percpu_counter_inc(sk->sk_prot->orphan_count);

    /* Have we already been destroyed by a softirq or backlog? */
    if(state != GMTP_CLOSED && sk->sk_state == GMTP_CLOSED)
        goto out;

    if(sk->sk_state == GMTP_CLOSED)
        inet_csk_destroy_sock(sk);

    /* Otherwise, socket is reprieved until protocol close. */
out:
    bh_unlock_sock(sk);
    local_bh_enable();
    sock_put(sk);
}
EXPORT_SYMBOL_GPL(gmtp_close);

static inline int gmtp_need_reset(int state)
{
    return state != GMTP_CLOSED && state != GMTP_LISTEN &&
           state != GMTP_REQUESTING;
}

int gmtp_disconnect(struct sock *sk, int flags)
{
    struct inet_connection_sock *icsk = inet_csk(sk);
    struct inet_sock *inet = inet_sk(sk);
    int err = 0;
    const int old_state = sk->sk_state;

    gmtp_print_function();

    if(old_state != GMTP_CLOSED)
        gmtp_set_state(sk, GMTP_CLOSED);


    /* This corresponds to the ABORT function of RFC793, sec. 3.8
     * TCP uses a RST segment, DCCP a Reset packet with Code 2, "Aborted".
     */
    if(old_state == GMTP_LISTEN) {
        inet_csk_listen_stop(sk);
    } else if(gmtp_need_reset(old_state)) {
        gmtp_send_reset(sk, GMTP_RESET_CODE_ABORTED);
        sk->sk_err = ECONNRESET;
    } else if(old_state == GMTP_REQUESTING)
        sk->sk_err = ECONNRESET;

    gmtp_clear_xmit_timers(sk);

    __skb_queue_purge(&sk->sk_receive_queue);
    __skb_queue_purge(&sk->sk_write_queue);
    if(sk->sk_send_head != NULL) {
        __kfree_skb(sk->sk_send_head);
        sk->sk_send_head = NULL;
    }

    inet->inet_dport = 0;

    if(!(sk->sk_userlocks & SOCK_BINDADDR_LOCK))
        inet_reset_saddr(sk);

    sk->sk_shutdown = 0;
    sock_reset_flag(sk, SOCK_DONE);

    icsk->icsk_backoff = 0;
    inet_csk_delack_init(sk);
    __sk_dst_reset(sk);

    WARN_ON(inet->inet_num && !icsk->icsk_bind_hash);

    sk->sk_error_report(sk);
    return err;
}
EXPORT_SYMBOL_GPL(gmtp_disconnect);

/*
 *	Wait for a GMTP event.
 *
 *	Note that we don't need to lock the socket, as the upper poll layers
 *	take care of normal races (between the test and the event) and we don't
 *	go look at any of the socket buffers directly.
 */
__poll_t gmtp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	__poll_t mask;
    struct sock *sk = sock->sk;

    sock_poll_wait(file, sock, wait);
    if(sk->sk_state == GMTP_LISTEN)
        return inet_csk_listen_poll(sk);

    /* Socket is not locked. We are protected from async events
     by poll logic and correct handling of state changes
     made by another threads is impossible in any case.
     */

    mask = 0;
    if(sk->sk_err)
        mask = EPOLLERR;

    if(sk->sk_shutdown == SHUTDOWN_MASK || sk->sk_state == GMTP_CLOSED)
        mask |= EPOLLHUP;
    if(sk->sk_shutdown & RCV_SHUTDOWN)
        mask |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;

    /* Connected? */
    if((1 << sk->sk_state) & ~(GMTPF_REQUESTING | GMTPF_REQUEST_RECV)) {
        if(atomic_read(&sk->sk_rmem_alloc) > 0)
            mask |= EPOLLIN | EPOLLRDNORM;

        if(!(sk->sk_shutdown & SEND_SHUTDOWN)) {
            if(sk_stream_is_writeable(sk)) {
                mask |= EPOLLOUT | EPOLLWRNORM;
            } else { /* send SIGIO later */
                set_bit(SOCKWQ_ASYNC_NOSPACE,
                        &sk->sk_socket->flags);
                set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);

                /* Race breaker. If space is freed after
                 * wspace test but before the flags are set,
                 * IO signal will be lost.
                 */
                if(sk_stream_is_writeable(sk))
                    mask |= EPOLLOUT | EPOLLWRNORM;
            }
        }
    }
    return mask;
}
EXPORT_SYMBOL_GPL(gmtp_poll);

int gmtp_ioctl(struct sock *sk, int cmd, unsigned long arg)
{
    int rc = -ENOTCONN;

    lock_sock(sk);

    if (sk->sk_state == GMTP_LISTEN)
        goto out;

    switch (cmd) {
    case SIOCINQ: {
        struct sk_buff *skb;
        unsigned long amount = 0;

        skb = skb_peek(&sk->sk_receive_queue);

        if (skb != NULL) {
            /*
             * We will only return the amount of this packet since
             * that is all that will be read.
             */
            amount = skb->len;
        }
        rc = put_user(amount, (int __user *)arg);
    }
    break;
    default:
        rc = -ENOIOCTLCMD;
        break;
    }
out:
    release_sock(sk);
    return rc;
}
EXPORT_SYMBOL_GPL(gmtp_ioctl);

int gmtp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int nonblock,
        int flags, int *addr_len)
{
    const struct gmtp_hdr *gh;
    long timeo;

    lock_sock(sk);

    if(sk->sk_state == GMTP_LISTEN) {
        len = -ENOTCONN;
        goto out;
    }

    timeo = sock_rcvtimeo(sk, nonblock);

    do {
        struct sk_buff *skb = skb_peek(&sk->sk_receive_queue);
        if(skb == NULL)
            goto verify_sock_status;

        gh = gmtp_hdr(skb);

        switch(gh->type) {
        case GMTP_PKT_DATA:
        case GMTP_PKT_DATAACK:
            goto found_ok_skb;
        case GMTP_PKT_CLOSE:
        	print_gmtp_sock(sk);
			gmtp_pr_info("(%s)", gmtp_state_name(sk->sk_state));
            if(!(flags & MSG_PEEK))
                gmtp_finish_passive_close(sk);
            /* fall through */
        case GMTP_PKT_RESET:
            gmtp_print_debug("found fin (%s) ok!\n",
                    gmtp_packet_name(gh->type));
            len = 0;
            goto found_fin_ok;
        default:
            gmtp_print_debug("packet_type=%s\n",
                    gmtp_packet_name(gh->type));
            sk_eat_skb(sk, skb);
        }
verify_sock_status:
        if(sock_flag(sk, SOCK_DONE)) {
            len = 0;
            break;
        }

        if(sk->sk_err) {
            len = sock_error(sk);
            break;
        }

        if(sk->sk_shutdown & RCV_SHUTDOWN) {
            len = 0;
            break;
        }

        if(sk->sk_state == GMTP_CLOSED) {
            if(!sock_flag(sk, SOCK_DONE)) {
                /* This occurs when user tries to read
                 * from never connected socket.
                 */
                len = -ENOTCONN;
                break;
            }
            len = 0;
            break;
        }

        if(!timeo) {
            len = -EAGAIN;
            break;
        }

        if(signal_pending(current)) {
            len = sock_intr_errno(timeo);
            break;
        }

        sk_wait_data(sk, &timeo, skb);
        continue;
found_ok_skb:
        if(len > skb->len)
            len = skb->len;
        else if(len < skb->len)
            msg->msg_flags |= MSG_TRUNC;

        if(skb_copy_datagram_msg(skb, 0, msg, len)) {
            /* Exception. Bailout! */
            len = -EFAULT;
            break;
        }
        if(flags & MSG_TRUNC)
            len = skb->len;
found_fin_ok:
        if(!(flags & MSG_PEEK))
            sk_eat_skb(sk, skb);
        break;
    } while(1);
out:
    release_sock(sk);
    return len;
}

EXPORT_SYMBOL_GPL(gmtp_recvmsg);

struct gmtp_sendmsg_data {
    struct sock *sk;
    struct sk_buff *skb;
    struct timer_list *sendmsg_timer;
};

static void gmtp_sendmsg_callback(unsigned long data)
{
    struct gmtp_sendmsg_data *sd = (struct gmtp_sendmsg_data*) data;
    if(!timer_pending(&gmtp_sk(sd->sk)->xmit_timer)) {
        gmtp_write_xmit(sd->sk, sd->skb);
        del_timer(sd->sendmsg_timer);
        kfree(sd->sendmsg_timer);
    } else
        mod_timer(sd->sendmsg_timer, jiffies + 1);
}

int gmtp_do_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
{
    struct gmtp_sock *gp = gmtp_sk(sk);
    const int flags = msg->msg_flags;
    const int noblock = flags & MSG_DONTWAIT;
    struct sk_buff *skb;
    int rc, size;
    long timeo;

    if (len > gp->mss)
        return -EMSGSIZE;

    lock_sock(sk);

    /* FIXME Check if sk queue is full */
    timeo = sock_sndtimeo(sk, noblock);

    /*
     * We have to use sk_stream_wait_connect here to set sk_write_pending,
     * so that the trick in gmtp_rcv_request_sent_state_process.
     */
    /* Wait for a connection to finish. */
    if ((1 << sk->sk_state) & ~(GMTPF_OPEN | GMTPF_PASSIVE_CLOSE))
        if ((rc = sk_stream_wait_connect(sk, &timeo)) != 0)
            goto out_release;

    size = sk->sk_prot->max_header + len;
    release_sock(sk);
    skb = sock_alloc_send_skb(sk, size, noblock, &rc);
    lock_sock(sk);
    if (skb == NULL)
        goto out_release;

    skb_reserve(skb, sk->sk_prot->max_header);
    rc = memcpy_from_msg(skb_put(skb, len), msg, len);
    if (rc != 0)
        goto out_discard;

    /** FIXME Enqueue packets when time is pending... */

    /**
     * Use a timer to rate-based congestion control protocols.
     * The timer will expire when congestion control permits to release
     * further packets into the network.
     *
     * Here, a while(timer_pending(...)) does not work for ns-3/dce
     * So, we use a timer...
     */
    if(!timer_pending(&gp->xmit_timer)) {
        gmtp_write_xmit(sk, skb);
        /* FIXME Commented for linux-5.4.21 */
    }

    /* else {
        struct timer_list *sendmsg_timer = kmalloc(
                sizeof(struct timer_list), GFP_KERNEL);
        struct gmtp_sendmsg_data *sd = kmalloc(sizeof(struct gmtp_sendmsg_data),
                GFP_KERNEL);
        sd->sk = sk;
        sd->skb = skb;
        sd->sendmsg_timer = sendmsg_timer;
        timer_setup(sd->sendmsg_timer, gmtp_sendmsg_callback,
                (unsigned long ) sd);
        mod_timer(sd->sendmsg_timer, jiffies + 1);
    }
*/
out_release:
    release_sock(sk);
    return rc ? : len;
out_discard:
    kfree_skb(skb);
    goto out_release;
}

/*struct gmtp_sendmsg_data {
    struct sock *sk;
    struct msghdr *msg;
    size_t len;
};*/

/*int gmtp_do_sendmsg_thread_func(void *data)
{
    struct gmtp_sendmsg_data *smd = (struct gmtp_sendmsg_data*) data;

    return gmtp_do_sendmsg(smd->sk, smd->msg, smd->len);
}*/

size_t gmtp_media_adapt_cc(struct sock *sk, struct msghdr *msg, size_t len)
{
    struct gmtp_sock *gp = gmtp_sk(sk);
    unsigned long tx_rate = min(gp->tx_max_rate, gp->tx_ucc_rate);

    unsigned int datalen, datalen20, datalen40, datalen80;
    unsigned int rate, new_len;

    new_len = len;

    if(tx_rate == UINT_MAX || gp->tx_ucc_type != GMTP_MEDIA_ADAPT_UCC)
        return len;

    if(gp->tx_total_rate <= tx_rate)
        return len;

    rate = DIV_ROUND_CLOSEST(1000 * tx_rate, gp->tx_total_rate);

    if(rate == 0)
        return len;

    datalen20 = DIV_ROUND_CLOSEST(200 * len, 1000);
    datalen40 = DIV_ROUND_CLOSEST(400 * len, 1000);
    datalen80 = DIV_ROUND_CLOSEST(800 * len, 1000);

    new_len = DIV_ROUND_CLOSEST(len * 1000, rate);

    char label[90];

    if(new_len >= datalen80)
        new_len = datalen80;
    else if(new_len >= datalen40)
        new_len = datalen40;
    else if(new_len >= datalen20)
        new_len = datalen20;
    else {
        return 0;
    }

    if(new_len < 0) {
        return 0;
    }

    pr_info("Cur_TX: %lu B/s, UCC_TX: %lu B/s. reducing to %u B (-%lu B) \n",
            gp->tx_total_rate, tx_rate, new_len, len - new_len);
    return new_len;
}

/**
 * FIXME Make it multithreading.
 * FIXME Send msg to remote clients (without relays)
 */
int gmtp_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
{
    struct gmtp_sock *gp = gmtp_sk(sk);
    struct gmtp_server_entry *s = NULL;
    struct gmtp_relay_entry *r;
    int ret = 0, j = 0;

    /*if (server_hashtable != NULL)
		s = (struct gmtp_server_entry*) gmtp_lookup_entry(&server_hashtable,
				gp->flowname);*/

    if(!s)
        return gmtp_do_sendmsg(sk, msg, len);

    /* For every socket(P) in server, send the same data */
    list_for_each_entry(r, &s->relays.relay_list, relay_list) {

        if(likely(r->sk != NULL)) {

            struct msghdr *msgcpy;
            struct inet_sock *inet = inet_sk(r->sk);
            size_t nlen;

            if(unlikely(r->sk->sk_state == GMTP_DELEGATED))
                continue;

            nlen = gmtp_media_adapt_cc(r->sk, msg, len);
            if(nlen < 0)
                continue;

            msgcpy = kmalloc(nlen, gfp_any());
            memcpy(msgcpy, msg, nlen);

            /*pr_info("Sending to %pI4:%d (%u)\n", &inet->inet_daddr,
                    htons(inet->inet_dport),
                    gmtp_sk(r->sk)->gss);*/

            ret = gmtp_do_sendmsg(r->sk, msgcpy, len);
        }
    }

    kfree(msg);
    return ret;
}
EXPORT_SYMBOL_GPL(gmtp_sendmsg);

int static inline gmtp_csk_listen_start(struct sock *sk, int backlog)
{
	int err = 0;

	err = inet_csk_listen_start(sk, backlog);
	gmtp_pr_debug("inet_csk_listen_start(sk, %d) -> %d", backlog, err);
	if (err)
		goto out;

	err = gmtp_sk_hash_listener(&gmtp_sk_hash, sk);

out:
	return err;
}

int inet_gmtp_listen(struct socket *sock, int backlog)
{
    struct sock *sk = sock->sk;
    struct gmtp_sock *gs = gmtp_sk(sk);
    unsigned char old_state;
    int err;

    gmtp_pr_func();

    lock_sock(sk);

    err = -EINVAL;
    if (sock->state != SS_UNCONNECTED || sock->type != SOCK_GMTP)
        goto out;

    old_state = sk->sk_state;
    if (!((1 << old_state) & (GMTPF_CLOSED | GMTPF_LISTEN)))
        goto out;

    sk->sk_max_ack_backlog = backlog;

    /* Really, if the socket is already in listen state
     * we can only allow the backlog to be adjusted.
     */
    if (old_state != GMTP_LISTEN) {
        /*
        * FIXME: here it probably should be sk->sk_prot->listen_start
        * see tcp_listen_start
        */
        gs->role = GMTP_ROLE_LISTEN;

        err = gmtp_csk_listen_start(sk, backlog);
        if (err)
        	goto out;
    }
    err = 0;

out:
    release_sock(sk);
    return err;
}
EXPORT_SYMBOL_GPL(inet_gmtp_listen);

void gmtp_shutdown(struct sock *sk, int how)
{
    gmtp_pr_func();
    gmtp_print_debug("called shutdown(%x)", how);
}
EXPORT_SYMBOL_GPL(gmtp_shutdown);

/* TODO Study thash_entries... This is from DCCP thash_entries */
static int thash_entries;
module_param(thash_entries, int, 0444);
MODULE_PARM_DESC(thash_entries, "Number of ehash buckets");

/**
 * An adaptation from dccp hashinfo initialization
 */
static int gmtp_create_inet_hashinfo(void)
{
    unsigned long goal;
    unsigned long nr_pages = totalram_pages();
    int ehash_order, bhash_order, i;
    int rc;

    gmtp_pr_func();

    rc = -ENOBUFS;

	inet_hashinfo_init(&gmtp_inet_hashinfo);

	/* See https://patchwork.ozlabs.org/patch/1018304/ */
	rc = inet_hashinfo2_init_mod(&gmtp_inet_hashinfo);
	gmtp_pr_info("inet_hashinfo2_init_mod returned: %d", rc);
	if (rc)
		goto out_fail;
	rc = -ENOBUFS;

    gmtp_inet_hashinfo.bind_bucket_cachep =
            kmem_cache_create("gmtp_bind_bucket",
            		sizeof(struct inet_bind_bucket),
					0,
                    SLAB_HWCACHE_ALIGN,
					NULL);
    if (!gmtp_inet_hashinfo.bind_bucket_cachep)
        goto out_fail;

    /*
     * Size and allocate the main established and bind bucket
     * hash tables.
     *
     * The methodology is similar to that of the buffer cache.
     */
    if (nr_pages >= (128 * 1024))
        goal = nr_pages >> (21 - PAGE_SHIFT);
    else
        goal = nr_pages >> (23 - PAGE_SHIFT);

    if (thash_entries)
        goal = (thash_entries *
                sizeof(struct inet_ehash_bucket)) >> PAGE_SHIFT;
    for (ehash_order = 0; (1UL << ehash_order) < goal; ehash_order++)
        ;

    do {
        unsigned long hash_size = (1UL << ehash_order) * PAGE_SIZE /
                sizeof(struct inet_ehash_bucket);

        while (hash_size & (hash_size - 1))
            hash_size--;
        gmtp_inet_hashinfo.ehash_mask = hash_size - 1;
        gmtp_inet_hashinfo.ehash = (struct inet_ehash_bucket *)
                __get_free_pages(GFP_ATOMIC|__GFP_NOWARN, ehash_order);
    } while (!gmtp_inet_hashinfo.ehash && --ehash_order > 0);

    if (!gmtp_inet_hashinfo.ehash) {
        gmtp_print_error("Failed to allocate GMTP bind hash table");
        goto out_free_bind_bucket_cachep;
    }

    for (i = 0; i <= gmtp_inet_hashinfo.ehash_mask; i++)
        INIT_HLIST_NULLS_HEAD(&gmtp_inet_hashinfo.ehash[i].chain, i);

    if (inet_ehash_locks_alloc(&gmtp_inet_hashinfo))
        goto out_free_gmtp_ehash;

    bhash_order = ehash_order;

    do {
        gmtp_inet_hashinfo.bhash_size = (1UL << bhash_order) * PAGE_SIZE /
                sizeof(struct inet_bind_hashbucket);
        if ((gmtp_inet_hashinfo.bhash_size > (64 * 1024)) &&
                bhash_order > 0)
            continue;
        gmtp_inet_hashinfo.bhash = (struct inet_bind_hashbucket *)
                __get_free_pages(GFP_ATOMIC|__GFP_NOWARN, bhash_order);
    } while (!gmtp_inet_hashinfo.bhash && --bhash_order >= 0);

    if (!gmtp_inet_hashinfo.bhash) {
        gmtp_print_error("Failed to allocate GMTP bind hash table");
        goto out_free_gmtp_locks;
    }

    for (i = 0; i < gmtp_inet_hashinfo.bhash_size; i++) {
        spin_lock_init(&gmtp_inet_hashinfo.bhash[i].lock);
        INIT_HLIST_HEAD(&gmtp_inet_hashinfo.bhash[i].chain);
    }

    return 0;

out_free_gmtp_locks:
    inet_ehash_locks_free(&gmtp_inet_hashinfo);
out_free_gmtp_ehash:
    free_pages((unsigned long)gmtp_inet_hashinfo.ehash, ehash_order);
out_free_bind_bucket_cachep:
    kmem_cache_destroy(gmtp_inet_hashinfo.bind_bucket_cachep);
out_fail:
    gmtp_print_error("gmtp_init_hashinfo: FAIL");
    gmtp_inet_hashinfo.bhash = NULL;
    gmtp_inet_hashinfo.ehash = NULL;
    gmtp_inet_hashinfo.bind_bucket_cachep = NULL;

    return rc;
}

static int ghash_entries = 1024;
module_param(ghash_entries, int, 0444);
MODULE_PARM_DESC(ghash_entries, "Number of GMTP hash entries");


/**
 * GMTP own hash structure
 */
static int gmtp_create_sk_hashtable(void)
{
	int rc = -ENOBUFS;
	gmtp_pr_func();
	rc = gmtp_build_sk_hashtable(&gmtp_sk_hash);
	return rc;
}

/*************************************************/
static int __init gmtp_init(void)
{
    int rc = 0;
    unsigned char *rid;
    __u8 relay_id[21];

    gmtp_pr_func();

    BUILD_BUG_ON(sizeof(struct gmtp_skb_cb) > FIELD_SIZEOF(struct sk_buff, cb));

    rc = mcc_lib_init();
    if(rc)
        goto out;

    rc = percpu_counter_init(&gmtp_orphan_count, 0, GFP_KERNEL);
    if(rc) {
        percpu_counter_destroy(&gmtp_orphan_count);
        goto out;
    }

    rc = gmtp_build_hashtable(&client_hashtable, ghash_entries,
    		gmtp_client_hash_ops);
    if(rc)
    	goto out;

    rc = gmtp_build_hashtable(&server_hashtable, ghash_entries,
    		gmtp_server_hash_ops);
    if(rc)
        goto out;

	gmtp_info = kmalloc(sizeof(struct gmtp_info), GFP_KERNEL);
	if (gmtp_info == NULL) {
		rc = -ENOBUFS;
		goto out;
	}

    gmtp_info->relay_enabled = 0;
    gmtp_info->pkt_sent = 0;
    gmtp_info->control_sk = NULL;
    gmtp_info->ctrl_addr = NULL;

    /* FIXME gmtp_build_relay_id does not work! */
    rid = gmtp_build_relay_id();

    if(rid == NULL) {
        gmtp_pr_error("Build Relay ID failed. Creating a random id.");
        get_random_bytes(gmtp_info->relay_id, GMTP_RELAY_ID_LEN);
    } else {
    	/* FIXME sizeof(rid) is 8, but relay_id is 16 bytes... */
    	gmtp_pr_info("rid ok (size: %ld)", sizeof(rid));
        memcpy(gmtp_info->relay_id, rid, sizeof(rid));
    }

    if (gmtp_info->relay_id != NULL)
    	gmtp_pr_info("GMTP Relay ID was built");

    rc = gmtp_create_inet_hashinfo();
    if(rc)
    	goto out;

    rc = gmtp_create_sk_hashtable();

out:
    return rc;
}

static void __exit gmtp_exit(void)
{
    gmtp_pr_func();

    free_pages((unsigned long)gmtp_inet_hashinfo.bhash,
            get_order(gmtp_inet_hashinfo.bhash_size *
                    sizeof(struct inet_bind_hashbucket)));
    free_pages((unsigned long)gmtp_inet_hashinfo.ehash,
            get_order((gmtp_inet_hashinfo.ehash_mask + 1) *
                    sizeof(struct inet_ehash_bucket)));
    inet_ehash_locks_free(&gmtp_inet_hashinfo);
    kmem_cache_destroy(gmtp_inet_hashinfo.bind_bucket_cachep);

    if(gmtp_info != NULL)
    	kfree_gmtp_info(gmtp_info);

    kfree_gmtp_hashtable(&client_hashtable);
    kfree_gmtp_hashtable(&server_hashtable);

    percpu_counter_destroy(&gmtp_orphan_count);
    mcc_lib_exit();
}

module_init(gmtp_init);
module_exit(gmtp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joilnen Leite <joilnen@gmail.com>");
MODULE_AUTHOR("Mário André Menezes <mariomenezescosta@gmail.com>");
MODULE_AUTHOR("Wendell Silva Soares <wendell@ic.ufal.br>");
MODULE_AUTHOR("Leandro Melo de Sales <leandro@ic.ufal.br>");
MODULE_DESCRIPTION("GMTP - Global Media Transmission Protocol");

