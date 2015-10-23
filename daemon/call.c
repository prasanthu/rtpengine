#include "call.h"

#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include <stdlib.h>
#include <pcre.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <xmlrpc_client.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>

#include "poller.h"
#include "aux.h"
#include "log.h"
#include "kernel.h"
#include "control_tcp.h"
#include "streambuf.h"
#include "redis.h"
#include "xt_RTPENGINE.h"
#include "bencode.h"
#include "sdp.h"
#include "str.h"
#include "stun.h"
#include "rtcp.h"
#include "rtp.h"
#include "call_interfaces.h"




#ifndef PORT_RANDOM_MIN
#define PORT_RANDOM_MIN 6
#define PORT_RANDOM_MAX 20
#endif

#ifndef MAX_RECV_ITERS
#define MAX_RECV_ITERS 50
#endif





typedef int (*rewrite_func)(str *, struct packet_stream *);

/* also serves as array index for callstream->peers[] */
struct iterator_helper {
	GSList			*del_timeout;
	GSList			*del_scheduled;
	struct stream_fd	*ports[0x10000];
};
struct xmlrpc_helper {
	enum xmlrpc_format fmt;
	GStringChunk		*c;
	GSList			*tags_urls;
};

struct streamhandler_io {
	rewrite_func	rtp;
	rewrite_func	rtcp;
	int		(*kernel)(struct rtpengine_srtp *, struct packet_stream *);
};
struct streamhandler {
	const struct streamhandler_io	*in;
	const struct streamhandler_io	*out;
};

const struct transport_protocol transport_protocols[] = {
	[PROTO_RTP_AVP] = {
		.index		= PROTO_RTP_AVP,
		.name		= "RTP/AVP",
		.srtp		= 0,
		.avpf		= 0,
	},
	[PROTO_RTP_SAVP] = {
		.index		= PROTO_RTP_SAVP,
		.name		= "RTP/SAVP",
		.srtp		= 1,
		.avpf		= 0,
	},
	[PROTO_RTP_AVPF] = {
		.index		= PROTO_RTP_AVPF,
		.name		= "RTP/AVPF",
		.srtp		= 0,
		.avpf		= 1,
	},
	[PROTO_RTP_SAVPF] = {
		.index		= PROTO_RTP_SAVPF,
		.name		= "RTP/SAVPF",
		.srtp		= 1,
		.avpf		= 1,
	},
	[PROTO_UDP_TLS_RTP_SAVP] = {
		.index		= PROTO_UDP_TLS_RTP_SAVP,
		.name		= "UDP/TLS/RTP/SAVP",
		.srtp		= 1,
		.avpf		= 0,
	},
	[PROTO_UDP_TLS_RTP_SAVPF] = {
		.index		= PROTO_UDP_TLS_RTP_SAVPF,
		.name		= "UDP/TLS/RTP/SAVPF",
		.srtp		= 1,
		.avpf		= 1,
	},
	[PROTO_UDPTL] = {
		.index		= PROTO_UDPTL,
		.name		= "udptl",
		.srtp		= 0,
		.avpf		= 0,
	},
};
const int num_transport_protocols = G_N_ELEMENTS(transport_protocols);

const char * get_term_reason_text(char *buf, enum termination_reason t) {
	if (t==TIMEOUT) { buf = "TIMEOUT"; return buf; }
	if (t==REGULAR) { buf = "REGULAR"; return buf; }
	if (t==FORCED) { buf = "FORCED"; return buf; }
	if (t==SILENT_TIMEOUT) { buf = "SILENT_TIMEOUT"; return buf; }

	buf = "UNKNOWN";
	return buf;
}

const char * get_tag_type_text(char *buf, enum tag_type t) {
	if (t==FROM_TAG) { buf = "FROM_TAG"; return buf; }
	if (t==TO_TAG) { buf = "TO_TAG"; return buf; }

	buf = "UNKNOWN";
	return buf;
}

static void determine_handler(struct packet_stream *in, const struct packet_stream *out);

static int __k_null(struct rtpengine_srtp *s, struct packet_stream *);
static int __k_srtp_encrypt(struct rtpengine_srtp *s, struct packet_stream *);
static int __k_srtp_decrypt(struct rtpengine_srtp *s, struct packet_stream *);

static int call_avp2savp_rtp(str *s, struct packet_stream *);
static int call_savp2avp_rtp(str *s, struct packet_stream *);
static int call_avp2savp_rtcp(str *s, struct packet_stream *);
static int call_savp2avp_rtcp(str *s, struct packet_stream *);
static int call_avpf2avp_rtcp(str *s, struct packet_stream *);
//static int call_avpf2savp_rtcp(str *s, struct packet_stream *);
static int call_savpf2avp_rtcp(str *s, struct packet_stream *);
//static int call_savpf2savp_rtcp(str *s, struct packet_stream *);


/* ********** */

static const struct streamhandler_io __shio_noop = {
	.kernel		= __k_null,
};
static const struct streamhandler_io __shio_decrypt = {
	.kernel		= __k_srtp_decrypt,
	.rtp		= call_savp2avp_rtp,
	.rtcp		= call_savp2avp_rtcp,
};
static const struct streamhandler_io __shio_encrypt = {
	.kernel		= __k_srtp_encrypt,
	.rtp		= call_avp2savp_rtp,
	.rtcp		= call_avp2savp_rtcp,
};
static const struct streamhandler_io __shio_avpf_strip = {
	.kernel		= __k_null,
	.rtcp		= call_avpf2avp_rtcp,
};
static const struct streamhandler_io __shio_decrypt_avpf_strip = {
	.kernel		= __k_srtp_decrypt,
	.rtp		= call_savp2avp_rtp,
	.rtcp		= call_savpf2avp_rtcp,
};

/* ********** */

static const struct streamhandler __sh_noop = {
	.in		= &__shio_noop,
	.out		= &__shio_noop,
};
static const struct streamhandler __sh_savp2avp = {
	.in		= &__shio_decrypt,
	.out		= &__shio_noop,
};
static const struct streamhandler __sh_avp2savp = {
	.in		= &__shio_noop,
	.out		= &__shio_encrypt,
};
static const struct streamhandler __sh_avpf2avp = {
	.in		= &__shio_avpf_strip,
	.out		= &__shio_noop,
};
static const struct streamhandler __sh_avpf2savp = {
	.in		= &__shio_avpf_strip,
	.out		= &__shio_encrypt,
};
static const struct streamhandler __sh_savpf2avp = {
	.in		= &__shio_decrypt_avpf_strip,
	.out		= &__shio_noop,
};
static const struct streamhandler __sh_savp2savp = {
	.in		= &__shio_decrypt,
	.out		= &__shio_encrypt,
};
static const struct streamhandler __sh_savpf2savp = {
	.in		= &__shio_decrypt_avpf_strip,
	.out		= &__shio_encrypt,
};

/* ********** */

static const struct streamhandler *__sh_matrix_in_rtp_avp[] = {
	[PROTO_RTP_AVP]			= &__sh_noop,
	[PROTO_RTP_AVPF]		= &__sh_noop,
	[PROTO_RTP_SAVP]		= &__sh_avp2savp,
	[PROTO_RTP_SAVPF]		= &__sh_avp2savp,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_avp2savp,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_avp2savp,
	[PROTO_UDPTL]			= &__sh_noop,
};
static const struct streamhandler *__sh_matrix_in_rtp_avpf[] = {
	[PROTO_RTP_AVP]			= &__sh_avpf2avp,
	[PROTO_RTP_AVPF]		= &__sh_noop,
	[PROTO_RTP_SAVP]		= &__sh_avpf2savp,
	[PROTO_RTP_SAVPF]		= &__sh_avp2savp,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_avpf2savp,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_avp2savp,
	[PROTO_UDPTL]			= &__sh_noop,
};
static const struct streamhandler *__sh_matrix_in_rtp_savp[] = {
	[PROTO_RTP_AVP]			= &__sh_savp2avp,
	[PROTO_RTP_AVPF]		= &__sh_savp2avp,
	[PROTO_RTP_SAVP]		= &__sh_noop,
	[PROTO_RTP_SAVPF]		= &__sh_noop,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_noop,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_noop,
	[PROTO_UDPTL]			= &__sh_noop,
};
static const struct streamhandler *__sh_matrix_in_rtp_savpf[] = {
	[PROTO_RTP_AVP]			= &__sh_savpf2avp,
	[PROTO_RTP_AVPF]		= &__sh_savp2avp,
	[PROTO_RTP_SAVP]		= &__sh_savpf2savp,
	[PROTO_RTP_SAVPF]		= &__sh_noop,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_savpf2savp,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_noop,
	[PROTO_UDPTL]			= &__sh_noop,
};
static const struct streamhandler *__sh_matrix_in_rtp_savp_dtls[] = {
	[PROTO_RTP_AVP]			= &__sh_savp2avp,
	[PROTO_RTP_AVPF]		= &__sh_savp2avp,
	[PROTO_RTP_SAVP]		= &__sh_savp2savp,
	[PROTO_RTP_SAVPF]		= &__sh_savp2savp,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_savp2savp,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_savp2savp,
	[PROTO_UDPTL]			= &__sh_noop,
};
static const struct streamhandler *__sh_matrix_in_rtp_savpf_dtls[] = {
	[PROTO_RTP_AVP]			= &__sh_savpf2avp,
	[PROTO_RTP_AVPF]		= &__sh_savp2avp,
	[PROTO_RTP_SAVP]		= &__sh_savpf2savp,
	[PROTO_RTP_SAVPF]		= &__sh_savp2savp,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_savpf2savp,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_savp2savp,
	[PROTO_UDPTL]			= &__sh_noop,
};
static const struct streamhandler *__sh_matrix_noop[] = {
	[PROTO_RTP_AVP]			= &__sh_noop,
	[PROTO_RTP_AVPF]		= &__sh_noop,
	[PROTO_RTP_SAVP]		= &__sh_noop,
	[PROTO_RTP_SAVPF]		= &__sh_noop,
	[PROTO_UDP_TLS_RTP_SAVP]	= &__sh_noop,
	[PROTO_UDP_TLS_RTP_SAVPF]	= &__sh_noop,
	[PROTO_UDPTL]			= &__sh_noop,
};

/* ********** */

static const struct streamhandler **__sh_matrix[] = {
	[PROTO_RTP_AVP]			= __sh_matrix_in_rtp_avp,
	[PROTO_RTP_AVPF]		= __sh_matrix_in_rtp_avpf,
	[PROTO_RTP_SAVP]		= __sh_matrix_in_rtp_savp,
	[PROTO_RTP_SAVPF]		= __sh_matrix_in_rtp_savpf,
	[PROTO_UDP_TLS_RTP_SAVP]	= __sh_matrix_in_rtp_savp,
	[PROTO_UDP_TLS_RTP_SAVPF]	= __sh_matrix_in_rtp_savpf,
	[PROTO_UDPTL]			= __sh_matrix_noop,
};
/* special case for DTLS as we can't pass through SRTP<>SRTP */
static const struct streamhandler **__sh_matrix_dtls[] = {
	[PROTO_RTP_AVP]			= __sh_matrix_in_rtp_avp,
	[PROTO_RTP_AVPF]		= __sh_matrix_in_rtp_avpf,
	[PROTO_RTP_SAVP]		= __sh_matrix_in_rtp_savp_dtls,
	[PROTO_RTP_SAVPF]		= __sh_matrix_in_rtp_savpf_dtls,
	[PROTO_UDP_TLS_RTP_SAVP]	= __sh_matrix_in_rtp_savp_dtls,
	[PROTO_UDP_TLS_RTP_SAVPF]	= __sh_matrix_in_rtp_savpf_dtls,
	[PROTO_UDPTL]			= __sh_matrix_noop,
};

/* ********** */

static const struct rtpengine_srtp __res_null = {
	.cipher			= REC_NULL,
	.hmac			= REH_NULL,
};






static void unkernelize(struct packet_stream *);
static void __stream_unkernelize(struct packet_stream *ps);
static void stream_unkernelize(struct packet_stream *ps);
static void __monologue_destroy(struct call_monologue *monologue);
static struct interface_address *get_interface_address(struct local_interface *lif, int family);
static const GQueue *get_interface_addresses(struct local_interface *lif, int family);




/* called lock-free */
static void stream_fd_closed(int fd, void *p, uintptr_t u) {
	struct stream_fd *sfd = p;
	struct call *c;
	int i;
	socklen_t j;

	assert(sfd->fd.fd == fd);
	c = sfd->call;
	if (!c)
		return;

	j = sizeof(i);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &i, &j);
	ilog(LOG_WARNING, "Read error on media socket: %i (%s) -- closing call", i, strerror(i));

	call_destroy(c);
}




INLINE void __re_address_translate(struct re_address *o, const struct endpoint *ep) {
	o->family = family_from_address(&ep->ip46);
	if (o->family == AF_INET)
		o->u.ipv4 = in6_to_4(&ep->ip46);
	else
		memcpy(o->u.ipv6, &ep->ip46, sizeof(o->u.ipv6));
	o->port = ep->port;
}

/* called with in_lock held */
void kernelize(struct packet_stream *stream) {
	struct rtpengine_target_info reti;
	struct call *call = stream->call;
	struct callmaster *cm = call->callmaster;
	struct packet_stream *sink = NULL;
	struct interface_address *ifa;

	if (PS_ISSET(stream, KERNELIZED))
		return;
	if (cm->conf.kernelid < 0)
		goto no_kernel;
	if (cm->conf.kernelfd < 0)
		goto no_kernel_warn;
	if (!PS_ISSET(stream, RTP))
		goto no_kernel;
	if (!stream->sfd)
		goto no_kernel;

	ilog(LOG_INFO, "Kernelizing media stream");

	sink = packet_stream_sink(stream);
	if (!sink) {
		ilog(LOG_WARNING, "Attempt to kernelize stream without sink");
		goto no_kernel;
	}

	determine_handler(stream, sink);

	if (is_addr_unspecified(&sink->advertised_endpoint.ip46)
			|| !sink->advertised_endpoint.port)
		goto no_kernel;
	if (!stream->handler->in->kernel
			|| !stream->handler->out->kernel)
		goto no_kernel_warn;

	ZERO(reti);

	if (PS_ISSET(stream, STRICT_SOURCE) || PS_ISSET(stream, MEDIA_HANDOVER)) {
		mutex_lock(&stream->out_lock);
		__re_address_translate(&reti.expected_src, &stream->endpoint);
		mutex_unlock(&stream->out_lock);
		if (PS_ISSET(stream, STRICT_SOURCE))
			reti.src_mismatch = MSM_DROP;
		else if (PS_ISSET(stream, MEDIA_HANDOVER))
			reti.src_mismatch = MSM_PROPAGATE;
	}

	mutex_lock(&sink->out_lock);

	reti.target_port = stream->sfd->fd.localport;
	reti.tos = call->tos;
	reti.rtcp_mux = MEDIA_ISSET(stream->media, RTCP_MUX);
	reti.dtls = MEDIA_ISSET(stream->media, DTLS);
	reti.stun = PS_ISSET(stream, STUN);

	__re_address_translate(&reti.dst_addr, &sink->endpoint);

	reti.src_addr.family = reti.dst_addr.family;
	reti.src_addr.port = sink->sfd->fd.localport;

	ifa = g_atomic_pointer_get(&sink->media->local_address);
	if (reti.src_addr.family == AF_INET)
		reti.src_addr.u.ipv4 = in6_to_4(&ifa->addr);
	else
		memcpy(reti.src_addr.u.ipv6, &ifa->addr, sizeof(reti.src_addr.u.ipv6));

	stream->handler->in->kernel(&reti.decrypt, stream);
	stream->handler->out->kernel(&reti.encrypt, sink);

	mutex_unlock(&sink->out_lock);

	if (!reti.encrypt.cipher || !reti.encrypt.hmac)
		goto no_kernel_warn;
	if (!reti.decrypt.cipher || !reti.decrypt.hmac)
		goto no_kernel_warn;

	ZERO(stream->kernel_stats);

	kernel_add_stream(cm->conf.kernelfd, &reti, 0);
	PS_SET(stream, KERNELIZED);

	return;

no_kernel_warn:
	ilog(LOG_WARNING, "No support for kernel packet forwarding available");
no_kernel:
	PS_SET(stream, KERNELIZED);
	PS_SET(stream, NO_KERNEL_SUPPORT);
}




/* returns: 0 = not a muxed stream, 1 = muxed, RTP, 2 = muxed, RTCP */
static int rtcp_demux(str *s, struct call_media *media) {
	if (!MEDIA_ISSET(media, RTCP_MUX))
		return 0;
	return rtcp_demux_is_rtcp(s) ? 2 : 1;
}

static int call_avpf2avp_rtcp(str *s, struct packet_stream *stream) {
	return rtcp_avpf2avp(s);
}
static int call_avp2savp_rtp(str *s, struct packet_stream *stream) {
	return rtp_avp2savp(s, &stream->crypto);
}
static int call_avp2savp_rtcp(str *s, struct packet_stream *stream) {
	return rtcp_avp2savp(s, &stream->crypto);
}
static int call_savp2avp_rtp(str *s, struct packet_stream *stream) {
	return rtp_savp2avp(s, &stream->sfd->crypto);
}
static int call_savp2avp_rtcp(str *s, struct packet_stream *stream) {
	return rtcp_savp2avp(s, &stream->sfd->crypto);
}
static int call_savpf2avp_rtcp(str *s, struct packet_stream *stream) {
	int ret;
	ret = rtcp_savp2avp(s, &stream->sfd->crypto);
	if (ret < 0)
		return ret;
	return rtcp_avpf2avp(s);
}


static int __k_null(struct rtpengine_srtp *s, struct packet_stream *stream) {
	*s = __res_null;
	return 0;
}
static int __k_srtp_crypt(struct rtpengine_srtp *s, struct crypto_context *c) {
	if (!c->params.crypto_suite)
		return -1;

	*s = (struct rtpengine_srtp) {
		.cipher		= c->params.crypto_suite->kernel_cipher,
		.hmac		= c->params.crypto_suite->kernel_hmac,
		.mki_len	= c->params.mki_len,
		.last_index	= c->last_index,
		.auth_tag_len	= c->params.crypto_suite->srtp_auth_tag,
	};
	if (c->params.mki_len)
		memcpy(s->mki, c->params.mki, c->params.mki_len);
	memcpy(s->master_key, c->params.master_key, c->params.crypto_suite->master_key_len);
	memcpy(s->master_salt, c->params.master_salt, c->params.crypto_suite->master_salt_len);
	return 0;
}
static int __k_srtp_encrypt(struct rtpengine_srtp *s, struct packet_stream *stream) {
	return __k_srtp_crypt(s, &stream->crypto);
}
static int __k_srtp_decrypt(struct rtpengine_srtp *s, struct packet_stream *stream) {
	return __k_srtp_crypt(s, &stream->sfd->crypto);
}

/* must be called with call->master_lock held in R, and in->in_lock held */
static void determine_handler(struct packet_stream *in, const struct packet_stream *out) {
	const struct streamhandler **sh_pp, *sh;
	const struct streamhandler ***matrix;

	if (PS_ISSET(in, HAS_HANDLER))
		return;
	if (MEDIA_ISSET(in->media, PASSTHRU))
		goto noop;

	if (!in->media->protocol)
		goto err;
	if (!out->media->protocol)
		goto err;

	matrix = __sh_matrix;
	if (MEDIA_ISSET(in->media, DTLS) || MEDIA_ISSET(out->media, DTLS))
		matrix = __sh_matrix_dtls;

	sh_pp = matrix[in->media->protocol->index];
	if (!sh_pp)
		goto err;
	sh = sh_pp[out->media->protocol->index];
	if (!sh)
		goto err;
	in->handler = sh;

done:
	PS_SET(in, HAS_HANDLER);
	return;

err:
	ilog(LOG_WARNING, "Unknown transport protocol encountered");
noop:
	in->handler = &__sh_noop;
	goto done;
}

void stream_msg_mh_src(struct packet_stream *ps, struct msghdr *mh) {
	struct cmsghdr *ch;
	struct in_pktinfo *pi;
	struct in6_pktinfo *pi6;
	struct sockaddr_in6 *sin6;
	struct interface_address *ifa;


	sin6 = mh->msg_name;
	ifa = g_atomic_pointer_get(&ps->media->local_address);

	ch = CMSG_FIRSTHDR(mh);
	ZERO(*ch);

	if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
		ch->cmsg_len = CMSG_LEN(sizeof(*pi));
		ch->cmsg_level = IPPROTO_IP;
		ch->cmsg_type = IP_PKTINFO;

		pi = (void *) CMSG_DATA(ch);
		ZERO(*pi);
		pi->ipi_spec_dst.s_addr = in6_to_4(&ifa->addr);

		mh->msg_controllen = CMSG_SPACE(sizeof(*pi));
	}
	else {
		ch->cmsg_len = CMSG_LEN(sizeof(*pi6));
		ch->cmsg_level = IPPROTO_IPV6;
		ch->cmsg_type = IPV6_PKTINFO;

		pi6 = (void *) CMSG_DATA(ch);
		ZERO(*pi6);
		pi6->ipi6_addr = ifa->addr;

		mh->msg_controllen = CMSG_SPACE(sizeof(*pi6));
	}
}

/* called lock-free */
static int stream_packet(struct stream_fd *sfd, str *s, struct sockaddr_in6 *fsin, struct in6_addr *dst) {
	struct packet_stream *stream,
			     *sink = NULL,
			     *in_srtp, *out_srtp;
	struct call_media *media;
	int ret = 0, update = 0, stun_ret = 0, handler_ret = 0, muxed_rtcp = 0, rtcp = 0,
	    unk = 0;
	int i;
	struct sockaddr_in6 sin6;
	struct msghdr mh;
	struct iovec iov;
	unsigned char buf[256];
	struct call *call;
	struct callmaster *cm;
	/*unsigned char cc;*/
	char addr[64];
	struct endpoint endpoint;
	rewrite_func rwf_in, rwf_out;
	struct interface_address *loc_addr;
        int can_use_libjitter = 1;
        int is_rtp_packet = 0;

	call = sfd->call;
	cm = call->callmaster;
	smart_ntop_port(addr, fsin, sizeof(addr));

	rwlock_lock_r(&call->master_lock);

	stream = sfd->stream;
	if (!stream)
		goto unlock_out;

	mutex_lock(&stream->in_lock);

	media = stream->media;

	if (!stream->sfd)
		goto done;

	if (MEDIA_ISSET(media, DTLS) && is_dtls(s)) {
		ret = dtls(stream, s, fsin);
		if (!ret)
			goto done;
	}

	if (PS_ISSET(stream, STUN) && is_stun(s)) {
		stun_ret = stun(s, stream, fsin);
		if (!stun_ret)
			goto done;
		if (stun_ret == 1) {
			ilog(LOG_INFO, "STUN: using this candidate");
			goto use_cand;
		}
		else /* not an stun packet */
			stun_ret = 0;
	}

#if RTP_LOOP_PROTECT
	for (i = 0; i < RTP_LOOP_PACKETS; i++) {
		if (stream->lp_buf[i].len != s->len)
			continue;
		if (memcmp(stream->lp_buf[i].buf, s->s, MIN(s->len, RTP_LOOP_PROTECT)))
			continue;

		__C_DBG("packet dupe");
		if (stream->lp_count >= RTP_LOOP_MAX_COUNT) {
			ilog(LOG_WARNING, "More than %d duplicate packets detected, dropping packet "
					"to avoid potential loop", RTP_LOOP_MAX_COUNT);
			goto done;
		}

		stream->lp_count++;
		goto loop_ok;
	}

	/* not a dupe */
	stream->lp_count = 0;
	stream->lp_buf[stream->lp_idx].len = s->len;
	memcpy(stream->lp_buf[stream->lp_idx].buf, s->s, MIN(s->len, RTP_LOOP_PROTECT));
	stream->lp_idx = (stream->lp_idx + 1) % RTP_LOOP_PACKETS;
loop_ok:
#endif

	mutex_unlock(&stream->in_lock);

	in_srtp = stream;
	sink = stream->rtp_sink;
	if (!sink && PS_ISSET(stream, RTCP)) {
		sink = stream->rtcp_sink;
		rtcp = 1;
	}
	else if (stream->rtcp_sink) {
		muxed_rtcp = rtcp_demux(s, media);
		if (muxed_rtcp == 2) {
			sink = stream->rtcp_sink;
			rtcp = 1;
			in_srtp = stream->rtcp_sibling;
		}
	}
	out_srtp = sink;
	if (rtcp && sink && sink->rtcp_sibling)
		out_srtp = sink->rtcp_sibling;

	if (!sink || !sink->sfd || !out_srtp->sfd || !in_srtp->sfd) {
		ilog(LOG_WARNING, "RTP packet from %s discarded", addr);
		mutex_lock(&stream->in_lock);
		stream->stats.errors++;
		mutex_lock(&cm->statspslock);
		cm->statsps.errors++;
		mutex_unlock(&cm->statspslock);
		goto done;
	}

	mutex_lock(&in_srtp->in_lock);

	determine_handler(in_srtp, sink);

	if (!rtcp) {
		rwf_in = in_srtp->handler->in->rtp;
		rwf_out = in_srtp->handler->out->rtp;
                is_rtp_packet = 1;
	}
	else {
		rwf_in = in_srtp->handler->in->rtcp;
		rwf_out = in_srtp->handler->out->rtcp;
	}

	mutex_lock(&out_srtp->out_lock);

	/* return values are: 0 = forward packet, -1 = error/dont forward,
	 * 1 = forward and push update to redis */
	if (rwf_in)
		handler_ret = rwf_in(s, in_srtp);
	if (handler_ret >= 0 && rwf_out)
		handler_ret += rwf_out(s, out_srtp);

	if (handler_ret > 0)
		update = 1;

	mutex_unlock(&out_srtp->out_lock);
	mutex_unlock(&in_srtp->in_lock);

	mutex_lock(&stream->in_lock);

use_cand:
	/* we're OK to (potentially) use the source address of this packet as destination
	 * in the other direction. */
	/* if the other side hasn't been signalled yet, just forward the packet */
	if (!PS_ISSET(stream, FILLED))
		goto forward;

	/* do not pay attention to source addresses of incoming packets for asymmetric streams */
	if (MEDIA_ISSET(media, ASYMMETRIC))
		PS_SET(stream, CONFIRMED);

	/* if we have already updated the endpoint in the past ... */
	if (PS_ISSET(stream, CONFIRMED)) {
		/* see if we need to compare the source address with the known endpoint */
		if (PS_ISSET(stream, STRICT_SOURCE) || PS_ISSET(stream, MEDIA_HANDOVER)) {
			endpoint.ip46 = fsin->sin6_addr;
			endpoint.port = ntohs(fsin->sin6_port);
			mutex_lock(&stream->out_lock);

			int tmp = memcmp(&endpoint, &stream->endpoint, sizeof(endpoint));
			if (tmp && PS_ISSET(stream, MEDIA_HANDOVER)) {
				/* out_lock remains locked */
				ilog(LOG_INFO, "Peer address changed to %s", addr);
				unk = 1;
				goto update_addr;
			}

			mutex_unlock(&stream->out_lock);

			if (tmp && PS_ISSET(stream, STRICT_SOURCE)) {
				stream->stats.errors++;
				goto drop;
			}
		}
		goto kernel_check;
	}

	/* wait at least 3 seconds after last signal before committing to a particular
	 * endpoint address */
	if (!call->last_signal || poller_now <= call->last_signal + 3)
		goto update_peerinfo;

	ilog(LOG_INFO, "Confirmed peer address as %s", addr);

	PS_SET(stream, CONFIRMED);
	update = 1;

update_peerinfo:
	mutex_lock(&stream->out_lock);
update_addr:
	endpoint = stream->endpoint;
	stream->endpoint.ip46 = fsin->sin6_addr;
	stream->endpoint.port = ntohs(fsin->sin6_port);
	if (memcmp(&endpoint, &stream->endpoint, sizeof(endpoint)))
		update = 1;
	mutex_unlock(&stream->out_lock);

	/* check the destination address of the received packet against what we think our
	 * local interface to use is */
	loc_addr = g_atomic_pointer_get(&media->local_address);
	if (dst && memcmp(dst, &loc_addr->addr, sizeof(*dst))) {
		struct interface_address *ifa;
		char ifa_buf[64];
		smart_ntop(ifa_buf, dst, sizeof(ifa_buf));
		ifa = get_interface_from_address(media->interface, dst);
		if (!ifa) {
			ilog(LOG_ERROR, "No matching local interface for destination address %s found", ifa_buf);
			goto drop;
		}
		if (g_atomic_pointer_compare_and_exchange(&media->local_address, loc_addr, ifa)) {
			ilog(LOG_INFO, "Switching local interface to %s", ifa_buf);
			update = 1;
		}
	}


kernel_check:
	if (PS_ISSET(stream, NO_KERNEL_SUPPORT))
		goto forward;

	if (PS_ISSET(stream, CONFIRMED) && sink && PS_ISSET(sink, CONFIRMED) && PS_ISSET(sink, FILLED))
		kernelize(stream);

forward:
	if (sink)
		mutex_lock(&sink->out_lock);

	if (!sink || is_addr_unspecified(&sink->advertised_endpoint.ip46)
			|| !sink->advertised_endpoint.port
			|| stun_ret || handler_ret < 0)
		goto drop;

	ZERO(mh);
	mh.msg_control = buf;
	mh.msg_controllen = sizeof(buf);

	ZERO(sin6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_addr = sink->endpoint.ip46;
	sin6.sin6_port = htons(sink->endpoint.port);
	mh.msg_name = &sin6;
	mh.msg_namelen = sizeof(sin6);

	mutex_unlock(&sink->out_lock);

	stream_msg_mh_src(sink, &mh);

	ZERO(iov);
	iov.iov_base = s->s;
	iov.iov_len = s->len;

	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;

        if (!cm->conf.libjitter.sendmsg) {
                can_use_libjitter = 0;
        }

        if (!is_rtp_packet) {
                can_use_libjitter = 0;
        }

        if (sink && (sink->media->protocol->srtp | sink->media->protocol->avpf)) {
                can_use_libjitter = 0;                
        }

        if (can_use_libjitter == 1) {
                ret = cm->conf.libjitter.sendmsg(sink->sfd->fd.fd, &mh, 0);
        } else {
	        ret = sendmsg(sink->sfd->fd.fd, &mh, 0);
        }

	if (ret == -1) {
		ret = -errno;
                ilog(LOG_DEBUG,"Error when sending message. Error: %s",strerror(errno));
		stream->stats.errors++;
		mutex_lock(&cm->statspslock);
		cm->statsps.errors++;
		mutex_unlock(&cm->statspslock);
		goto out;
	}

	sink = NULL;

drop:
	if (sink)
		mutex_unlock(&sink->out_lock);
	ret = 0;
	stream->stats.packets++;
	stream->stats.bytes += s->len;
	stream->last_packet = poller_now;
	mutex_lock(&cm->statspslock);
	cm->statsps.packets++;
	cm->statsps.bytes += s->len;
	mutex_unlock(&cm->statspslock);

out:
	if (ret == 0 && update)
		ret = 1;

done:
	if (unk)
		__stream_unkernelize(stream);
	mutex_unlock(&stream->in_lock);
	if (unk) {
		stream_unkernelize(stream->rtp_sink);
		stream_unkernelize(stream->rtcp_sink);
	}
unlock_out:
	rwlock_unlock_r(&call->master_lock);

	return ret;
}




static void stream_fd_readable(int fd, void *p, uintptr_t u) {
	struct stream_fd *sfd = p;
	char buf[RTP_BUFFER_SIZE];
	int ret, iters;
	struct sockaddr_in6 sin6_src;
	int update = 0;
	struct call *ca;
	str s;
	struct msghdr mh;
	struct iovec iov;
	char control[128];
	struct cmsghdr *cmh;
	struct in6_pktinfo *pi6;
	struct in6_addr *dst;

	if (sfd->fd.fd != fd)
		goto out;

	log_info_stream_fd(sfd);

	for (iters = 0; ; iters++) {
#if MAX_RECV_ITERS
		if (iters >= MAX_RECV_ITERS) {
			ilog(LOG_ERROR, "Too many packets in UDP receive queue (more than %d), "
					"aborting loop. Dropped packets possible", iters);
			break;
		}
#endif

		ZERO(mh);
		mh.msg_name = &sin6_src;
		mh.msg_namelen = sizeof(sin6_src);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = control;
		mh.msg_controllen = sizeof(control);
		iov.iov_base = buf + RTP_BUFFER_HEAD_ROOM;
		iov.iov_len = MAX_RTP_PACKET_SIZE;

		ret = recvmsg(fd, &mh, 0);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			stream_fd_closed(fd, sfd, 0);
			goto done;
		}
		if (ret >= MAX_RTP_PACKET_SIZE)
			ilog(LOG_WARNING, "UDP packet possibly truncated");

		dst = NULL;
		for (cmh = CMSG_FIRSTHDR(&mh); cmh; cmh = CMSG_NXTHDR(&mh, cmh)) {
			if (cmh->cmsg_level == IPPROTO_IPV6 && cmh->cmsg_type == IPV6_PKTINFO) {
				pi6 = (void *) CMSG_DATA(cmh);
				dst = &pi6->ipi6_addr;
			}
		}

		str_init_len(&s, buf + RTP_BUFFER_HEAD_ROOM, ret);
		ret = stream_packet(sfd, &s, &sin6_src, dst);
		if (ret < 0) {
			ilog(LOG_WARNING, "Write error on RTP socket: %s", strerror(-ret));
			call_destroy(sfd->call);
			return;
		}
		if (ret == 1)
			update = 1;
	}

out:
	ca = sfd->call ? : NULL;

	if (ca && update)
		redis_update(ca, sfd->call->callmaster->conf.redis);
done:
	log_info_clear();
}





/* called with call->master_lock held in R */
static int call_timer_delete_monologues(struct call *c) {
	GSList *i;
	struct call_monologue *ml;
	int ret = 0;
	time_t min_deleted = 0;

	/* we need a write lock here */
	rwlock_unlock_r(&c->master_lock);
	rwlock_lock_w(&c->master_lock);

	for (i = c->monologues; i; i = i->next) {
		ml = i->data;

		if (!ml->deleted)
			continue;
		if (ml->deleted > poller_now) {
			if (!min_deleted || ml->deleted < min_deleted)
				min_deleted = ml->deleted;
			continue;
		}

		__monologue_destroy(ml);

		ml->deleted = 0;

		if (!g_hash_table_size(c->tags)) {
			ilog(LOG_INFO, "Call branch '"STR_FORMAT"' deleted, no more branches remaining",
					STR_FMT(&ml->tag));
			ret = 1; /* destroy call */
			goto out;
		}

		ilog(LOG_INFO, "Call branch "STR_FORMAT" deleted",
				STR_FMT(&ml->tag));
	}

out:
	c->ml_deleted = min_deleted;

	rwlock_unlock_w(&c->master_lock);
	rwlock_lock_r(&c->master_lock);

	return ret;
}



/* called with callmaster->hashlock held */
static void call_timer_iterator(void *key, void *val, void *ptr) {
	struct call *c = val;
	struct iterator_helper *hlp = ptr;
	GSList *it;
	struct callmaster *cm;
	unsigned int check;
	int good = 0;
	struct packet_stream *ps;
	struct stream_fd *sfd;
	int tmp_t_reason=0;
	struct call_monologue *ml;
	GSList *i;

	rwlock_lock_r(&c->master_lock);
	log_info_call(c);

	cm = c->callmaster;

	if (c->deleted && poller_now >= c->deleted
			&& c->last_signal <= c->deleted)
		goto delete;

	if (c->ml_deleted && poller_now >= c->ml_deleted) {
		if (call_timer_delete_monologues(c))
			goto delete;
	}

	if (!c->streams)
		goto drop;

	for (it = c->streams; it; it = it->next) {
		ps = it->data;
		mutex_lock(&ps->in_lock);

		if (!ps->media)
			goto next;
		sfd = ps->sfd;
		if (!sfd)
			goto no_sfd;

		if (MEDIA_ISSET(ps->media, DTLS) && sfd->dtls.init && !sfd->dtls.connected)
			dtls(ps, NULL, NULL);

		if (hlp->ports[sfd->fd.localport])
			goto next;
		hlp->ports[sfd->fd.localport] = sfd;
		obj_hold(sfd);

no_sfd:
		if (good)
			goto next;

		check = cm->conf.timeout;
		tmp_t_reason = 1;
		if (!MEDIA_ISSET(ps->media, RECV) || !sfd) {
			check = cm->conf.silent_timeout;
			tmp_t_reason = 2;
		}

		if (poller_now - ps->last_packet < check)
			good = 1;

next:
		mutex_unlock(&ps->in_lock);
	}

	if (good)
		goto out;

	if (c->ml_deleted)
		goto out;

	for (i = c->monologues; i; i = i->next) {
		ml = i->data;
		memset(&ml->terminated,0,sizeof(struct timeval));
		gettimeofday(&(ml->terminated),NULL);
		if (tmp_t_reason==1) {
			ml->term_reason = TIMEOUT;
		} else if (tmp_t_reason==2) {
			ml->term_reason = SILENT_TIMEOUT;
		} else {
			ml->term_reason = UNKNOWN;
		}
	}

	ilog(LOG_INFO, "Closing call due to timeout");

drop:
	hlp->del_timeout = g_slist_prepend(hlp->del_timeout, obj_get(c));
	goto out;

delete:
	hlp->del_scheduled = g_slist_prepend(hlp->del_scheduled, obj_get(c));
	goto out;

out:
	rwlock_unlock_r(&c->master_lock);
	log_info_clear();
}

void xmlrpc_kill_calls(void *p) {
	struct xmlrpc_helper *xh = p;
	xmlrpc_env e;
	xmlrpc_client *c;
	xmlrpc_value *r;
	pid_t pid;
	sigset_t ss;
	int i = 0;
	int status;
	str *tag;
	const char *url;

	while (xh->tags_urls && xh->tags_urls->next) {
		tag = xh->tags_urls->data;
		url = xh->tags_urls->next->data;

		ilog(LOG_INFO, "Forking child to close call with tag "STR_FORMAT" via XMLRPC call to %s",
				STR_FMT(tag), url);
		pid = fork();

		if (pid) {
retry:
			pid = waitpid(pid, &status, 0);
			if ((pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) || i >= 3) {
				xh->tags_urls = g_slist_delete_link(xh->tags_urls, xh->tags_urls);
				xh->tags_urls = g_slist_delete_link(xh->tags_urls, xh->tags_urls);
				i = 0;
			}
			else {
				if (pid == -1 && errno == EINTR)
					goto retry;
				ilog(LOG_INFO, "XMLRPC child exited with status %i", status);
				i++;
			}
			continue;
		}

		/* child process */
		alarm(1); /* syslog functions contain a lock, which may be locked at
			     this point and can't be unlocked */
		rlim(RLIMIT_CORE, 0);
		sigemptyset(&ss);
		sigprocmask(SIG_SETMASK, &ss, NULL);
		closelog();

		for (i = 0; i < 100; i++)
			close(i);

		if (!_log_stderr) {
			openlog("rtpengine/child", LOG_PID | LOG_NDELAY, LOG_DAEMON);
		}
		ilog(LOG_INFO, "Initiating XMLRPC call for tag "STR_FORMAT"", STR_FMT(tag));

		alarm(5);

		xmlrpc_env_init(&e);
		xmlrpc_client_setup_global_const(&e);
		xmlrpc_client_create(&e, XMLRPC_CLIENT_NO_FLAGS, "ngcp-rtpengine", RTPENGINE_VERSION,
			NULL, 0, &c);
		if (e.fault_occurred)
			goto fault;

		r = NULL;
		switch (xh->fmt) {
		case XF_SEMS:
			xmlrpc_client_call2f(&e, c, url, "di", &r, "(ssss)",
						"sbc", "postControlCmd", tag->s, "teardown");
			break;
		case XF_CALLID:
			xmlrpc_client_call2f(&e, c, url, "teardown", &r, "(s)", tag->s);
			break;
		}

		if (r)
			xmlrpc_DECREF(r);
		if (e.fault_occurred)
			goto fault;

		xmlrpc_client_destroy(c);
		xh->tags_urls = g_slist_delete_link(xh->tags_urls, xh->tags_urls);
		xh->tags_urls = g_slist_delete_link(xh->tags_urls, xh->tags_urls);
		xmlrpc_env_clean(&e);

		_exit(0);

fault:
		ilog(LOG_WARNING, "XMLRPC fault occurred: %s", e.fault_string);
		_exit(1);
	}

	g_string_chunk_free(xh->c);
	g_slice_free1(sizeof(*xh), xh);
}

void kill_calls_timer(GSList *list, struct callmaster *m) {
	struct call *ca;
	GSList *csl;
	struct call_monologue *cm;
	const char *url, *url_prefix, *url_suffix;
	struct xmlrpc_helper *xh = NULL;
	char addr[64], url_buf[128];

	if (!list)
		return;

	/* if m is NULL, it's the scheduled deletions, otherwise it's the timeouts */
	url = m ? m->conf.b2b_url : NULL;
	if (url) {
		xh = g_slice_alloc(sizeof(*xh));
		xh->c = g_string_chunk_new(64);
		url_prefix = NULL;
		url_suffix = strstr(url, "%%");
		if (url_suffix) {
			url_prefix = g_string_chunk_insert_len(xh->c, url, url_suffix - url);
			url_suffix = g_string_chunk_insert(xh->c, url_suffix + 2);
		}
		else
			url_suffix = g_string_chunk_insert(xh->c, url);
		xh->tags_urls = NULL;
		xh->fmt = m->conf.fmt;
	}

	while (list) {
		ca = list->data;
		log_info_call(ca);
		if (!url)
			goto destroy;

		rwlock_lock_r(&ca->master_lock);

		if (url_prefix) {
			smart_ntop_p(addr, &ca->created_from_addr.sin6_addr, sizeof(addr));
			snprintf(url_buf, sizeof(url_buf), "%s%s%s",
					url_prefix, addr, url_suffix);
		}
		else
			snprintf(url_buf, sizeof(url_buf), "%s", url_suffix);

		switch (m->conf.fmt) {
		case XF_SEMS:
			for (csl = ca->monologues; csl; csl = csl->next) {
				cm = csl->data;
				if (cm->tag.s && cm->tag.len) {
					xh->tags_urls = g_slist_prepend(xh->tags_urls, g_string_chunk_insert(xh->c, url_buf));
					xh->tags_urls = g_slist_prepend(xh->tags_urls, str_chunk_insert(xh->c, &cm->tag));
				}
			}
			break;
		case XF_CALLID:
			xh->tags_urls = g_slist_prepend(xh->tags_urls, g_string_chunk_insert(xh->c, url_buf));
			xh->tags_urls = g_slist_prepend(xh->tags_urls, str_chunk_insert(xh->c, &ca->callid));
			break;
		}

		rwlock_unlock_r(&ca->master_lock);

destroy:
		call_destroy(ca);
		obj_put(ca);
		list = g_slist_delete_link(list, list);
		log_info_clear();
	}

	if (xh)
		thread_create_detach(xmlrpc_kill_calls, xh);
}


#define DS(x) do {							\
		mutex_lock(&ps->in_lock);				\
		if (ke->stats.x < ps->kernel_stats.x)			\
			d = 0;						\
		else							\
			d = ke->stats.x - ps->kernel_stats.x;		\
		ps->stats.x += d;					\
		mutex_unlock(&ps->in_lock);				\
		mutex_lock(&m->statspslock);				\
		m->statsps.x += d;					\
		mutex_unlock(&m->statspslock);				\
	} while (0)
static void callmaster_timer(void *ptr) {
	struct callmaster *m = ptr;
	struct iterator_helper hlp;
	GList *i;
	struct rtpengine_list_entry *ke;
	struct packet_stream *ps, *sink;
	u_int64_t d;
	struct stats tmpstats;
	int j, update;
	struct stream_fd *sfd;

	ZERO(hlp);

	rwlock_lock_r(&m->hashlock);
	g_hash_table_foreach(m->callhash, call_timer_iterator, &hlp);
	rwlock_unlock_r(&m->hashlock);

	mutex_lock(&m->statspslock);
	memcpy(&tmpstats, &m->statsps, sizeof(tmpstats));
	ZERO(m->statsps);
	mutex_unlock(&m->statspslock);
	mutex_lock(&m->statslock);
	memcpy(&m->stats, &tmpstats, sizeof(m->stats));
	mutex_unlock(&m->statslock);

	i = (m->conf.kernelid >= 0) ? kernel_list(m->conf.kernelid) : NULL;
	while (i) {
		ke = i->data;

		sfd = hlp.ports[ke->target.target_port];
		if (!sfd)
			goto next;

		rwlock_lock_r(&sfd->call->master_lock);

		ps = sfd->stream;
		if (!ps || ps->sfd != sfd) {
			rwlock_unlock_r(&sfd->call->master_lock);
			goto next;
		}

		DS(packets);
		DS(bytes);
		DS(errors);

		mutex_lock(&ps->in_lock);

		if (ke->stats.packets != ps->kernel_stats.packets)
			ps->last_packet = poller_now;

		ps->kernel_stats.packets = ke->stats.packets;
		ps->kernel_stats.bytes = ke->stats.bytes;
		ps->kernel_stats.errors = ke->stats.errors;

		update = 0;

		sink = packet_stream_sink(ps);

		if (sink)
			mutex_lock(&sink->out_lock);

		/* XXX this only works if the kernel module actually gets to see the packets. */
		if (sink && sink->crypto.params.crypto_suite
				&& ke->target.encrypt.last_index - sink->crypto.last_index > 0x4000) {
			sink->crypto.last_index = ke->target.encrypt.last_index;
			update = 1;
		}
		if (sfd->crypto.params.crypto_suite
				&& ke->target.decrypt.last_index - sfd->crypto.last_index > 0x4000) {
			sfd->crypto.last_index = ke->target.decrypt.last_index;
			update = 1;
		}

		if (sink)
			mutex_unlock(&sink->out_lock);
		mutex_unlock(&ps->in_lock);
		rwlock_unlock_r(&sfd->call->master_lock);

		if (update)
			redis_update(ps->call, m->conf.redis);

next:
		hlp.ports[ke->target.target_port] = NULL;
		g_slice_free1(sizeof(*ke), ke);
		i = g_list_delete_link(i, i);
		if (sfd)
			obj_put(sfd);
	}

	for (j = 0; j < (sizeof(hlp.ports) / sizeof(*hlp.ports)); j++)
		if (hlp.ports[j])
			obj_put(hlp.ports[j]);

	kill_calls_timer(hlp.del_scheduled, NULL);
	kill_calls_timer(hlp.del_timeout, m);
}
#undef DS


struct callmaster *callmaster_new(struct poller *p) {
	struct callmaster *c;
	const char *errptr;
	int erroff;

	c = obj_alloc0("callmaster", sizeof(*c), NULL);

	c->callhash = g_hash_table_new(str_hash, str_equal);
	if (!c->callhash)
		goto fail;
	c->poller = p;
	rwlock_init(&c->hashlock);

	c->info_re = pcre_compile("^([^:,]+)(?::(.*?))?(?:$|,)", PCRE_DOLLAR_ENDONLY | PCRE_DOTALL, &errptr, &erroff, NULL);
	if (!c->info_re)
		goto fail;
	c->info_ree = pcre_study(c->info_re, 0, &errptr);

	c->streams_re = pcre_compile("^([\\d.]+):(\\d+)(?::(.*?))?(?:$|,)", PCRE_DOLLAR_ENDONLY | PCRE_DOTALL, &errptr, &erroff, NULL);
	if (!c->streams_re)
		goto fail;
	c->streams_ree = pcre_study(c->streams_re, 0, &errptr);

	poller_add_timer(p, callmaster_timer, &c->obj);

	return c;

fail:
	obj_put(c);
	return NULL;
}



static void __set_tos(int fd, const struct call *c) {
	int tos;

	setsockopt(fd, IPPROTO_IP, IP_TOS, &c->tos, sizeof(c->tos));
#ifdef IPV6_TCLASS
	tos = c->tos;
	setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
#else
#warning "Will not set IPv6 traffic class"
#endif
}

static void __get_pktinfo(int fd) {
	int x;
	x = 1;
	setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &x, sizeof(x));
}

static int get_port6(struct udp_fd *r, u_int16_t p, const struct call *c) {
	int fd;
	struct sockaddr_in6 sin;

	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	nonblock(fd);
	reuseaddr(fd);
	ipv6only(fd, 0);
	__set_tos(fd, c);
	__get_pktinfo(fd);

	ZERO(sin);
	sin.sin6_family = AF_INET6;
	sin.sin6_port = htons(p);
	if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)))
		goto fail;

	r->fd = fd;

	return 0;

fail:
	close(fd);
	return -1;
}

static int get_port(struct udp_fd *r, u_int16_t p, const struct call *c) {
	int ret;
	struct callmaster *m = c->callmaster;

	assert(r->fd == -1);

	__C_DBG("attempting to open port %u", p);

	mutex_lock(&m->portlock);
	if (bit_array_isset(m->ports_used, p)) {
		mutex_unlock(&m->portlock);
		__C_DBG("port in use");
		return -1;
	}
	bit_array_set(m->ports_used, p);
	mutex_unlock(&m->portlock);
	__C_DBG("port locked");

	ret = get_port6(r, p, c);

	if (ret) {
		__C_DBG("couldn't open port");
		mutex_lock(&m->portlock);
		bit_array_clear(m->ports_used, p);
		mutex_unlock(&m->portlock);
		return ret;
	}

	r->localport = p;

	return 0;
}

static void release_port(struct udp_fd *r, struct callmaster *m) {
	if (r->fd == -1 || !r->localport)
		return;
	__C_DBG("releasing port %u", r->localport);
	mutex_lock(&m->portlock);
	bit_array_clear(m->ports_used, r->localport);
	mutex_unlock(&m->portlock);
	close(r->fd);
	r->fd = -1;
	r->localport = 0;
}

int __get_consecutive_ports(struct udp_fd *array, int array_len, int wanted_start_port, const struct call *c) {
	int i, j, cycle = 0;
	struct udp_fd *it;
	int port;
	struct callmaster *m = c->callmaster;

	memset(array, -1, sizeof(*array) * array_len);

	if (wanted_start_port > 0)
		port = wanted_start_port;
	else {
		mutex_lock(&m->portlock);
		port = m->lastport;
		mutex_unlock(&m->portlock);
#if PORT_RANDOM_MIN && PORT_RANDOM_MAX
		port += PORT_RANDOM_MIN + (random() % (PORT_RANDOM_MAX - PORT_RANDOM_MIN));
#endif
	}

	while (1) {
		if (!wanted_start_port) {
			if (port < m->conf.port_min)
				port = m->conf.port_min;
			if ((port & 1))
				port++;
		}

		for (i = 0; i < array_len; i++) {
			it = &array[i];

			if (!wanted_start_port && port > m->conf.port_max) {
				port = 0;
				cycle++;
				goto release_restart;
			}

			if (get_port(it, port++, c))
				goto release_restart;
		}
		break;

release_restart:
		for (j = 0; j < i; j++)
			release_port(&array[j], m);

		if (cycle >= 2 || wanted_start_port > 0)
			goto fail;
	}

	/* success */
	mutex_lock(&m->portlock);
	m->lastport = port;
	mutex_unlock(&m->portlock);

	ilog(LOG_DEBUG, "Opened ports %u..%u for media relay", 
		array[0].localport, array[array_len - 1].localport);
	return 0;

fail:
	ilog(LOG_ERR, "Failed to get %u consecutive UDP ports for relay",
			array_len);
	return -1;
}

static struct call_media *__get_media(struct call_monologue *ml, GList **it, const struct stream_params *sp) {
	struct call_media *med;

	/* iterator points to last seen element, or NULL if uninitialized */
	if (!*it)
		*it = ml->medias.head;
	else
		*it = (*it)->next;

	/* possible incremental update, hunt for correct media struct */
	while (*it) {
		med = (*it)->data;
		if (med->index == sp->index) {
			__C_DBG("found existing call_media for stream #%u", sp->index);
			return med;
		}
		*it = (*it)->next;
	}

	__C_DBG("allocating new call_media for stream #%u", sp->index);
	med = g_slice_alloc0(sizeof(*med));
	med->monologue = ml;
	med->call = ml->call;
	med->index = sp->index;
	call_str_cpy(ml->call, &med->type, &sp->type);

	g_queue_push_tail(&ml->medias, med);
	*it = ml->medias.tail;

	return med;
}

static void stream_fd_free(void *p) {
	struct stream_fd *f = p;
	struct callmaster *m = f->call->callmaster;

	release_port(&f->fd, m);
	crypto_cleanup(&f->crypto);
	dtls_connection_cleanup(&f->dtls);

	obj_put(f->call);
}

struct stream_fd *__stream_fd_new(struct udp_fd *fd, struct call *call) {
	struct stream_fd *sfd;
	struct poller_item pi;
	struct poller *po = call->callmaster->poller;

	sfd = obj_alloc0("stream_fd", sizeof(*sfd), stream_fd_free);
	sfd->fd = *fd;
	sfd->call = obj_get(call);
	call->stream_fds = g_slist_prepend(call->stream_fds, sfd); /* hand over ref */

	ZERO(pi);
	pi.fd = sfd->fd.fd;
	pi.obj = &sfd->obj;
	pi.readable = stream_fd_readable;
	pi.closed = stream_fd_closed;

	poller_add_item(po, &pi);

	return sfd;
}

static struct endpoint_map *__get_endpoint_map(struct call_media *media, unsigned int num_ports,
		const struct endpoint *ep)
{
	GSList *l;
	struct endpoint_map *em;
	struct udp_fd fd_arr[16];
	unsigned int i;
	struct stream_fd *sfd;
	struct call *call = media->call;

	for (l = media->endpoint_maps; l; l = l->next) {
		em = l->data;
		if (em->wildcard && em->sfds.length >= num_ports) {
			__C_DBG("found a wildcard endpoint map%s", ep ? " and filling it in" : "");
			if (ep) {
				em->endpoint = *ep;
				em->wildcard = 0;
			}
			return em;
		}
		if (!ep) /* creating wildcard map */
			break;
		/* handle zero endpoint address */
		if (is_addr_unspecified(&ep->ip46) || is_addr_unspecified(&em->endpoint.ip46)) {
			if (ep->port != em->endpoint.port)
				continue;
		}
		else if (memcmp(&em->endpoint, ep, sizeof(*ep)))
			continue;
		if (em->sfds.length >= num_ports) {
			if (is_addr_unspecified(&em->endpoint.ip46))
				em->endpoint.ip46 = ep->ip46;
			return em;
		}
		/* endpoint matches, but not enough ports. flush existing ports
		 * and allocate a new set. */
		__C_DBG("endpoint matches, doesn't have enough ports");
		g_queue_clear(&em->sfds);
		goto alloc;
	}

	__C_DBG("allocating new %sendpoint map", ep ? "" : "wildcard ");
	em = g_slice_alloc0(sizeof(*em));
	if (ep)
		em->endpoint = *ep;
	else
		em->wildcard = 1;
	g_queue_init(&em->sfds);
	media->endpoint_maps = g_slist_prepend(media->endpoint_maps, em);

alloc:
	if (num_ports > G_N_ELEMENTS(fd_arr))
		return NULL;
	if (__get_consecutive_ports(fd_arr, num_ports, 0, media->call))
		return NULL;

	__C_DBG("allocating stream_fds for %u ports", num_ports);
	for (i = 0; i < num_ports; i++) {
		sfd = __stream_fd_new(&fd_arr[i], call);
		g_queue_push_tail(&em->sfds, sfd); /* not referenced */
	}

	return em;
}

static void __assign_stream_fds(struct call_media *media, GList *sfds) {
	GList *l;
	struct packet_stream *ps;
	struct stream_fd *sfd;

	for (l = media->streams.head; l; l = l->next) {
		assert(sfds != NULL);
		ps = l->data;
		sfd = sfds->data;

		/* if we switch local ports, we reset crypto params */
		if (ps->sfd && ps->sfd != sfd) {
			dtls_shutdown(ps);
			crypto_reset(&ps->sfd->crypto);
		}

		ps->sfd = sfd;
		sfd->stream = ps;
		sfds = sfds->next;
	}
}

static int __wildcard_endpoint_map(struct call_media *media, unsigned int num_ports) {
	struct endpoint_map *em;

	em = __get_endpoint_map(media, num_ports, NULL);
	if (!em)
		return -1;

	__assign_stream_fds(media, em->sfds.head);

	return 0;
}

struct packet_stream *__packet_stream_new(struct call *call) {
	struct packet_stream *stream;

	stream = g_slice_alloc0(sizeof(*stream));
	mutex_init(&stream->in_lock);
	mutex_init(&stream->out_lock);
	stream->call = call;
	stream->last_packet = poller_now;
	call->streams = g_slist_prepend(call->streams, stream);

	return stream;
}

static int __num_media_streams(struct call_media *media, unsigned int num_ports) {
	struct packet_stream *stream;
	struct call *call = media->call;
	int ret = 0;

	__C_DBG("allocating %i new packet_streams", num_ports - media->streams.length);
	while (media->streams.length < num_ports) {
		stream = __packet_stream_new(call);
		stream->media = media;
		g_queue_push_tail(&media->streams, stream);
		ret++;
	}

	g_queue_truncate(&media->streams, num_ports);

	return ret;
}

static void __fill_stream(struct packet_stream *ps, const struct endpoint *ep, unsigned int port_off) {
	ps->endpoint = *ep;
	ps->endpoint.port += port_off;
	/* we reset crypto params whenever the endpoint changes */
	if (PS_ISSET(ps, FILLED) && memcmp(&ps->advertised_endpoint, &ps->endpoint, sizeof(ps->endpoint))) {
		crypto_reset(&ps->crypto);
		dtls_shutdown(ps);
	}
	ps->advertised_endpoint = ps->endpoint;
	PS_SET(ps, FILLED);
}

static int __init_stream(struct packet_stream *ps) {
	struct call_media *media = ps->media;
	struct call *call = ps->call;
	int active;

	if (ps->sfd) {
		if (MEDIA_ISSET(media, SDES))
			crypto_init(&ps->sfd->crypto, &media->sdes_in.params);

		if (MEDIA_ISSET(media, DTLS) && !PS_ISSET(ps, FALLBACK_RTCP)) {
			active = (PS_ISSET(ps, FILLED) && MEDIA_ISSET(media, SETUP_ACTIVE));
			if (active && (PS_ISSET(ps, FILLED) && MEDIA_ISSET(media, SETUP_PASSIVE)))
			{
			    ilog(LOG_INFO, "reset active due to actpass");
			    active = 0;
			}
			dtls_connection_init(ps, active, call->dtls_cert);

			if (!PS_ISSET(ps, FINGERPRINT_VERIFIED) && media->fingerprint.hash_func
					&& ps->dtls_cert)
			{
				if (dtls_verify_cert(ps))
					return -1;
			}
		}
	}

	if (MEDIA_ISSET(media, SDES))
		crypto_init(&ps->crypto, &media->sdes_out.params);

	return 0;
}

static int __init_streams(struct call_media *A, struct call_media *B, const struct stream_params *sp) {
	GList *la, *lb;
	struct packet_stream *a, *ax, *b;
	unsigned int port_off = 0;

	la = A->streams.head;
	lb = B->streams.head;

	while (la) {
		assert(lb != NULL);
		a = la->data;
		b = lb->data;

		/* RTP */
		a->rtp_sink = b;
		PS_SET(a, RTP);

		if (sp) {
			__fill_stream(a, &sp->rtp_endpoint, port_off);
			bf_copy_same(&a->ps_flags, &sp->sp_flags,
					SHARED_FLAG_STRICT_SOURCE | SHARED_FLAG_MEDIA_HANDOVER);
		}
		if (__init_stream(a))
			return -1;

		/* RTCP */
		if (!MEDIA_ISSET(B, RTCP_MUX)) {
			lb = lb->next;
			assert(lb != NULL);
			b = lb->data;
		}

		if (!MEDIA_ISSET(A, RTCP_MUX)) {
			a->rtcp_sink = NULL;
			PS_CLEAR(a, RTCP);
		}
		else {
			a->rtcp_sink = b;
			PS_SET(a, RTCP);
			PS_CLEAR(a, IMPLICIT_RTCP);
		}

		ax = a;

		/* if muxing, this is the fallback RTCP port. it also contains the RTCP
		 * crypto context */
		la = la->next;
		assert(la != NULL);
		a = la->data;

		a->rtp_sink = NULL;
		a->rtcp_sink = b;
		PS_CLEAR(a, RTP);
		PS_SET(a, RTCP);
		a->rtcp_sibling = NULL;
		bf_copy(&a->ps_flags, PS_FLAG_FALLBACK_RTCP, &ax->ps_flags, PS_FLAG_RTCP);

		ax->rtcp_sibling = a;

		if (sp) {
			if (!SP_ISSET(sp, IMPLICIT_RTCP)) {
				__fill_stream(a, &sp->rtcp_endpoint, port_off);
				PS_CLEAR(a, IMPLICIT_RTCP);
			}
			else {
				__fill_stream(a, &sp->rtp_endpoint, port_off + 1);
				PS_SET(a, IMPLICIT_RTCP);
			}
			bf_copy_same(&a->ps_flags, &sp->sp_flags,
					SHARED_FLAG_STRICT_SOURCE | SHARED_FLAG_MEDIA_HANDOVER);
		}
		if (__init_stream(a))
			return -1;

		la = la->next;
		lb = lb->next;

		port_off += 2;
	}

	return 0;
}

static void __ice_offer(const struct sdp_ng_flags *flags, struct call_media *this,
		struct call_media *other)
{
	if (!flags)
		return;

	/* we offer ICE by default */
	if (!MEDIA_ISSET(this, INITIALIZED))
		MEDIA_SET(this, ICE);
	if (flags->ice_remove)
		MEDIA_CLEAR(this, ICE);

	/* special case: if doing ICE on both sides and ice_force is not set, we cannot
	 * be sure that media will pass through us, so we have to disable certain features */
	if (MEDIA_ISSET(this, ICE) && MEDIA_ISSET(other, ICE) && !flags->ice_force) {
		ilog(LOG_DEBUG, "enabling passthrough mode");
		MEDIA_SET(this, PASSTHRU);
		MEDIA_SET(other, PASSTHRU);
	}
}

/* generates SDES parametes for outgoing SDP, which is our media "out" direction */
static void __generate_crypto(const struct sdp_ng_flags *flags, struct call_media *this,
		struct call_media *other)
{
	struct crypto_params *cp = &this->sdes_out.params,
			     *cp_in = &this->sdes_in.params;

	if (!flags)
		return;

	if (!this->protocol || !this->protocol->srtp || MEDIA_ISSET(this, PASSTHRU)) {
		cp->crypto_suite = NULL;
		/* clear crypto for the this leg b/c we are in passthrough mode */
		MEDIA_CLEAR(this, DTLS);
		MEDIA_CLEAR(this, SDES);
		MEDIA_CLEAR(this, SETUP_PASSIVE);
		MEDIA_CLEAR(this, SETUP_ACTIVE);

		if (MEDIA_ISSET(this, PASSTHRU)) {
			/* clear crypto for the other leg as well b/c passthrough only
			 * works if it is done for both legs */
			MEDIA_CLEAR(other, DTLS);
			MEDIA_CLEAR(other, SDES);
			MEDIA_CLEAR(other, SETUP_PASSIVE);
			MEDIA_CLEAR(other, SETUP_ACTIVE);
		}

		return;
	}

	if (flags->opmode == OP_OFFER) {
		/* we always offer actpass */
		MEDIA_SET(this, SETUP_PASSIVE);
		MEDIA_SET(this, SETUP_ACTIVE);
	}
	else {
		if (flags->dtls_passive && MEDIA_ISSET(this, SETUP_PASSIVE))
			MEDIA_CLEAR(this, SETUP_ACTIVE);
		/* if we can be active, we will, otherwise we'll be passive */
		if (MEDIA_ISSET(this, SETUP_ACTIVE))
			MEDIA_CLEAR(this, SETUP_PASSIVE);
	}

	if (!MEDIA_ISSET(this, INITIALIZED)) {
		/* we offer both DTLS and SDES by default */
		MEDIA_SET(this, DTLS);
		MEDIA_SET(this, SDES);
	}
	else {
		/* if we're talking to someone understanding DTLS, then skip the SDES stuff */
		if (MEDIA_ISSET(this, DTLS)) {
			MEDIA_CLEAR(this, SDES);
			goto skip_sdes;
		}
	}

	/* for answer case, otherwise we default to one */
	this->sdes_out.tag = cp_in->crypto_suite ? this->sdes_in.tag : 1;

	if (other->sdes_in.params.crypto_suite) {
		/* SRTP <> SRTP case, copy from other stream */
		crypto_params_copy(cp, &other->sdes_in.params);
		return;
	}

	if (cp->crypto_suite)
		return;

	cp->crypto_suite = cp_in->crypto_suite;
	if (!cp->crypto_suite)
		cp->crypto_suite = &crypto_suites[0];
	random_string((unsigned char *) cp->master_key,
			cp->crypto_suite->master_key_len);
	random_string((unsigned char *) cp->master_salt,
			cp->crypto_suite->master_salt_len);
	/* mki = mki_len = 0 */

skip_sdes:
	;
}


static void __disable_streams(struct call_media *media, unsigned int num_ports) {
	GList *l;
	struct packet_stream *ps;

	__num_media_streams(media, num_ports);

	for (l = media->streams.head; l; l = l->next) {
		ps = l->data;
		ps->sfd = NULL;
	}
}

static void __rtcp_mux_logic(const struct sdp_ng_flags *flags, struct call_media *media,
		struct call_media *other_media)
{
	if (!flags)
		return;

	if (flags->opmode == OP_ANSWER) {
		/* default is to go with the client's choice, unless we were instructed not
		 * to do that in the offer (see below) */
		if (!MEDIA_ISSET(media, RTCP_MUX_OVERRIDE))
			bf_copy_same(&media->media_flags, &other_media->media_flags, MEDIA_FLAG_RTCP_MUX);

		return;
	}

	if (flags->opmode != OP_OFFER)
		return;


	/* default is to pass through the client's choice, unless our peer is already
	 * talking rtcp-mux, then we stick to that */
	if (!MEDIA_ISSET(media, RTCP_MUX))
		bf_copy_same(&media->media_flags, &other_media->media_flags, MEDIA_FLAG_RTCP_MUX);
	/* in our offer, we can override the client's choice */
	if (flags->rtcp_mux_offer)
		MEDIA_SET(media, RTCP_MUX);
	else if (flags->rtcp_mux_demux)
		MEDIA_CLEAR(media, RTCP_MUX);

	/* we can also control what's going to happen in the answer. it
	 * depends on what was offered, but by default we go with the other
	 * client's choice */
	MEDIA_CLEAR(other_media, RTCP_MUX_OVERRIDE);
	if (MEDIA_ISSET(other_media, RTCP_MUX)) {
		if (!MEDIA_ISSET(media, RTCP_MUX)) {
			/* rtcp-mux was offered, but we don't offer it ourselves.
			 * the answer will not accept rtcp-mux (wasn't offered).
			 * the default is to accept the offer, unless we want to
			 * explicitly reject it. */
			MEDIA_SET(other_media, RTCP_MUX_OVERRIDE);
			if (flags->rtcp_mux_reject)
				MEDIA_CLEAR(other_media, RTCP_MUX);
		}
		else {
			/* rtcp-mux was offered and we offer it too. default is
			 * to go with the other client's choice, unless we want to
			 * either explicitly accept it (possibly demux) or reject
			 * it (possible reverse demux). */
			if (flags->rtcp_mux_accept)
				MEDIA_SET(other_media, RTCP_MUX_OVERRIDE);
			else if (flags->rtcp_mux_reject) {
				MEDIA_SET(other_media, RTCP_MUX_OVERRIDE);
				MEDIA_CLEAR(other_media, RTCP_MUX);
			}
		}
	}
	else {
		/* rtcp-mux was not offered. we may offer it, but since it wasn't
		 * offered to us, we must not accept it. */
		MEDIA_SET(other_media, RTCP_MUX_OVERRIDE);
	}
}

static void __fingerprint_changed(struct call_media *m) {
	GList *l;
	struct packet_stream *ps;

	if (!m->fingerprint.hash_func)
		return;

	ilog(LOG_INFO, "DTLS fingerprint changed, restarting DTLS");

	for (l = m->streams.head; l; l = l->next) {
		ps = l->data;
		PS_CLEAR(ps, FINGERPRINT_VERIFIED);
		dtls_shutdown(ps);
	}
}

static void __set_all_tos(struct call *c) {
	GSList *l;
	struct stream_fd *sfd;

	for (l = c->stream_fds; l; l = l->next) {
		sfd = l->data;
		__set_tos(sfd->fd.fd, c);
	}
}

static void __tos_change(struct call *call, const struct sdp_ng_flags *flags) {
	unsigned char new_tos;

	/* Handle TOS= parameter. Negative value = no change, not present or too large =
	 * revert to default, otherwise set specified value. We only do it in an offer, but
	 * then for both directions. */
	if (flags && (flags->opmode != OP_OFFER || flags->tos < 0))
		return;

	if (!flags || flags->tos > 255)
		new_tos = call->callmaster->conf.default_tos;
	else
		new_tos = flags->tos;

	if (new_tos == call->tos)
		return;

	call->tos = new_tos;
	__set_all_tos(call);
}

static void __init_interface(struct call_media *media, const str *ifname) {
	/* we're holding master_lock in W mode here, so we can safely ignore the
	 * atomic ops */
	struct interface_address *ifa = (void *) media->local_address;

	if (!media->interface || !ifa)
		goto get;
	if (!ifname || !ifname->s)
		return;
	if (!str_cmp_str(&media->interface->name, ifname))
		return;
get:
	media->interface = get_local_interface(media->call->callmaster, ifname);
	if (!media->interface) {
		media->interface = get_local_interface(media->call->callmaster, NULL);
		/* legacy support */
		if (!str_cmp(ifname, "internal"))
			media->desired_family = AF_INET;
		else if (!str_cmp(ifname, "external"))
			media->desired_family = AF_INET6;
		else
			ilog(LOG_WARNING, "Interface '"STR_FORMAT"' not found, using default", STR_FMT(ifname));
	}
	media->local_address = ifa = get_interface_address(media->interface, media->desired_family);
	if (!ifa) {
		ilog(LOG_WARNING, "No usable address in interface '"STR_FORMAT"' found, using default",
				STR_FMT(ifname));
		media->local_address = ifa = get_any_interface_address(media->interface, media->desired_family);
		media->desired_family = family_from_address(&ifa->addr);
	}
}


static void __dtls_logic(const struct sdp_ng_flags *flags, struct call_media *media,
		struct call_media *other_media, struct stream_params *sp)
{
	unsigned int tmp;

	/* active and passive are from our POV */
	tmp = other_media->media_flags;
	bf_copy(&other_media->media_flags, MEDIA_FLAG_SETUP_PASSIVE,
			&sp->sp_flags, SP_FLAG_SETUP_ACTIVE);
	bf_copy(&other_media->media_flags, MEDIA_FLAG_SETUP_ACTIVE,
			&sp->sp_flags, SP_FLAG_SETUP_PASSIVE);

	if (flags) {
		/* Special case: if this is an offer and actpass is being offered (as it should),
		 * we would normally choose to be active. However, if this is a reinvite and we
		 * were passive previously, we should retain this role. */
		if (flags && flags->opmode == OP_OFFER && MEDIA_ISSET(other_media, SETUP_ACTIVE)
				&& MEDIA_ISSET(other_media, SETUP_PASSIVE)
				&& (tmp & (MEDIA_FLAG_SETUP_ACTIVE | MEDIA_FLAG_SETUP_PASSIVE))
				== MEDIA_FLAG_SETUP_PASSIVE)
			MEDIA_CLEAR(other_media, SETUP_ACTIVE);
		/* if passive mode is requested, honour it if we can */
		if (flags && flags->dtls_passive && MEDIA_ISSET(other_media, SETUP_PASSIVE))
			MEDIA_CLEAR(other_media, SETUP_ACTIVE);
	}

	if (memcmp(&other_media->fingerprint, &sp->fingerprint, sizeof(sp->fingerprint))) {
		__fingerprint_changed(other_media);
		other_media->fingerprint = sp->fingerprint;
                // restart ice
                media->ice_ufrag = STR_NULL;
                media->ice_pwd =STR_NULL;
	}
	MEDIA_CLEAR(other_media, DTLS);
	if ((MEDIA_ISSET(other_media, SETUP_PASSIVE) || MEDIA_ISSET(other_media, SETUP_ACTIVE))
			&& other_media->fingerprint.hash_func)
		MEDIA_SET(other_media, DTLS);
}

/* called with call->master_lock held in W */
int monologue_offer_answer(struct call_monologue *other_ml, GQueue *streams,
		const struct sdp_ng_flags *flags)
{
	struct stream_params *sp;
	GList *media_iter, *ml_media, *other_ml_media;
	struct call_media *media, *other_media;
	unsigned int num_ports;
	struct call_monologue *monologue = other_ml->active_dialogue;
	struct endpoint_map *em;
	struct call *call;

	call = monologue->call;

	call->last_signal = poller_now;
	call->deleted = 0;

	/* we must have a complete dialogue, even though the to-tag (monologue->tag)
	 * may not be known yet */
	if (!other_ml) {
		ilog(LOG_ERROR, "Incomplete dialogue association");
		return -1;
	}
	__C_DBG("this="STR_FORMAT" other="STR_FORMAT, STR_FMT(&monologue->tag), STR_FMT(&other_ml->tag));

	__tos_change(call, flags);

	ml_media = other_ml_media = NULL;

	for (media_iter = streams->head; media_iter; media_iter = media_iter->next) {
		sp = media_iter->data;
		__C_DBG("processing media stream #%u", sp->index);

		/* first, check for existance of call_media struct on both sides of
		 * the dialogue */
		media = __get_media(monologue, &ml_media, sp);
		other_media = __get_media(other_ml, &other_ml_media, sp);
		/* OTHER is the side which has sent the message. SDP parameters in
		 * "sp" are as advertised by OTHER side. The message will be sent to
		 * THIS side. Parameters sent to THIS side may be overridden by
		 * what's in "flags". If this is an answer, or if we have talked to
		 * THIS side (recipient) before, then the structs will be populated with
		 * details already. */

		/* deduct protocol from stream parameters received */
		if (other_media->protocol != sp->protocol) {
			other_media->protocol = sp->protocol;
			/* if the endpoint changes the protocol, we reset the other side's
			 * protocol as well. this lets us remember our previous overrides,
			 * but also lets endpoints re-negotiate. */
			media->protocol = NULL;
		}
		/* allow override of outgoing protocol even if we know it already */
		if (flags && flags->transport_protocol)
			media->protocol = flags->transport_protocol;
		else if (!media->protocol)
			media->protocol = other_media->protocol;

		/* copy parameters advertised by the sender of this message */
		bf_copy_same(&other_media->media_flags, &sp->sp_flags,
				SHARED_FLAG_RTCP_MUX | SHARED_FLAG_ASYMMETRIC | SHARED_FLAG_ICE);

		crypto_params_copy(&other_media->sdes_in.params, &sp->crypto);
		other_media->sdes_in.tag = sp->sdes_tag;
		if (other_media->sdes_in.params.crypto_suite)
			MEDIA_SET(other_media, SDES);

		/* send and recv are from our POV */
		bf_copy_same(&media->media_flags, &sp->sp_flags,
				SP_FLAG_SEND | SP_FLAG_RECV);
		bf_copy(&other_media->media_flags, MEDIA_FLAG_RECV, &sp->sp_flags, SP_FLAG_SEND);
		bf_copy(&other_media->media_flags, MEDIA_FLAG_SEND, &sp->sp_flags, SP_FLAG_RECV);

		/* DTLS stuff */
		__dtls_logic(flags, media, other_media, sp);

		/* ICE negotiation */
		__ice_offer(flags, media, other_media);

		/* control rtcp-mux */
		__rtcp_mux_logic(flags, media, other_media);

		/* SDES and DTLS */
		__generate_crypto(flags, media, other_media);

		/* deduct address family from stream parameters received */
		other_media->desired_family = family_from_address(&sp->rtp_endpoint.ip46);
		/* for outgoing SDP, use "direction"/DF or default to what was offered */
		if (!media->desired_family)
			media->desired_family = other_media->desired_family;
		if (sp->desired_family)
			media->desired_family = sp->desired_family;


		/* local interface selection */
		__init_interface(media, &sp->direction[1]);
		__init_interface(other_media, &sp->direction[0]);


		/* we now know what's being advertised by the other side */
		MEDIA_SET(other_media, INITIALIZED);


		/* determine number of consecutive ports needed locally.
		 * XXX only do *=2 for RTP streams? */
		num_ports = sp->consecutive_ports;
		num_ports *= 2;


		if (!sp->rtp_endpoint.port) {
			/* Zero port: stream has been rejected.
			 * RFC 3264, chapter 6:
			 * If a stream is rejected, the offerer and answerer MUST NOT
			 * generate media (or RTCP packets) for that stream. */
			__disable_streams(media, num_ports);
			__disable_streams(other_media, num_ports);
			goto init;
		}
		if (is_addr_unspecified(&sp->rtp_endpoint.ip46)) {
			/* Zero endpoint address, equivalent to setting the media stream
			 * to sendonly or inactive */
			MEDIA_CLEAR(media, RECV);
			MEDIA_CLEAR(other_media, SEND);
		}


		/* get that many ports for each side, and one packet stream for each port, then
		 * assign the ports to the streams */
		em = __get_endpoint_map(media, num_ports, &sp->rtp_endpoint);
		if (!em)
			goto error;

		__num_media_streams(media, num_ports);
		__assign_stream_fds(media, em->sfds.head);

		if (__num_media_streams(other_media, num_ports)) {
			/* new streams created on OTHER side. normally only happens in
			 * initial offer. create a wildcard endpoint_map to be filled in
			 * when the answer comes. */
			if (__wildcard_endpoint_map(other_media, num_ports))
				goto error;
		}

init:
		if (__init_streams(media, other_media, NULL))
			return -1;
		if (__init_streams(other_media, media, sp))
			return -1;
	}

	return 0;

error:
	ilog(LOG_ERR, "Error allocating media ports");
	return -1;
}

/* must be called with in_lock held or call->master_lock held in W */
static void unkernelize(struct packet_stream *p) {
	if (!PS_ISSET(p, KERNELIZED))
		return;
	if (PS_ISSET(p, NO_KERNEL_SUPPORT))
		return;

	if (p->call->callmaster->conf.kernelfd >= 0)
		kernel_del_stream(p->call->callmaster->conf.kernelfd, p->sfd->fd.localport);

	PS_CLEAR(p, KERNELIZED);
}

void timeval_subtract (struct timeval *result, const struct timeval *a, const struct timeval *b) {
	long microseconds=0;
	microseconds = ((long)a->tv_sec - (long)b->tv_sec) * 1000000 + ((long)a->tv_usec - (long)b->tv_usec);
	result->tv_sec = microseconds/(long)1000000;
	result->tv_usec = microseconds%(long)1000000;
}

void timeval_multiply(struct timeval *result, const struct timeval *a, const long multiplier) {
	long microseconds=0;
	microseconds = (((long)a->tv_sec * 1000000) + (long)a->tv_usec) * multiplier;
	result->tv_sec = microseconds/(long)1000000;
	result->tv_usec = microseconds%(long)1000000;
}

void timeval_devide(struct timeval *result, const struct timeval *a, const long devisor) {
	long microseconds=0;
	microseconds = (((long)a->tv_sec * 1000000) + (long)a->tv_usec) / devisor;
	result->tv_sec = microseconds/(long)1000000;
	result->tv_usec = microseconds%(long)1000000;
}

void timeval_add(struct timeval *result, const struct timeval *a, const struct timeval *b) {
	long microseconds=0;
	microseconds = ((long)a->tv_sec + (long)b->tv_sec) * (long)1000000 + ((long)a->tv_usec + (long)b->tv_usec);
	result->tv_sec = microseconds/(long)1000000;
	result->tv_usec = microseconds%(long)1000000;
}

/* called lock-free, but must hold a reference to the call */
void call_destroy(struct call *c) {
	struct callmaster *m = c->callmaster;
	struct packet_stream *ps=0, *ps2=0;
	struct stream_fd *sfd;
	struct poller *p = m->poller;
	GSList *l;
	int ret;
	struct call_monologue *ml;
	struct call_media *md;
	GList *k, *o;
	char buf[64];
	struct timeval tim_result_duration;
	static const int CDRBUFLENGTH = 4096*2;
	char reasonbuf[16]; memset(&reasonbuf,0,16);
	char tagtypebuf[16]; memset(&tagtypebuf,0,16);
	char cdrbuffer[CDRBUFLENGTH]; memset(&cdrbuffer,0,CDRBUFLENGTH);
	char* cdrbufcur = cdrbuffer;
	int cdrlinecnt = 0;
	int found = 0;
	//char tmpstreampairstatus[2]; memset(&tmpstreampairstatus,0,2);

	rwlock_lock_w(&m->hashlock);
	ret = g_hash_table_remove(m->callhash, &c->callid);
	rwlock_unlock_w(&m->hashlock);

	if (!ret)
		return;

	obj_put(c);

	redis_delete(c, m->conf.redis);

	rwlock_lock_w(&c->master_lock);
	/* at this point, no more packet streams can be added */

	ilog(LOG_INFO, "Final packet stats:");

	/* CDRs and statistics */
	cdrbufcur += sprintf(cdrbufcur,"ci=%s, ",c->callid.s);
	cdrbufcur += sprintf(cdrbufcur,"created_from=%s, ", c->created_from);
	for (l = c->monologues; l; l = l->next) {
		ml = l->data;
		if (_log_facility_cdr) {
			memset(&tim_result_duration,0,sizeof(struct timeval));
			timeval_subtract(&tim_result_duration,&ml->terminated,&ml->started);
		    cdrbufcur += sprintf(cdrbufcur, "ml%i_start_time=%ld.%06lu, "
		            "ml%i_end_time=%ld.%06ld, "
		            "ml%i_duration=%ld.%06ld, "
		            "ml%i_termination=%s, "
		            "ml%i_local_tag=%s, "
		            "ml%i_local_tag_type=%s, "
		            "ml%i_remote_tag=%s, ",
		            cdrlinecnt, ml->started.tv_sec, ml->started.tv_usec,
		            cdrlinecnt, ml->terminated.tv_sec, ml->terminated.tv_usec,
		            cdrlinecnt, tim_result_duration.tv_sec, tim_result_duration.tv_usec,
		            cdrlinecnt, get_term_reason_text(reasonbuf,ml->term_reason),
		            cdrlinecnt, ml->tag.s,
		            cdrlinecnt, get_tag_type_text(tagtypebuf,ml->tagtype),
		            cdrlinecnt, ml->active_dialogue ? ml->active_dialogue->tag.s : "(none)");
		}

		ilog(LOG_INFO, "--- Tag '"STR_FORMAT"', created "
				"%u:%02u ago, in dialogue with '"STR_FORMAT"'",
				STR_FMT(&ml->tag),
				(unsigned int) (poller_now - ml->created) / 60,
				(unsigned int) (poller_now - ml->created) % 60,
				ml->active_dialogue ? ml->active_dialogue->tag.len : 6,
				ml->active_dialogue ? ml->active_dialogue->tag.s : "(none)");

		for (k = ml->medias.head; k; k = k->next) {
			md = k->data;

			for (o = md->streams.head; o; o = o->next) {
				ps = o->data;

				if (PS_ISSET(ps, FALLBACK_RTCP))
					continue;

				smart_ntop_p(buf, &ps->endpoint.ip46, sizeof(buf));

				if (_log_facility_cdr) {
				    const char* protocol = (!PS_ISSET(ps, RTP) && PS_ISSET(ps, RTCP)) ? "rtcp" : "rtp";
				    cdrbufcur += sprintf(cdrbufcur,
				            "ml%i_midx%u_%s_endpoint_ip=%s, "
				            "ml%i_midx%u_%s_endpoint_port=%u, "
				            "ml%i_midx%u_%s_local_relay_port=%u, "
				            "ml%i_midx%u_%s_relayed_packets=%llu, "
				            "ml%i_midx%u_%s_relayed_bytes=%llu, "
				            "ml%i_midx%u_%s_relayed_errors=%llu, ",
				            cdrlinecnt, md->index, protocol, buf,
				            cdrlinecnt, md->index, protocol, ps->endpoint.port,
				            cdrlinecnt, md->index, protocol, (unsigned int) (ps->sfd ? ps->sfd->fd.localport : 0),
				            cdrlinecnt, md->index, protocol, (unsigned long long) ps->stats.packets,
				            cdrlinecnt, md->index, protocol, (unsigned long long) ps->stats.bytes,
				            cdrlinecnt, md->index, protocol, (unsigned long long) ps->stats.errors);
				}

				ilog(LOG_INFO, "------ Media #%u, port %5u <> %15s:%-5hu%s, "
						"%llu p, %llu b, %llu e",
						md->index,
						(unsigned int) (ps->sfd ? ps->sfd->fd.localport : 0),
						buf, ps->endpoint.port,
						(!PS_ISSET(ps, RTP) && PS_ISSET(ps, RTCP)) ? " (RTCP)" : "",
						(unsigned long long) ps->stats.packets,
						(unsigned long long) ps->stats.bytes,
						(unsigned long long) ps->stats.errors);
				m->totalstats.total_relayed_packets += (unsigned long long) ps->stats.packets;
				m->totalstats.total_relayed_errors  += (unsigned long long) ps->stats.errors;
			}
		}
		if (_log_facility_cdr)
		    ++cdrlinecnt;
	}

	// --- for statistics getting one way stream or no relay at all
	m->totalstats.total_nopacket_relayed_sess *= 2;
	for (l = c->monologues; l; l = l->next) {
		ml = l->data;

		// --- go through partner ml and search the RTP
		for (k = ml->medias.head; k; k = k->next) {
			md = k->data;

			for (o = md->streams.head; o; o = o->next) {
				ps = o->data;
				if ((PS_ISSET(ps, RTP) && !PS_ISSET(ps, RTCP))) {
					// --- only RTP is interesting
					found = 1;
					break;
				}
			}
			if (found) { break; }
		}
		found = 0;

		if (ml->active_dialogue) {
			// --- go through partner ml and search the RTP
			for (k = ml->active_dialogue->medias.head; k; k = k->next) {
				md = k->data;

				for (o = md->streams.head; o; o = o->next) {
					ps2 = o->data;
					if ((PS_ISSET(ps2, RTP) && !PS_ISSET(ps2, RTCP))) {
						// --- only RTP is interesting
						found = 1;
						break;
					}
				}
				if (found) { break; }
			}
		}

		if (ps && ps2 && ps->stats.packets!=0 && ps2->stats.packets==0)
			m->totalstats.total_oneway_stream_sess++;

		if (ps && ps2 && ps->stats.packets==0 && ps2->stats.packets==0)
			m->totalstats.total_nopacket_relayed_sess++;

	}
	m->totalstats.total_nopacket_relayed_sess /= 2;

	m->totalstats.total_managed_sess += 1;

	ml = c->monologues->data;
	if (ml->term_reason==TIMEOUT) {
		m->totalstats.total_timeout_sess++;
	} else if (ml->term_reason==SILENT_TIMEOUT) {
		m->totalstats.total_silent_timeout_sess++;
	} else if (ml->term_reason==REGULAR) {
		m->totalstats.total_regular_term_sess++;
	} else if (ml->term_reason==FORCED) {
		m->totalstats.total_forced_term_sess++;
	}

	timeval_multiply(&m->totalstats.total_average_call_dur,&m->totalstats.total_average_call_dur,m->totalstats.total_managed_sess-1);
	timeval_add(&m->totalstats.total_average_call_dur,&m->totalstats.total_average_call_dur,&tim_result_duration);
	timeval_devide(&m->totalstats.total_average_call_dur,&m->totalstats.total_average_call_dur,m->totalstats.total_managed_sess);

	if (_log_facility_cdr)
	    /* log it */
	    cdrlog(cdrbuffer);

	for (l = c->streams; l; l = l->next) {
		ps = l->data;

		unkernelize(ps);
		dtls_shutdown(ps);
		ps->sfd = NULL;
		crypto_cleanup(&ps->crypto);

		ps->rtp_sink = NULL;
		ps->rtcp_sink = NULL;
	}

	while (c->stream_fds) {
		sfd = c->stream_fds->data;
                if (m->conf.libjitter.close != NULL) {
                         m->conf.libjitter.close(sfd->fd.fd);
                }
		c->stream_fds = g_slist_delete_link(c->stream_fds, c->stream_fds);
		poller_del_item(p, sfd->fd.fd);
		obj_put(sfd);
	}

	rwlock_unlock_w(&c->master_lock);
}



static int call_stream_address4(char *o, struct packet_stream *ps, enum stream_address_format format,
		int *len, struct interface_address *ifa)
{
	u_int32_t ip4;
	int l = 0;

	if (format == SAF_NG) {
		strcpy(o + l, "IP4 ");
		l = 4;
	}

	if (!in6_to_4(&ps->advertised_endpoint.ip46)) {
		strcpy(o + l, "0.0.0.0");
		l += 7;
	}
	else {
		ip4 = in6_to_4(&ifa->advertised);
		l += sprintf(o + l, IPF, IPP(ip4));
	}

	*len = l;
	return AF_INET;
}

static int call_stream_address6(char *o, struct packet_stream *ps, enum stream_address_format format,
		int *len, struct interface_address *ifa)
{
	int l = 0;

	if (format == SAF_NG) {
		strcpy(o + l, "IP6 ");
		l += 4;
	}

	if (is_addr_unspecified(&ps->advertised_endpoint.ip46)) {
		strcpy(o + l, "::");
		l += 2;
	}
	else {
		inet_ntop(AF_INET6, &ifa->advertised, o + l, 45); /* lies ... */
		l += strlen(o + l);
	}

	*len = l;
	return AF_INET6;
}


int call_stream_address46(char *o, struct packet_stream *ps, enum stream_address_format format,
		int *len, struct interface_address *ifa)
{
	struct packet_stream *sink;

	sink = packet_stream_sink(ps);
	if (ifa->family == AF_INET)
		return call_stream_address4(o, sink, format, len, ifa);
	return call_stream_address6(o, sink, format, len, ifa);
}

int call_stream_address(char *o, struct packet_stream *ps, enum stream_address_format format, int *len) {
	struct interface_address *ifa;
	struct call_media *media;

	media = ps->media;

	ifa = g_atomic_pointer_get(&media->local_address);
	if (!ifa)
		return -1;

	return call_stream_address46(o, ps, format, len, ifa);
}


static void __call_free(void *p) {
	struct call *c = p;
	struct call_monologue *m;
	struct call_media *md;
	struct packet_stream *ps;
	struct endpoint_map *em;
	GList *it;

	__C_DBG("freeing call struct");

	call_buffer_free(&c->buffer);
	mutex_destroy(&c->buffer_lock);
	rwlock_destroy(&c->master_lock);
	obj_put(c->dtls_cert);

	while (c->monologues) {
		m = c->monologues->data;
		c->monologues = g_slist_delete_link(c->monologues, c->monologues);

		g_hash_table_destroy(m->other_tags);

		for (it = m->medias.head; it; it = it->next) {
			md = it->data;
			g_queue_clear(&md->streams);
			while (md->endpoint_maps) {
				em = md->endpoint_maps->data;
				md->endpoint_maps = g_slist_delete_link(md->endpoint_maps, md->endpoint_maps);
				g_queue_clear(&em->sfds);
				g_slice_free1(sizeof(*em), em);
			}
			g_slice_free1(sizeof(*md), md);
		}
		g_queue_clear(&m->medias);

		g_slice_free1(sizeof(*m), m);
	}

	g_hash_table_destroy(c->tags);

	while (c->streams) {
		ps = c->streams->data;
		c->streams = g_slist_delete_link(c->streams, c->streams);
		g_slice_free1(sizeof(*ps), ps);
	}

	assert(c->stream_fds == NULL);
}

static struct call *call_create(const str *callid, struct callmaster *m) {
	struct call *c;

	ilog(LOG_NOTICE, "["STR_FORMAT"] Creating new call",
		STR_FMT(callid));
	c = obj_alloc0("call", sizeof(*c), __call_free);
	c->callmaster = m;
	mutex_init(&c->buffer_lock);
	call_buffer_init(&c->buffer);
	rwlock_init(&c->master_lock);
	c->tags = g_hash_table_new(str_hash, str_equal);
	call_str_cpy(c, &c->callid, callid);
	c->created = poller_now;
	c->dtls_cert = dtls_cert();
	c->tos = m->conf.default_tos;
	return c;
}

/* returns call with master_lock held in W */
struct call *call_get_or_create(const str *callid, struct callmaster *m) {
	struct call *c;

restart:
	rwlock_lock_r(&m->hashlock);
	c = g_hash_table_lookup(m->callhash, callid);
	if (!c) {
		rwlock_unlock_r(&m->hashlock);
		/* completely new call-id, create call */
		c = call_create(callid, m);
		rwlock_lock_w(&m->hashlock);
		if (g_hash_table_lookup(m->callhash, callid)) {
			/* preempted */
			rwlock_unlock_w(&m->hashlock);
			obj_put(c);
			goto restart;
		}
		g_hash_table_insert(m->callhash, &c->callid, obj_get(c));
		rwlock_lock_w(&c->master_lock);
		rwlock_unlock_w(&m->hashlock);
	}
	else {
		obj_hold(c);
		rwlock_lock_w(&c->master_lock);
		rwlock_unlock_r(&m->hashlock);
	}

	log_info_call(c);
	return c;
}

/* returns call with master_lock held in W, or NULL if not found */
struct call *call_get(const str *callid, struct callmaster *m) {
	struct call *ret;

	rwlock_lock_r(&m->hashlock);
	ret = g_hash_table_lookup(m->callhash, callid);
	if (!ret) {
		rwlock_unlock_r(&m->hashlock);
		return NULL;
	}

	rwlock_lock_w(&ret->master_lock);
	obj_hold(ret);
	rwlock_unlock_r(&m->hashlock);

	log_info_call(ret);
	return ret;
}

/* returns call with master_lock held in W, or possibly NULL iff opmode == OP_ANSWER */
struct call *call_get_opmode(const str *callid, struct callmaster *m, enum call_opmode opmode) {
	if (opmode == OP_OFFER)
		return call_get_or_create(callid, m);
	return call_get(callid, m);
}

/* must be called with call->master_lock held in W */
struct call_monologue *__monologue_create(struct call *call) {
	struct call_monologue *ret;

	__C_DBG("creating new monologue");
	ret = g_slice_alloc0(sizeof(*ret));

	ret->call = call;
	ret->created = poller_now;
	ret->other_tags = g_hash_table_new(str_hash, str_equal);
	g_queue_init(&ret->medias);

	call->monologues = g_slist_prepend(call->monologues, ret);

	return ret;
}

/* must be called with call->master_lock held in W */
void __monologue_tag(struct call_monologue *ml, const str *tag) {
	struct call *call = ml->call;

	__C_DBG("tagging monologue with '"STR_FORMAT"'", STR_FMT(tag));
	call_str_cpy(call, &ml->tag, tag);
	g_hash_table_insert(call->tags, &ml->tag, ml);
}

static void __stream_unkernelize(struct packet_stream *ps) {
	unkernelize(ps);
	PS_CLEAR(ps, CONFIRMED);
	PS_CLEAR(ps, HAS_HANDLER);
}
static void stream_unkernelize(struct packet_stream *ps) {
	if (!ps)
		return;
	mutex_lock(&ps->in_lock);
	__stream_unkernelize(ps);
	mutex_unlock(&ps->in_lock);
}

/* must be called with call->master_lock held in W */
static void __monologue_unkernelize(struct call_monologue *monologue) {
	GList *l, *m;
	struct call_media *media;
	struct packet_stream *stream;

	if (!monologue)
		return;

	monologue->deleted = 0; /* not really related, but indicates activity, so cancel
				   any pending deletion */

	for (l = monologue->medias.head; l; l = l->next) {
		media = l->data;

		for (m = media->streams.head; m; m = m->next) {
			stream = m->data;
			__stream_unkernelize(stream);
			if (stream->rtp_sink)
				__stream_unkernelize(stream->rtp_sink);
			if (stream->rtcp_sink)
				__stream_unkernelize(stream->rtcp_sink);
		}
	}
}

/* must be called with call->master_lock held in W */
static void __monologue_destroy(struct call_monologue *monologue) {
	struct call *call;
	struct call_monologue *dialogue;
	GList *l;

	call = monologue->call;

	g_hash_table_remove(call->tags, &monologue->tag);

	l = g_hash_table_get_values(monologue->other_tags);

	while (l) {
		dialogue = l->data;
		l = g_list_delete_link(l, l);
		g_hash_table_remove(dialogue->other_tags, &monologue->tag);
		if (!g_hash_table_size(dialogue->other_tags))
			__monologue_destroy(dialogue);
	}
}

/* must be called with call->master_lock held in W */
static struct call_monologue *call_get_monologue(struct call *call, const str *fromtag) {
	struct call_monologue *ret;

	__C_DBG("getting monologue for tag '"STR_FORMAT"' in call '"STR_FORMAT"'",
			STR_FMT(fromtag), STR_FMT(&call->callid));
	ret = g_hash_table_lookup(call->tags, fromtag);
	if (ret) {
		__C_DBG("found existing monologue");
		__monologue_unkernelize(ret);
		__monologue_unkernelize(ret->active_dialogue);
		return ret;
	}

	ret = __monologue_create(call);
	__monologue_tag(ret, fromtag);
	/* we need both sides of the dialogue even in the initial offer, so create
	 * another monologue without to-tag (to be filled in later) */
	ret->active_dialogue = __monologue_create(call);

	return ret;
}

/* must be called with call->master_lock held in W */
static struct call_monologue *call_get_dialogue(struct call *call, const str *fromtag, const str *totag) {
	struct call_monologue *ft, *ret, *tt;

	__C_DBG("getting dialogue for tags '"STR_FORMAT"'<>'"STR_FORMAT"' in call '"STR_FORMAT"'",
			STR_FMT(fromtag), STR_FMT(totag), STR_FMT(&call->callid));
	/* if the to-tag is known already, return that */
	tt = g_hash_table_lookup(call->tags, totag);
	if (tt) {
		__C_DBG("found existing dialogue");
		__monologue_unkernelize(tt);
		__monologue_unkernelize(tt->active_dialogue);

		/* make sure that the dialogue is actually intact */
		if (!str_cmp_str(fromtag, &tt->active_dialogue->tag))
			return tt;
	}

	/* otherwise, at least the from-tag has to be known. it's an error if it isn't */
	ft = g_hash_table_lookup(call->tags, fromtag);
	if (!ft)
		return NULL;

	__monologue_unkernelize(ft);

	/* check for a half-complete dialogue and fill in the missing half if possible */
	ret = ft->active_dialogue;
	__monologue_unkernelize(ret);

	if (!ret->tag.s)
		goto tag;

	/* we may have seen both tags previously and they just need to be linked up */
	if (tt) {
		ret = tt;
		goto link;
	}

	/* this is an additional dialogue created from a single from-tag */
	ret = __monologue_create(call);

tag:
	__monologue_tag(ret, totag);
link:
	g_hash_table_insert(ret->other_tags, &ft->tag, ft);
	g_hash_table_insert(ft->other_tags, &ret->tag, ret);
	ret->active_dialogue = ft;
	ft->active_dialogue = ret;

	return ret;
}

struct call_monologue *call_get_mono_dialogue(struct call *call, const str *fromtag, const str *totag) {
	if (!totag || !totag->s) /* offer, not answer */
		return call_get_monologue(call, fromtag);
	return call_get_dialogue(call, fromtag, totag);
}


int call_delete_branch(struct callmaster *m, const str *callid, const str *branch,
	const str *fromtag, const str *totag, bencode_item_t *output)
{
	struct call *c;
	struct call_monologue *ml;
	int ret;
	const str *match_tag;
	GSList *i;

	c = call_get(callid, m);
	if (!c) {
		ilog(LOG_INFO, "["STR_FORMAT"] Call-ID to delete not found", STR_FMT(callid));
		goto err;
	}

	for (i = c->monologues; i; i = i->next) {
		ml = i->data;
		memset(&ml->terminated,0,sizeof(struct timeval));
		gettimeofday(&(ml->terminated), NULL);
		ml->term_reason = REGULAR;
	}

	if (!fromtag || !fromtag->s || !fromtag->len)
		goto del_all;

	match_tag = (totag && totag->s && totag->len) ? totag : fromtag;

	ml = g_hash_table_lookup(c->tags, match_tag);
	if (!ml) {
		ilog(LOG_INFO, "Tag '"STR_FORMAT"' in delete message not found, ignoring",
				STR_FMT(match_tag));
		goto err;
	}

	if (output)
		ng_call_stats(c, fromtag, totag, output, NULL);

/*
	if (branch && branch->len) {
		if (!g_hash_table_remove(c->branches, branch)) {
			ilog(LOG_INFO, LOG_PREFIX_CI "Branch to delete doesn't exist", STR_FMT(&c->callid), STR_FMT(branch));
			goto err;
		}

		ilog(LOG_INFO, LOG_PREFIX_CI "Branch deleted", LOG_PARAMS_CI(c));
		if (g_hash_table_size(c->branches))
			goto success_unlock;
		else
			DBG("no branches left, deleting full call");
	}
*/

	ilog(LOG_INFO, "Scheduling deletion of call branch '"STR_FORMAT"' in %d seconds",
			STR_FMT(&ml->tag), m->conf.delete_delay);
	ml->deleted = poller_now + m->conf.delete_delay;
	if (!c->ml_deleted || c->ml_deleted > ml->deleted)
		c->ml_deleted = ml->deleted;
	goto success_unlock;

del_all:
	ilog(LOG_INFO, "Scheduling deletion of entire call in %d seconds", m->conf.delete_delay);
	c->deleted = poller_now + m->conf.delete_delay;
	rwlock_unlock_w(&c->master_lock);
	goto success;

success_unlock:
	rwlock_unlock_w(&c->master_lock);
success:
	ret = 0;
	goto out;

err:
	if (c)
		rwlock_unlock_w(&c->master_lock);
	ret = -1;
	goto out;

out:
	if (c)
		obj_put(c);
	return ret;
}


static void callmaster_get_all_calls_interator(void *key, void *val, void *ptr) {
	GQueue *q = ptr;
	g_queue_push_tail(q, obj_get_o(val));
}

void callmaster_get_all_calls(struct callmaster *m, GQueue *q) {
	rwlock_lock_r(&m->hashlock);
	g_hash_table_foreach(m->callhash, callmaster_get_all_calls_interator, q);
	rwlock_unlock_r(&m->hashlock);

}


static void calls_dump_iterator(void *key, void *val, void *ptr) {
	struct call *c = val;
	struct callmaster *m = c->callmaster;

	redis_update(c, m->conf.redis);
}

void calls_dump_redis(struct callmaster *m) {
	if (!m->conf.redis)
		return;

	ilog(LOG_DEBUG, "Start dumping all call data to Redis...\n");
	redis_wipe_mod(m->conf.redis);
	g_hash_table_foreach(m->callhash, calls_dump_iterator, NULL);
	ilog(LOG_DEBUG, "Finished dumping all call data to Redis\n");
}

const struct transport_protocol *transport_protocol(const str *s) {
	int i;

	if (!s || !s->s)
		goto out;

	for (i = 0; i < num_transport_protocols; i++) {
		if (strlen(transport_protocols[i].name) != s->len)
			continue;
		if (strncasecmp(transport_protocols[i].name, s->s, s->len))
			continue;
		return &transport_protocols[i];
	}

out:
	return NULL;
}

void callmaster_config_init(struct callmaster *m) {
	GList *l;
	struct interface_address *ifa;
	struct local_interface *lif;

	m->interfaces = g_hash_table_new(str_hash, str_equal);

	for (l = m->conf.interfaces->head; l; l = l->next) {
		ifa = l->data;

		lif = g_hash_table_lookup(m->interfaces, &ifa->interface_name);
		if (!lif) {
			lif = g_slice_alloc0(sizeof(*lif));
			lif->name = ifa->interface_name;
			g_hash_table_insert(m->interfaces, &lif->name, lif);
			g_queue_push_tail(&m->interface_list, lif);
		}

		if (IN6_IS_ADDR_V4MAPPED(&ifa->addr))
			g_queue_push_tail(&lif->ipv4, ifa);
		else
			g_queue_push_tail(&lif->ipv6, ifa);

		sdp_ice_foundation(ifa);
	}
}

struct local_interface *get_local_interface(struct callmaster *m, const str *name) {
	struct local_interface *lif;

	if (!name || !name->s)
		return m->interface_list.head->data;

	lif = g_hash_table_lookup(m->interfaces, name);
	return lif;
}

static const GQueue *get_interface_addresses(struct local_interface *lif, int family) {
	if (!lif)
		return NULL;

	switch (family) {
		case AF_INET:
			return &lif->ipv4;
			break;
		case AF_INET6:
			return &lif->ipv6;
			break;
		default:
			return NULL;
	}
}

static struct interface_address *get_interface_address(struct local_interface *lif, int family) {
	const GQueue *q;

	q = get_interface_addresses(lif, family);
	if (!q || !q->head)
		return NULL;
	return q->head->data;
}

/* safety fallback */
struct interface_address *get_any_interface_address(struct local_interface *lif, int family) {
	struct interface_address *ifa;
	GQueue q = G_QUEUE_INIT;

	get_all_interface_addresses(&q, lif, family);
	ifa = q.head->data;
	g_queue_clear(&q);
	return ifa;
}

void get_all_interface_addresses(GQueue *q, struct local_interface *lif, int family) {
	g_queue_append(q, get_interface_addresses(lif, family));
	if (family == AF_INET)
		g_queue_append(q, get_interface_addresses(lif, AF_INET6));
	else
		g_queue_append(q, get_interface_addresses(lif, AF_INET));
}

struct interface_address *get_interface_from_address(struct local_interface *lif, const struct in6_addr *addr) {
	GQueue *q;
	GList *l;
	struct interface_address *ifa;

	if (IN6_IS_ADDR_V4MAPPED(addr))
		q = &lif->ipv4;
	else
		q = &lif->ipv6;

	for (l = q->head; l; l = l->next) {
		ifa = l->data;
		if (!memcmp(&ifa->addr, addr, sizeof(*addr)))
			return ifa;
	}

	return NULL;
}
