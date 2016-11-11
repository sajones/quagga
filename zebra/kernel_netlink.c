/* Kernel communication using netlink interface.
 * Copyright (C) 1999 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.  
 */

#include <zebra.h>

#include "linklist.h"
#include "if.h"
#include "log.h"
#include "prefix.h"
#include "connected.h"
#include "table.h"
#include "memory.h"
#include "zebra_memory.h"
#include "rib.h"
#include "thread.h"
#include "privs.h"
#include "nexthop.h"
#include "vrf.h"
#include "mpls.h"

#include "zebra/zserv.h"
#include "zebra/zebra_ns.h"
#include "zebra/zebra_vrf.h"
#include "zebra/debug.h"
#include "zebra/kernel_netlink.h"
#include "zebra/rt_netlink.h"
#include "zebra/if_netlink.h"

#ifndef SO_RCVBUFFORCE
#define SO_RCVBUFFORCE  (33)
#endif

/* Hack for GNU libc version 2. */
#ifndef MSG_TRUNC
#define MSG_TRUNC      0x20
#endif /* MSG_TRUNC */

#ifndef NLMSG_TAIL
#define NLMSG_TAIL(nmsg) \
        ((struct rtattr *) (((u_char *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))
#endif

#ifndef RTA_TAIL
#define RTA_TAIL(rta) \
        ((struct rtattr *) (((u_char *) (rta)) + RTA_ALIGN((rta)->rta_len)))
#endif

static const struct message nlmsg_str[] = {
  {RTM_NEWROUTE, "RTM_NEWROUTE"},
  {RTM_DELROUTE, "RTM_DELROUTE"},
  {RTM_GETROUTE, "RTM_GETROUTE"},
  {RTM_NEWLINK,  "RTM_NEWLINK"},
  {RTM_DELLINK,  "RTM_DELLINK"},
  {RTM_GETLINK,  "RTM_GETLINK"},
  {RTM_NEWADDR,  "RTM_NEWADDR"},
  {RTM_DELADDR,  "RTM_DELADDR"},
  {RTM_GETADDR,  "RTM_GETADDR"},
  {RTM_NEWNEIGH, "RTM_NEWNEIGH"},
  {RTM_DELNEIGH, "RTM_DELNEIGH"},
  {RTM_GETNEIGH, "RTM_GETNEIGH"},
  {0, NULL}
};

static const struct message rtproto_str[] = {
  {RTPROT_REDIRECT, "redirect"},
  {RTPROT_KERNEL,   "kernel"},
  {RTPROT_BOOT,     "boot"},
  {RTPROT_STATIC,   "static"},
  {RTPROT_GATED,    "GateD"},
  {RTPROT_RA,       "router advertisement"},
  {RTPROT_MRT,      "MRT"},
  {RTPROT_ZEBRA,    "Zebra"},
#ifdef RTPROT_BIRD
  {RTPROT_BIRD,     "BIRD"},
#endif /* RTPROT_BIRD */
  {0,               NULL}
};

static const struct message family_str[] = {
  {AF_INET,           "ipv4"},
  {AF_INET6,          "ipv6"},
  {AF_BRIDGE,         "bridge"},
  {RTNL_FAMILY_IPMR,  "ipv4MR"},
  {RTNL_FAMILY_IP6MR, "ipv6MR"},
  {0,                 NULL},
};

static const struct message rttype_str[] = {
  {RTN_UNICAST,   "unicast"},
  {RTN_MULTICAST, "multicast"},
  {0,             NULL},
};

extern struct thread_master *master;
extern u_int32_t nl_rcvbufsize;

extern struct zebra_privs_t zserv_privs;

int
netlink_talk_filter (struct sockaddr_nl *snl, struct nlmsghdr *h,
    ns_id_t ns_id)
{
  zlog_warn ("netlink_talk: ignoring message type 0x%04x NS %u", h->nlmsg_type,
             ns_id);
  return 0;
}

static int
netlink_recvbuf (struct nlsock *nl, uint32_t newsize)
{
  u_int32_t oldsize;
  socklen_t newlen = sizeof(newsize);
  socklen_t oldlen = sizeof(oldsize);
  int ret;

  ret = getsockopt(nl->sock, SOL_SOCKET, SO_RCVBUF, &oldsize, &oldlen);
  if (ret < 0)
    {
      zlog (NULL, LOG_ERR, "Can't get %s receive buffer size: %s", nl->name,
	    safe_strerror (errno));
      return -1;
    }

  /* Try force option (linux >= 2.6.14) and fall back to normal set */
  if ( zserv_privs.change (ZPRIVS_RAISE) )
    zlog_err ("routing_socket: Can't raise privileges");
  ret = setsockopt(nl->sock, SOL_SOCKET, SO_RCVBUFFORCE, &nl_rcvbufsize,
		   sizeof(nl_rcvbufsize));
  if ( zserv_privs.change (ZPRIVS_LOWER) )
    zlog_err ("routing_socket: Can't lower privileges");
  if (ret < 0)
     ret = setsockopt(nl->sock, SOL_SOCKET, SO_RCVBUF, &nl_rcvbufsize,
		      sizeof(nl_rcvbufsize));
  if (ret < 0)
    {
      zlog (NULL, LOG_ERR, "Can't set %s receive buffer size: %s", nl->name,
	    safe_strerror (errno));
      return -1;
    }

  ret = getsockopt(nl->sock, SOL_SOCKET, SO_RCVBUF, &newsize, &newlen);
  if (ret < 0)
    {
      zlog (NULL, LOG_ERR, "Can't get %s receive buffer size: %s", nl->name,
	    safe_strerror (errno));
      return -1;
    }

  zlog (NULL, LOG_INFO,
	"Setting netlink socket receive buffer size: %u -> %u",
	oldsize, newsize);
  return 0;
}

/* Make socket for Linux netlink interface. */
static int
netlink_socket (struct nlsock *nl, unsigned long groups, ns_id_t ns_id)
{
  int ret;
  struct sockaddr_nl snl;
  int sock;
  int namelen;
  int save_errno;

  if (zserv_privs.change (ZPRIVS_RAISE))
    {
      zlog (NULL, LOG_ERR, "Can't raise privileges");
      return -1;
    }

  sock = socket (AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (sock < 0)
    {
      zlog (NULL, LOG_ERR, "Can't open %s socket: %s", nl->name,
            safe_strerror (errno));
      return -1;
    }

  memset (&snl, 0, sizeof snl);
  snl.nl_family = AF_NETLINK;
  snl.nl_groups = groups;

  /* Bind the socket to the netlink structure for anything. */
  ret = bind (sock, (struct sockaddr *) &snl, sizeof snl);
  save_errno = errno;
  if (zserv_privs.change (ZPRIVS_LOWER))
    zlog (NULL, LOG_ERR, "Can't lower privileges");

  if (ret < 0)
    {
      zlog (NULL, LOG_ERR, "Can't bind %s socket to group 0x%x: %s",
            nl->name, snl.nl_groups, safe_strerror (save_errno));
      close (sock);
      return -1;
    }

  /* multiple netlink sockets will have different nl_pid */
  namelen = sizeof snl;
  ret = getsockname (sock, (struct sockaddr *) &snl, (socklen_t *) &namelen);
  if (ret < 0 || namelen != sizeof snl)
    {
      zlog (NULL, LOG_ERR, "Can't get %s socket name: %s", nl->name,
            safe_strerror (errno));
      close (sock);
      return -1;
    }

  nl->snl = snl;
  nl->sock = sock;
  return ret;
}

static int
netlink_information_fetch (struct sockaddr_nl *snl, struct nlmsghdr *h,
    ns_id_t ns_id)
{
  /* JF: Ignore messages that aren't from the kernel */
  if ( snl->nl_pid != 0 )
    {
      zlog ( NULL, LOG_ERR, "Ignoring message from pid %u", snl->nl_pid );
      return 0;
    }

  switch (h->nlmsg_type)
    {
    case RTM_NEWROUTE:
      return netlink_route_change (snl, h, ns_id);
      break;
    case RTM_DELROUTE:
      return netlink_route_change (snl, h, ns_id);
      break;
    case RTM_NEWLINK:
      return netlink_link_change (snl, h, ns_id);
      break;
    case RTM_DELLINK:
      return netlink_link_change (snl, h, ns_id);
      break;
    case RTM_NEWADDR:
      return netlink_interface_addr (snl, h, ns_id);
      break;
    case RTM_DELADDR:
      return netlink_interface_addr (snl, h, ns_id);
      break;
    case RTM_NEWNEIGH:
      return netlink_neigh_change (snl, h, ns_id);
      break;
    case RTM_DELNEIGH:
      return netlink_neigh_change (snl, h, ns_id);
      break;
    default:
      zlog_warn ("Unknown netlink nlmsg_type %d vrf %u\n", h->nlmsg_type,
                 ns_id);
      break;
    }
  return 0;
}

static int
kernel_read (struct thread *thread)
{
  struct zebra_ns *zns = (struct zebra_ns *)THREAD_ARG (thread);
  netlink_parse_info (netlink_information_fetch, &zns->netlink, zns, 5);
  zns->t_netlink = thread_add_read (zebrad.master, kernel_read, zns,
                                     zns->netlink.sock);

  return 0;
}

/* Filter out messages from self that occur on listener socket,
 * caused by our actions on the command socket
 */
static void netlink_install_filter (int sock, __u32 pid)
{
  struct sock_filter filter[] = {
    /* 0: ldh [4]	          */
    BPF_STMT(BPF_LD|BPF_ABS|BPF_H, offsetof(struct nlmsghdr, nlmsg_type)),
    /* 1: jeq 0x18 jt 5 jf next  */
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, htons(RTM_NEWROUTE), 3, 0),
    /* 2: jeq 0x19 jt 5 jf next  */
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, htons(RTM_DELROUTE), 2, 0),
    /* 3: jeq 0x19 jt 5 jf next  */
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, htons(RTM_NEWNEIGH), 1, 0),
    /* 4: jeq 0x19 jt 5 jf 8  */
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, htons(RTM_DELNEIGH), 0, 3),
    /* 5: ldw [12]		  */
    BPF_STMT(BPF_LD|BPF_ABS|BPF_W, offsetof(struct nlmsghdr, nlmsg_pid)),
    /* 6: jeq XX  jt 7 jf 8   */
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, htonl(pid), 0, 1),
    /* 7: ret 0    (skip)     */
    BPF_STMT(BPF_RET|BPF_K, 0),
    /* 8: ret 0xffff (keep)   */
    BPF_STMT(BPF_RET|BPF_K, 0xffff),
  };

  struct sock_fprog prog = {
    .len = array_size(filter),
    .filter = filter,
  };

  if (setsockopt(sock, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0)
    zlog_warn ("Can't install socket filter: %s\n", safe_strerror(errno));
}

void
netlink_parse_rtattr (struct rtattr **tb, int max, struct rtattr *rta,
                      int len)
{
  while (RTA_OK (rta, len))
    {
      if (rta->rta_type <= max)
        tb[rta->rta_type] = rta;
      rta = RTA_NEXT (rta, len);
    }
}

int
addattr_l (struct nlmsghdr *n, unsigned int maxlen, int type,
	   void *data, unsigned int alen)
{
  int len;
  struct rtattr *rta;

  len = RTA_LENGTH (alen);

  if (NLMSG_ALIGN (n->nlmsg_len) + RTA_ALIGN (len) > maxlen)
    return -1;

  rta = (struct rtattr *) (((char *) n) + NLMSG_ALIGN (n->nlmsg_len));
  rta->rta_type = type;
  rta->rta_len = len;
  memcpy (RTA_DATA (rta), data, alen);
  n->nlmsg_len = NLMSG_ALIGN (n->nlmsg_len) + RTA_ALIGN (len);

  return 0;
}

int
rta_addattr_l (struct rtattr *rta, unsigned int maxlen, int type,
	       void *data, unsigned int alen)
{
  unsigned int len;
  struct rtattr *subrta;

  len = RTA_LENGTH (alen);

  if (RTA_ALIGN (rta->rta_len) + RTA_ALIGN (len) > maxlen)
    return -1;

  subrta = (struct rtattr *) (((char *) rta) + RTA_ALIGN (rta->rta_len));
  subrta->rta_type = type;
  subrta->rta_len = len;
  memcpy (RTA_DATA (subrta), data, alen);
  rta->rta_len = NLMSG_ALIGN (rta->rta_len) + RTA_ALIGN (len);

  return 0;
}

int
addattr32 (struct nlmsghdr *n, unsigned int maxlen, int type, int data)
{
  return addattr_l(n, maxlen, type, &data, sizeof(u_int32_t));
}

struct rtattr *
addattr_nest(struct nlmsghdr *n, int maxlen, int type)
{
  struct rtattr *nest = NLMSG_TAIL(n);

  addattr_l(n, maxlen, type, NULL, 0);
  return nest;
}

int
addattr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
  nest->rta_len = (u_char *)NLMSG_TAIL(n) - (u_char *)nest;
  return n->nlmsg_len;
}

struct rtattr *
rta_nest(struct rtattr *rta, int maxlen, int type)
{
  struct rtattr *nest = RTA_TAIL(rta);

  rta_addattr_l(rta, maxlen, type, NULL, 0);
  return nest;
}

int
rta_nest_end(struct rtattr *rta, struct rtattr *nest)
{
  nest->rta_len = (u_char *)RTA_TAIL(rta) - (u_char *)nest;
  return rta->rta_len;
}

const char *
nl_msg_type_to_str (uint16_t msg_type)
{
  return lookup (nlmsg_str, msg_type);
}

const char *
nl_rtproto_to_str (u_char rtproto)
{
  return lookup (rtproto_str, rtproto);
}

const char *
nl_family_to_str (u_char family)
{
  return lookup (family_str, family);
}

const char *
nl_rttype_to_str (u_char rttype)
{
  return lookup (rttype_str, rttype);
}

/* Receive message from netlink interface and pass those information
   to the given function. */
int
netlink_parse_info (int (*filter) (struct sockaddr_nl *, struct nlmsghdr *,
                                   ns_id_t),
                    struct nlsock *nl, struct zebra_ns *zns, int count)
{
  int status;
  int ret = 0;
  int error;
  int read_in = 0;

  while (1)
    {
      char buf[NL_PKT_BUF_SIZE];
      struct iovec iov = {
        .iov_base = buf,
        .iov_len = sizeof buf
      };
      struct sockaddr_nl snl;
      struct msghdr msg = {
        .msg_name = (void *) &snl,
        .msg_namelen = sizeof snl,
        .msg_iov = &iov,
        .msg_iovlen = 1
      };
      struct nlmsghdr *h;

      if (count && read_in >= count)
        return 0;

      status = recvmsg (nl->sock, &msg, 0);
      if (status < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EWOULDBLOCK || errno == EAGAIN)
            break;
          zlog (NULL, LOG_ERR, "%s recvmsg overrun: %s",
                nl->name, safe_strerror(errno));
          /*
           *  In this case we are screwed.
           *  There is no good way to
           *  recover zebra at this point.
           */
          exit (-1);
          continue;
        }

      if (status == 0)
        {
          zlog (NULL, LOG_ERR, "%s EOF", nl->name);
          return -1;
        }

      if (msg.msg_namelen != sizeof snl)
        {
          zlog (NULL, LOG_ERR, "%s sender address length error: length %d",
                nl->name, msg.msg_namelen);
          return -1;
        }

      if (IS_ZEBRA_DEBUG_KERNEL_MSGDUMP_RECV)
        {
          zlog_debug("%s: << netlink message dump [recv]", __func__);
          zlog_hexdump(&msg, sizeof(msg));
        }

      read_in++;
      for (h = (struct nlmsghdr *) buf; NLMSG_OK (h, (unsigned int) status);
           h = NLMSG_NEXT (h, status))
        {
          /* Finish of reading. */
          if (h->nlmsg_type == NLMSG_DONE)
            return ret;

          /* Error handling. */
          if (h->nlmsg_type == NLMSG_ERROR)
            {
              struct nlmsgerr *err = (struct nlmsgerr *) NLMSG_DATA (h);
	      int errnum = err->error;
	      int msg_type = err->msg.nlmsg_type;

              /* If the error field is zero, then this is an ACK */
              if (err->error == 0)
                {
                  if (IS_ZEBRA_DEBUG_KERNEL)
                    {
                      zlog_debug ("%s: %s ACK: type=%s(%u), seq=%u, pid=%u",
                                 __FUNCTION__, nl->name,
		                 nl_msg_type_to_str (err->msg.nlmsg_type),
                                 err->msg.nlmsg_type, err->msg.nlmsg_seq,
                                 err->msg.nlmsg_pid);
                    }

                  /* return if not a multipart message, otherwise continue */
                  if (!(h->nlmsg_flags & NLM_F_MULTI))
                    return 0;
                  continue;
                }

              if (h->nlmsg_len < NLMSG_LENGTH (sizeof (struct nlmsgerr)))
                {
                  zlog (NULL, LOG_ERR, "%s error: message truncated",
                        nl->name);
                  return -1;
                }

              /* Deal with errors that occur because of races in link handling */
	      if (nl == &zns->netlink_cmd
		  && ((msg_type == RTM_DELROUTE &&
		       (-errnum == ENODEV || -errnum == ESRCH))
		      || (msg_type == RTM_NEWROUTE &&
			  (-errnum == ENETDOWN || -errnum == EEXIST))))
		{
		  if (IS_ZEBRA_DEBUG_KERNEL)
		    zlog_debug ("%s: error: %s type=%s(%u), seq=%u, pid=%u",
				nl->name, safe_strerror (-errnum),
				nl_msg_type_to_str (msg_type),
				msg_type, err->msg.nlmsg_seq, err->msg.nlmsg_pid);
		  return 0;
		}

              /* We see RTM_DELNEIGH when shutting down an interface with an IPv4
               * link-local.  The kernel should have already deleted the neighbor
               * so do not log these as an error.
               */
              if (msg_type == RTM_DELNEIGH ||
                  (nl == &zns->netlink_cmd && msg_type == RTM_NEWROUTE &&
                   (-errnum == ESRCH || -errnum == ENETUNREACH)))
		{
                  /* This is known to happen in some situations, don't log
                   * as error.
                   */
		  if (IS_ZEBRA_DEBUG_KERNEL)
	            zlog_debug ("%s error: %s, type=%s(%u), seq=%u, pid=%u",
                                nl->name, safe_strerror (-errnum),
                                nl_msg_type_to_str (msg_type),
                                msg_type, err->msg.nlmsg_seq, err->msg.nlmsg_pid);
                }
              else
	        zlog_err ("%s error: %s, type=%s(%u), seq=%u, pid=%u",
			nl->name, safe_strerror (-errnum),
                        nl_msg_type_to_str (msg_type),
			msg_type, err->msg.nlmsg_seq, err->msg.nlmsg_pid);

              return -1;
            }

          /* OK we got netlink message. */
          if (IS_ZEBRA_DEBUG_KERNEL)
            zlog_debug ("netlink_parse_info: %s type %s(%u), len=%d, seq=%u, pid=%u",
                       nl->name,
                       nl_msg_type_to_str (h->nlmsg_type), h->nlmsg_type,
                       h->nlmsg_len, h->nlmsg_seq, h->nlmsg_pid);

          /* skip unsolicited messages originating from command socket
           * linux sets the originators port-id for {NEW|DEL}ADDR messages,
           * so this has to be checked here. */
          if (nl != &zns->netlink_cmd
              && h->nlmsg_pid == zns->netlink_cmd.snl.nl_pid
              && (h->nlmsg_type != RTM_NEWADDR && h->nlmsg_type != RTM_DELADDR))
            {
              if (IS_ZEBRA_DEBUG_KERNEL)
                zlog_debug ("netlink_parse_info: %s packet comes from %s",
                            zns->netlink_cmd.name, nl->name);
              continue;
            }

          error = (*filter) (&snl, h, zns->ns_id);
          if (error < 0)
            {
              zlog (NULL, LOG_ERR, "%s filter function error", nl->name);
              ret = error;
            }
        }

      /* After error care. */
      if (msg.msg_flags & MSG_TRUNC)
        {
          zlog (NULL, LOG_ERR, "%s error: message truncated", nl->name);
          continue;
        }
      if (status)
        {
          zlog (NULL, LOG_ERR, "%s error: data remnant size %d", nl->name,
                status);
          return -1;
        }
    }
  return ret;
}

/* sendmsg() to netlink socket then recvmsg(). */
int
netlink_talk (int (*filter) (struct sockaddr_nl *, struct nlmsghdr *,
			     ns_id_t),
	      struct nlmsghdr *n, struct nlsock *nl, struct zebra_ns *zns)
{
  int status;
  struct sockaddr_nl snl;
  struct iovec iov = {
    .iov_base = (void *) n,
    .iov_len = n->nlmsg_len
  };
  struct msghdr msg = {
    .msg_name = (void *) &snl,
    .msg_namelen = sizeof snl,
    .msg_iov = &iov,
    .msg_iovlen = 1,
  };
  int save_errno;

  memset (&snl, 0, sizeof snl);
  snl.nl_family = AF_NETLINK;

  n->nlmsg_seq = ++nl->seq;

  /* Request an acknowledgement by setting NLM_F_ACK */
  n->nlmsg_flags |= NLM_F_ACK;

  if (IS_ZEBRA_DEBUG_KERNEL)
    zlog_debug ("netlink_talk: %s type %s(%u), len=%d seq=%u flags 0x%x",
               nl->name,
               lookup (nlmsg_str, n->nlmsg_type), n->nlmsg_type,
               n->nlmsg_len, n->nlmsg_seq, n->nlmsg_flags);

  /* Send message to netlink interface. */
  if (zserv_privs.change (ZPRIVS_RAISE))
    zlog (NULL, LOG_ERR, "Can't raise privileges");
  status = sendmsg (nl->sock, &msg, 0);
  save_errno = errno;
  if (zserv_privs.change (ZPRIVS_LOWER))
    zlog (NULL, LOG_ERR, "Can't lower privileges");

  if (IS_ZEBRA_DEBUG_KERNEL_MSGDUMP_SEND)
    {
      zlog_debug("%s: >> netlink message dump [sent]", __func__);
      zlog_hexdump(&msg, sizeof(msg));
    }

  if (status < 0)
    {
      zlog (NULL, LOG_ERR, "netlink_talk sendmsg() error: %s",
            safe_strerror (save_errno));
      return -1;
    }


  /* 
   * Get reply from netlink socket. 
   * The reply should either be an acknowlegement or an error.
   */
  return netlink_parse_info (filter, nl, zns, 0);
}

/* Get type specified information from netlink. */
int
netlink_request (int family, int type, struct nlsock *nl,
                 u_int32_t filter_mask)
{
  int ret;
  struct sockaddr_nl snl;
  int save_errno;
  size_t size;
  void *ptr = NULL;

  struct
  {
    struct nlmsghdr nlh;
    struct ifinfomsg ifm;
    struct rtattr ext_req;
    u_int32_t ext_filter_mask;
  } reqfilter;

  struct
  {
    struct nlmsghdr nlh;
    struct rtgenmsg g;
  } req;

  /* Check netlink socket. */
  if (nl->sock < 0)
    {
      zlog (NULL, LOG_ERR, "%s socket isn't active.", nl->name);
      return -1;
    }

  memset (&snl, 0, sizeof snl);
  snl.nl_family = AF_NETLINK;

  memset (&req, 0, sizeof req);

  if (!filter_mask)
    {
      memset (&req, 0, sizeof req);
      req.nlh.nlmsg_len = sizeof req;
      req.nlh.nlmsg_type = type;
      req.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_REQUEST;
      req.nlh.nlmsg_pid = nl->snl.nl_pid;
      req.nlh.nlmsg_seq = ++nl->seq;
      req.g.rtgen_family = family;
      size = sizeof req;
      ptr = &req;
    }
  else
    {
      memset (&reqfilter, 0, sizeof reqfilter);
      reqfilter.nlh.nlmsg_len = sizeof req;
      reqfilter.nlh.nlmsg_type = type;
      reqfilter.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_REQUEST;
      reqfilter.nlh.nlmsg_pid = nl->snl.nl_pid;
      reqfilter.nlh.nlmsg_seq = ++nl->seq;
      reqfilter.ifm.ifi_family = family;
      reqfilter.ext_req.rta_type = IFLA_EXT_MASK;
      reqfilter.ext_req.rta_len = RTA_LENGTH(sizeof(__u32));
      reqfilter.ext_filter_mask = filter_mask;
      size = sizeof reqfilter;
      ptr = &reqfilter;
    }

  /* linux appears to check capabilities on every message
   * have to raise caps for every message sent
   */
  if (zserv_privs.change (ZPRIVS_RAISE))
    {
      zlog (NULL, LOG_ERR, "Can't raise privileges");
      return -1;
    }


  ret = sendto (nl->sock, ptr, size, 0,
		(struct sockaddr *) &snl, sizeof snl);
  save_errno = errno;

  if (zserv_privs.change (ZPRIVS_LOWER))
    zlog (NULL, LOG_ERR, "Can't lower privileges");

  if (ret < 0)
    {
      zlog (NULL, LOG_ERR, "%s sendto failed: %s", nl->name,
            safe_strerror (save_errno));
      return -1;
    }

  return 0;
}

/* Exported interface function.  This function simply calls
   netlink_socket (). */
void
kernel_init (struct zebra_ns *zns)
{
  unsigned long groups;

  groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV4_IFADDR;
  groups |= RTMGRP_IPV6_ROUTE | RTMGRP_IPV6_IFADDR;
  groups |= RTMGRP_NEIGH;

  netlink_socket (&zns->netlink, groups, zns->ns_id);
  netlink_socket (&zns->netlink_cmd, 0, zns->ns_id);

  /* Register kernel socket. */
  if (zns->netlink.sock > 0)
    {
      /* Only want non-blocking on the netlink event socket */
      if (fcntl (zns->netlink.sock, F_SETFL, O_NONBLOCK) < 0)
        zlog_err ("Can't set %s socket flags: %s", zns->netlink.name,
                  safe_strerror (errno));

      /* Set receive buffer size if it's set from command line */
      if (nl_rcvbufsize)
        netlink_recvbuf (&zns->netlink, nl_rcvbufsize);

      netlink_install_filter (zns->netlink.sock, zns->netlink_cmd.snl.nl_pid);
      zns->t_netlink = thread_add_read (zebrad.master, kernel_read, zns,
                                         zns->netlink.sock);
    }
}

void
kernel_terminate (struct zebra_ns *zns)
{
  THREAD_READ_OFF (zns->t_netlink);

  if (zns->netlink.sock >= 0)
    {
      close (zns->netlink.sock);
      zns->netlink.sock = -1;
    }

  if (zns->netlink_cmd.sock >= 0)
    {
      close (zns->netlink_cmd.sock);
      zns->netlink_cmd.sock = -1;
    }
}
