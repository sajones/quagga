/*
  PIM for Quagga
  Copyright (C) 2008  Everton da Silva Marques

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING; if not, write to the
  Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA 02110-1301 USA
  
*/

#include <zebra.h>

#include "lib/json.h"
#include "command.h"
#include "if.h"
#include "prefix.h"
#include "zclient.h"
#include "plist.h"

#include "pimd.h"
#include "pim_mroute.h"
#include "pim_cmd.h"
#include "pim_iface.h"
#include "pim_vty.h"
#include "pim_mroute.h"
#include "pim_str.h"
#include "pim_igmp.h"
#include "pim_igmpv3.h"
#include "pim_sock.h"
#include "pim_time.h"
#include "pim_util.h"
#include "pim_oil.h"
#include "pim_neighbor.h"
#include "pim_pim.h"
#include "pim_ifchannel.h"
#include "pim_hello.h"
#include "pim_msg.h"
#include "pim_upstream.h"
#include "pim_rpf.h"
#include "pim_macro.h"
#include "pim_ssmpingd.h"
#include "pim_zebra.h"
#include "pim_static.h"
#include "pim_rp.h"
#include "pim_zlookup.h"

static struct cmd_node pim_global_node = {
  PIM_NODE,
  "",
  1 /* vtysh ? yes */
};

static struct cmd_node interface_node = {
  INTERFACE_NODE,
  "%s(config-if)# ",
  1 /* vtysh ? yes */
};

static void pim_if_membership_clear(struct interface *ifp)
{
  struct pim_interface *pim_ifp;

  pim_ifp = ifp->info;
  zassert(pim_ifp);

  if (PIM_IF_TEST_PIM(pim_ifp->options) &&
      PIM_IF_TEST_IGMP(pim_ifp->options)) {
    return;
  }

  pim_ifchannel_membership_clear(ifp);
}

/*
  When PIM is disabled on interface, IGMPv3 local membership
  information is not injected into PIM interface state.

  The function pim_if_membership_refresh() fetches all IGMPv3 local
  membership information into PIM. It is intented to be called
  whenever PIM is enabled on the interface in order to collect missed
  local membership information.
 */
static void pim_if_membership_refresh(struct interface *ifp)
{
  struct pim_interface *pim_ifp;
  struct listnode      *sock_node;
  struct igmp_sock     *igmp;

  pim_ifp = ifp->info;
  zassert(pim_ifp);

  if (!PIM_IF_TEST_PIM(pim_ifp->options))
    return;
  if (!PIM_IF_TEST_IGMP(pim_ifp->options))
    return;

  /*
    First clear off membership from all PIM (S,G) entries on the
    interface
  */

  pim_ifchannel_membership_clear(ifp);

  /*
    Then restore PIM (S,G) membership from all IGMPv3 (S,G) entries on
    the interface
  */

  /* scan igmp sockets */
  for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
    struct listnode   *grpnode;
    struct igmp_group *grp;
    
    /* scan igmp groups */
    for (ALL_LIST_ELEMENTS_RO(igmp->igmp_group_list, grpnode, grp)) {
      struct listnode    *srcnode;
      struct igmp_source *src;
      
      /* scan group sources */
      for (ALL_LIST_ELEMENTS_RO(grp->group_source_list, srcnode, src)) {

	if (IGMP_SOURCE_TEST_FORWARDING(src->source_flags)) {
	  struct prefix_sg sg;

	  memset (&sg, 0, sizeof (struct prefix_sg));
	  sg.src = src->source_addr;
	  sg.grp = grp->group_addr;
	  pim_ifchannel_local_membership_add(ifp, &sg);
	}
	
      } /* scan group sources */
    } /* scan igmp groups */
  } /* scan igmp sockets */

  /*
    Finally delete every PIM (S,G) entry lacking all state info
   */

  pim_ifchannel_delete_on_noinfo(ifp);

}

static void pim_show_assert(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  time_t            now;
  
  now = pim_time_monotonic_sec();

  vty_out(vty,
	  "Interface Address         Source          Group           State  Winner          Uptime   Timer%s",
	  VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO(vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *ch_node;
    struct pim_ifchannel *ch;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, ch_node, ch)) {
      char ch_src_str[100];
      char ch_grp_str[100];
      char winner_str[100];
      char uptime[10];
      char timer[10];

      pim_inet4_dump("<ch_src?>", ch->sg.src,
		     ch_src_str, sizeof(ch_src_str));
      pim_inet4_dump("<ch_grp?>", ch->sg.grp,
		     ch_grp_str, sizeof(ch_grp_str));
      pim_inet4_dump("<assrt_win?>", ch->ifassert_winner,
		     winner_str, sizeof(winner_str));

      pim_time_uptime(uptime, sizeof(uptime), now - ch->ifassert_creation);
      pim_time_timer_to_mmss(timer, sizeof(timer),
			     ch->t_ifassert_timer);

      vty_out(vty, "%-9s %-15s %-15s %-15s %-6s %-15s %-8s %-5s%s",
	      ifp->name,
	      inet_ntoa(ifaddr),
	      ch_src_str,
	      ch_grp_str,
	      pim_ifchannel_ifassert_name(ch->ifassert_state),
	      winner_str,
	      uptime,
	      timer,
	      VTY_NEWLINE);
    } /* scan interface channels */
  } /* scan interfaces */
}

static void pim_show_assert_internal(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;

  vty_out(vty,
	  "CA:   CouldAssert%s"
	  "ECA:  Evaluate CouldAssert%s"
	  "ATD:  AssertTrackingDesired%s"
	  "eATD: Evaluate AssertTrackingDesired%s%s",
	  VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE);

  vty_out(vty,
	  "Interface Address         Source          Group           CA  eCA ATD eATD%s",
	  VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO(vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *ch_node;
    struct pim_ifchannel *ch;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, ch_node, ch)) {
      char ch_src_str[100];
      char ch_grp_str[100];

      pim_inet4_dump("<ch_src?>", ch->sg.src,
		     ch_src_str, sizeof(ch_src_str));
      pim_inet4_dump("<ch_grp?>", ch->sg.grp,
		     ch_grp_str, sizeof(ch_grp_str));
      vty_out(vty, "%-9s %-15s %-15s %-15s %-3s %-3s %-3s %-4s%s",
	      ifp->name,
	      inet_ntoa(ifaddr),
	      ch_src_str,
	      ch_grp_str,
	      PIM_IF_FLAG_TEST_COULD_ASSERT(ch->flags) ? "yes" : "no",
	      pim_macro_ch_could_assert_eval(ch) ? "yes" : "no",
	      PIM_IF_FLAG_TEST_ASSERT_TRACKING_DESIRED(ch->flags) ? "yes" : "no",
	      pim_macro_assert_tracking_desired_eval(ch) ? "yes" : "no",
	      VTY_NEWLINE);
    } /* scan interface channels */
  } /* scan interfaces */
}

static void pim_show_assert_metric(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  
  vty_out(vty,
	  "Interface Address         Source          Group           RPT Pref Metric Address        %s",
	  VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *ch_node;
    struct pim_ifchannel *ch;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, ch_node, ch)) {
      char ch_src_str[100];
      char ch_grp_str[100];
      char addr_str[100];
      struct pim_assert_metric am;

      am = pim_macro_spt_assert_metric(&ch->upstream->rpf, pim_ifp->primary_address);

      pim_inet4_dump("<ch_src?>", ch->sg.src,
		     ch_src_str, sizeof(ch_src_str));
      pim_inet4_dump("<ch_grp?>", ch->sg.grp,
		     ch_grp_str, sizeof(ch_grp_str));
      pim_inet4_dump("<addr?>", am.ip_address,
		     addr_str, sizeof(addr_str));

      vty_out(vty, "%-9s %-15s %-15s %-15s %-3s %4u %6u %-15s%s",
	      ifp->name,
	      inet_ntoa(ifaddr),
	      ch_src_str,
	      ch_grp_str,
	      am.rpt_bit_flag ? "yes" : "no",
	      am.metric_preference,
	      am.route_metric,
	      addr_str,
	      VTY_NEWLINE);
    } /* scan interface channels */
  } /* scan interfaces */
}

static void pim_show_assert_winner_metric(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  
  vty_out(vty,
	  "Interface Address         Source          Group           RPT Pref Metric Address        %s",
	  VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *ch_node;
    struct pim_ifchannel *ch;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, ch_node, ch)) {
      char ch_src_str[100];
      char ch_grp_str[100];
      char addr_str[100];
      struct pim_assert_metric *am;
      char pref_str[5];
      char metr_str[7];

      am = &ch->ifassert_winner_metric;

      pim_inet4_dump("<ch_src?>", ch->sg.src,
		     ch_src_str, sizeof(ch_src_str));
      pim_inet4_dump("<ch_grp?>", ch->sg.grp,
		     ch_grp_str, sizeof(ch_grp_str));
      pim_inet4_dump("<addr?>", am->ip_address,
		     addr_str, sizeof(addr_str));

      if (am->metric_preference == PIM_ASSERT_METRIC_PREFERENCE_MAX)
	snprintf(pref_str, sizeof(pref_str), "INFI");
      else
	snprintf(pref_str, sizeof(pref_str), "%4u", am->metric_preference);

      if (am->route_metric == PIM_ASSERT_ROUTE_METRIC_MAX)
	snprintf(metr_str, sizeof(metr_str), "INFI");
      else
	snprintf(metr_str, sizeof(metr_str), "%6u", am->route_metric);

      vty_out(vty, "%-9s %-15s %-15s %-15s %-3s %-4s %-6s %-15s%s",
	      ifp->name,
	      inet_ntoa(ifaddr),
	      ch_src_str,
	      ch_grp_str,
	      am->rpt_bit_flag ? "yes" : "no",
	      pref_str,
	      metr_str,
	      addr_str,
	      VTY_NEWLINE);
    } /* scan interface channels */
  } /* scan interfaces */
}

static void json_object_pim_ifp_add(struct json_object *json, struct interface *ifp)
{
  struct pim_interface *pim_ifp;

  pim_ifp = ifp->info;
  json_object_string_add(json, "name", ifp->name);
  json_object_string_add(json, "state", if_is_up(ifp) ? "up" : "down");
  json_object_string_add(json, "address", inet_ntoa(pim_ifp->primary_address));
  json_object_int_add(json, "index", ifp->ifindex);

  if (if_is_multicast(ifp))
    json_object_boolean_true_add(json, "flagMulticast");

  if (if_is_broadcast(ifp))
    json_object_boolean_true_add(json, "flagBroadcast");

  if (ifp->flags & IFF_ALLMULTI)
    json_object_boolean_true_add(json, "flagAllMulticast");

  if (ifp->flags & IFF_PROMISC)
    json_object_boolean_true_add(json, "flagPromiscuous");

  if (PIM_IF_IS_DELETED(ifp))
    json_object_boolean_true_add(json, "flagDeleted");

  if (pim_if_lan_delay_enabled(ifp))
    json_object_boolean_true_add(json, "lanDelayEnabled");
}

static void pim_show_membership(struct vty *vty, u_char uj)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  json_object *json = NULL;
  json_object *json_iface = NULL;
  json_object *json_row = NULL;

  if (uj)
    json = json_object_new_object();
  else
    vty_out(vty,
            "Interface Address         Source          Group           Membership%s",
            VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *ch_node;
    struct pim_ifchannel *ch;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, ch_node, ch)) {
      char ch_src_str[100];
      char ch_grp_str[100];

      pim_inet4_dump("<ch_src?>", ch->sg.src,
		     ch_src_str, sizeof(ch_src_str));
      pim_inet4_dump("<ch_grp?>", ch->sg.grp,
		     ch_grp_str, sizeof(ch_grp_str));

      if (uj) {
        json_object_object_get_ex(json, ifp->name, &json_iface);

        if (!json_iface) {
          json_iface = json_object_new_object();
          json_object_pim_ifp_add(json_iface, ifp);
          json_object_object_add(json, ifp->name, json_iface);
        }

        json_row = json_object_new_object();
        json_object_string_add(json_row, "source", ch_src_str);
        json_object_string_add(json_row, "group", ch_grp_str);
        json_object_string_add(json_row, "localMembership",
                               ch->local_ifmembership == PIM_IFMEMBERSHIP_NOINFO ?  "NOINFO" : "INCLUDE");
        json_object_object_add(json_iface, ch_grp_str, json_row);

      } else {
        vty_out(vty, "%-9s %-15s %-15s %-15s %-10s%s",
                ifp->name,
                inet_ntoa(ifaddr),
                ch_src_str,
                ch_grp_str,
                ch->local_ifmembership == PIM_IFMEMBERSHIP_NOINFO ?
                "NOINFO" : "INCLUDE",
                VTY_NEWLINE);
      }
    } /* scan interface channels */
  } /* scan interfaces */

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void pim_print_ifp_flags(struct vty *vty, struct interface *ifp, int mloop)
{
  vty_out(vty, "Flags%s", VTY_NEWLINE);
  vty_out(vty, "-----%s", VTY_NEWLINE);
  vty_out(vty, "All Multicast   : %s%s", (ifp->flags & IFF_ALLMULTI) ? "yes" : "no", VTY_NEWLINE);
  vty_out(vty, "Broadcast       : %s%s", if_is_broadcast(ifp)? "yes" : "no", VTY_NEWLINE);
  vty_out(vty, "Deleted         : %s%s", PIM_IF_IS_DELETED(ifp) ? "yes" : "no", VTY_NEWLINE);
  vty_out(vty, "Interface Index : %d%s", ifp->ifindex, VTY_NEWLINE);
  vty_out(vty, "Multicast       : %s%s", if_is_multicast(ifp) ? "yes" : "no", VTY_NEWLINE);
  vty_out(vty, "Multicast Loop  : %d%s", mloop, VTY_NEWLINE);
  vty_out(vty, "Promiscuous     : %s%s", (ifp->flags & IFF_PROMISC) ? "yes" : "no", VTY_NEWLINE);
  vty_out(vty, "%s", VTY_NEWLINE);
  vty_out(vty, "%s", VTY_NEWLINE);
}

static void igmp_show_interfaces(struct vty *vty, u_char uj)
{
  struct listnode  *node;
  struct interface *ifp;
  time_t            now;
  json_object *json = NULL;
  json_object *json_row = NULL;

  now = pim_time_monotonic_sec();

  if (uj)
    json = json_object_new_object();
  else
    vty_out(vty,
            "Interface  State          Address  Querier  Query Timer    Uptime%s",
            VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    struct pim_interface *pim_ifp;
    struct listnode *sock_node;
    struct igmp_sock *igmp;

    pim_ifp = ifp->info;

    if (!pim_ifp)
      continue;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
      char uptime[10];
      char query_hhmmss[10];

      pim_time_uptime(uptime, sizeof(uptime), now - igmp->sock_creation);
      pim_time_timer_to_hhmmss(query_hhmmss, sizeof(query_hhmmss), igmp->t_igmp_query_timer);

      if (uj) {
        json_row = json_object_new_object();
        json_object_pim_ifp_add(json_row, ifp);
        json_object_string_add(json_row, "upTime", uptime);

        if (igmp->t_igmp_query_timer) {
          json_object_boolean_true_add(json_row, "querier");
          json_object_string_add(json_row, "queryTimer", query_hhmmss);
        }

        json_object_object_add(json, ifp->name, json_row);

      } else {
        vty_out(vty, "%-9s  %5s  %15s  %7s  %11s  %8s%s",
                ifp->name,
                if_is_up(ifp) ? "up" : "down",
                inet_ntoa(igmp->ifaddr),
	        igmp->t_igmp_query_timer ? "local" : "other",
	        query_hhmmss,
                uptime,
                VTY_NEWLINE);
      }
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void igmp_show_interfaces_single(struct vty *vty, const char *ifname, u_char uj)
{
  struct igmp_sock *igmp;
  struct interface *ifp;
  struct listnode  *node;
  struct listnode *sock_node;
  struct pim_interface *pim_ifp;
  char uptime[10];
  char query_hhmmss[10];
  char other_hhmmss[10];
  int found_ifname = 0;
  int sqi;
  int mloop;
  long gmi_msec;  /* Group Membership Interval */
  long lmqt_msec;
  long ohpi_msec;
  long oqpi_msec; /* Other Querier Present Interval */
  long qri_msec;
  time_t now;

  json_object *json = NULL;
  json_object *json_row = NULL;

  now = pim_time_monotonic_sec();

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    pim_ifp = ifp->info;

    if (!pim_ifp)
      continue;

    if (strcmp(ifname, ifp->name))
      continue;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
      found_ifname = 1;
      pim_time_uptime(uptime, sizeof(uptime), now - igmp->sock_creation);
      pim_time_timer_to_hhmmss(query_hhmmss, sizeof(query_hhmmss), igmp->t_igmp_query_timer);
      pim_time_timer_to_hhmmss(other_hhmmss, sizeof(other_hhmmss), igmp->t_other_querier_timer);

      gmi_msec = PIM_IGMP_GMI_MSEC(igmp->querier_robustness_variable,
                                   igmp->querier_query_interval,
                                   pim_ifp->igmp_query_max_response_time_dsec);

      sqi = PIM_IGMP_SQI(pim_ifp->igmp_default_query_interval);

      oqpi_msec = PIM_IGMP_OQPI_MSEC(igmp->querier_robustness_variable,
                                     igmp->querier_query_interval,
                                     pim_ifp->igmp_query_max_response_time_dsec);

      lmqt_msec = PIM_IGMP_LMQT_MSEC(pim_ifp->igmp_query_max_response_time_dsec,
                                     igmp->querier_robustness_variable);

      ohpi_msec = PIM_IGMP_OHPI_DSEC(igmp->querier_robustness_variable,
                                     igmp->querier_query_interval,
                                     pim_ifp->igmp_query_max_response_time_dsec) * 100;

      qri_msec = pim_ifp->igmp_query_max_response_time_dsec * 100;
      mloop = pim_socket_mcastloop_get(pim_ifp->pim_sock_fd);

      if (uj) {
        json = json_object_new_object();
        json_row = json_object_new_object();
        json_object_pim_ifp_add(json_row, ifp);

        json_object_string_add(json_row, "upTime", uptime);
        json_object_string_add(json_row, "querier", igmp->t_igmp_query_timer ? "local" : "other");
        json_object_int_add(json_row, "queryStartCount", igmp->startup_query_count);
        json_object_string_add(json_row, "queryQueryTimer", query_hhmmss);
        json_object_string_add(json_row, "queryOtherTimer", other_hhmmss);
        json_object_int_add(json_row, "timerGroupMembershipIntervalMsec", gmi_msec);
        json_object_int_add(json_row, "timerLastMemberQueryMsec", lmqt_msec);
        json_object_int_add(json_row, "timerOlderHostPresentIntervalMsec", ohpi_msec);
        json_object_int_add(json_row, "timerOtherQuerierPresentIntervalMsec", oqpi_msec);
        json_object_int_add(json_row, "timerQueryInterval", igmp->querier_query_interval);
        json_object_int_add(json_row, "timerQueryResponseIntervalMsec", qri_msec);
        json_object_int_add(json_row, "timerRobustnessVariable", igmp->querier_robustness_variable);
        json_object_int_add(json_row, "timerStartupQueryInterval", sqi);

        json_object_object_add(json, ifp->name, json_row);
        vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
        json_object_free(json);

      } else {
        vty_out(vty, "Interface : %s%s", ifp->name, VTY_NEWLINE);
        vty_out(vty, "State     : %s%s", if_is_up(ifp) ? "up" : "down", VTY_NEWLINE);
        vty_out(vty, "Address   : %s%s", inet_ntoa(pim_ifp->primary_address), VTY_NEWLINE);
        vty_out(vty, "Uptime    : %s%s", uptime, VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);

        vty_out(vty, "Querier%s", VTY_NEWLINE);
        vty_out(vty, "-------%s", VTY_NEWLINE);
        vty_out(vty, "Querier     : %s%s", igmp->t_igmp_query_timer ? "local" : "other", VTY_NEWLINE);
        vty_out(vty, "Start Count : %d%s", igmp->startup_query_count, VTY_NEWLINE);
        vty_out(vty, "Query Timer : %s%s", query_hhmmss, VTY_NEWLINE);
        vty_out(vty, "Other Timer : %s%s", other_hhmmss, VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);

        vty_out(vty, "Timers%s", VTY_NEWLINE);
        vty_out(vty, "------%s", VTY_NEWLINE);
        vty_out(vty, "Group Membership Interval      : %lis%s", gmi_msec/1000, VTY_NEWLINE);
        vty_out(vty, "Last Member Query Time         : %lis%s", lmqt_msec/1000, VTY_NEWLINE);
        vty_out(vty, "Older Host Present Interval    : %lis%s", ohpi_msec/1000, VTY_NEWLINE);
        vty_out(vty, "Other Querier Present Interval : %lis%s", oqpi_msec/1000, VTY_NEWLINE);
        vty_out(vty, "Query Interval                 : %ds%s", igmp->querier_query_interval, VTY_NEWLINE);
        vty_out(vty, "Query Response Interval        : %lis%s", qri_msec/1000, VTY_NEWLINE);
        vty_out(vty, "Robustness Variable            : %d%s", igmp->querier_robustness_variable, VTY_NEWLINE);
        vty_out(vty, "Startup Query Interval         : %ds%s", sqi, VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);

        pim_print_ifp_flags(vty, ifp, mloop);
      }
    }
  }

  if (!found_ifname)
    {
      if (uj)
        vty_out (vty, "{}%s", VTY_NEWLINE);
      else
        vty_out (vty, "%% No such interface%s", VTY_NEWLINE);
    }
}

static void igmp_show_interface_join(struct vty *vty)
{
  struct listnode  *node;
  struct interface *ifp;
  time_t            now;
  
  now = pim_time_monotonic_sec();

  vty_out(vty,
	  "Interface Address         Source          Group           Socket Uptime  %s",
	  VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    struct pim_interface *pim_ifp;
    struct listnode *join_node;
    struct igmp_join *ij;
    struct in_addr pri_addr;
    char pri_addr_str[100];

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    if (!pim_ifp->igmp_join_list)
      continue;

    pri_addr = pim_find_primary_addr(ifp);
    pim_inet4_dump("<pri?>", pri_addr, pri_addr_str, sizeof(pri_addr_str));

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_join_list, join_node, ij)) {
      char group_str[100];
      char source_str[100];
      char uptime[10];

      pim_time_uptime(uptime, sizeof(uptime), now - ij->sock_creation);
      pim_inet4_dump("<grp?>", ij->group_addr, group_str, sizeof(group_str));
      pim_inet4_dump("<src?>", ij->source_addr, source_str, sizeof(source_str));
      
      vty_out(vty, "%-9s %-15s %-15s %-15s %6d %8s%s",
	      ifp->name,
	      pri_addr_str,
	      source_str,
	      group_str,
	      ij->sock_fd,
	      uptime,
	      VTY_NEWLINE);
    } /* for (pim_ifp->igmp_join_list) */

  } /* for (iflist) */

}

static void pim_show_interfaces_single(struct vty *vty, const char *ifname, u_char uj)
{
  struct in_addr ifaddr;
  struct interface *ifp;
  struct listnode *neighnode;
  struct listnode*node;
  struct listnode *upnode;
  struct pim_interface *pim_ifp;
  struct pim_neighbor *neigh;
  struct pim_upstream *up;
  time_t now;
  char dr_str[100];
  char dr_uptime[10];
  char expire[10];
  char grp_str[100];
  char hello_period[10];
  char hello_timer[10];
  char neigh_src_str[100];
  char src_str[100];
  char stat_uptime[10];
  char uptime[10];
  int mloop;
  int found_ifname = 0;
  int print_header;
  json_object *json = NULL;
  json_object *json_row = NULL;
  json_object *json_pim_neighbor = NULL;
  json_object *json_pim_neighbors = NULL;
  json_object *json_group = NULL;
  json_object *json_group_source = NULL;
  json_object *json_fhr_sources = NULL;

  now = pim_time_monotonic_sec();

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    pim_ifp = ifp->info;

    if (!pim_ifp)
      continue;

    if (pim_ifp->pim_sock_fd < 0)
      continue;

    if (strcmp(ifname, ifp->name))
      continue;

    found_ifname = 1;
    ifaddr = pim_ifp->primary_address;
    pim_inet4_dump("<dr?>", pim_ifp->pim_dr_addr, dr_str, sizeof(dr_str));
    pim_time_uptime_begin(dr_uptime, sizeof(dr_uptime), now, pim_ifp->pim_dr_election_last);
    pim_time_timer_to_hhmmss(hello_timer, sizeof(hello_timer), pim_ifp->t_pim_hello_timer);
    pim_time_mmss(hello_period, sizeof(hello_period), pim_ifp->pim_hello_period);
    pim_time_uptime(stat_uptime, sizeof(stat_uptime), now - pim_ifp->pim_ifstat_start);
    mloop = pim_socket_mcastloop_get(pim_ifp->pim_sock_fd);

    if (uj) {
      json = json_object_new_object();
      json_row = json_object_new_object();
      json_object_pim_ifp_add(json_row, ifp);

      // PIM neighbors
      if (pim_ifp->pim_neighbor_list->count) {
        json_pim_neighbors = json_object_new_object();

        for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode, neigh)) {
          json_pim_neighbor = json_object_new_object();
          pim_inet4_dump("<src?>", neigh->source_addr, neigh_src_str, sizeof(neigh_src_str));
          pim_time_uptime(uptime, sizeof(uptime), now - neigh->creation);
          pim_time_timer_to_hhmmss(expire, sizeof(expire), neigh->t_expire_timer);

          json_object_string_add(json_pim_neighbor, "address", neigh_src_str);
          json_object_string_add(json_pim_neighbor, "upTime", uptime);
          json_object_string_add(json_pim_neighbor, "holdtime", expire);

          json_object_object_add(json_pim_neighbors, neigh_src_str, json_pim_neighbor);
        }

        json_object_object_add(json_row, "neighbors", json_pim_neighbors);
      }

      json_object_string_add(json_row, "drAddress", dr_str);
      json_object_int_add(json_row, "drPriority", pim_ifp->pim_dr_priority);
      json_object_string_add(json_row, "drUptime", dr_uptime);
      json_object_int_add(json_row, "drElections", pim_ifp->pim_dr_election_count);
      json_object_int_add(json_row, "drChanges", pim_ifp->pim_dr_election_changes);

      // FHR
      for (ALL_LIST_ELEMENTS_RO(qpim_upstream_list, upnode, up)) {
        if (strcmp(ifp->name, up->rpf.source_nexthop.interface->name) == 0) {
          if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR) {
            if (!json_fhr_sources) {
              json_fhr_sources = json_object_new_object();
            }

            pim_inet4_dump("<src?>", up->sg.src, src_str, sizeof(src_str));
            pim_inet4_dump("<grp?>", up->sg.grp, grp_str, sizeof(grp_str));
            pim_time_uptime(uptime, sizeof(uptime), now - up->state_transition);

            /* Does this group live in json_fhr_sources?  If not create it. */
            json_object_object_get_ex(json_fhr_sources, grp_str, &json_group);

            if (!json_group) {
                json_group = json_object_new_object();
                json_object_object_add(json_fhr_sources, grp_str, json_group);
            }

            json_group_source = json_object_new_object();
            json_object_string_add(json_group_source, "source", src_str);
            json_object_string_add(json_group_source, "group", grp_str);
            json_object_string_add(json_group_source, "upTime", uptime);
            json_object_object_add(json_group, src_str, json_group_source);
          }
        }
      }

      if (json_fhr_sources) {
        json_object_object_add(json_row, "firstHopRouter", json_fhr_sources);
      }

      json_object_int_add(json_row, "helloPeriod", pim_ifp->pim_hello_period);
      json_object_string_add(json_row, "helloTimer", hello_timer);
      json_object_string_add(json_row, "helloStatStart", stat_uptime);
      json_object_int_add(json_row, "helloReceived", pim_ifp->pim_ifstat_hello_recv);
      json_object_int_add(json_row, "helloReceivedFailed", pim_ifp->pim_ifstat_hello_recvfail);
      json_object_int_add(json_row, "helloSend", pim_ifp->pim_ifstat_hello_sent);
      json_object_int_add(json_row, "hellosendFailed", pim_ifp->pim_ifstat_hello_sendfail);
      json_object_int_add(json_row, "helloGenerationId", pim_ifp->pim_generation_id);
      json_object_int_add(json_row, "flagMulticastLoop", mloop);

      json_object_int_add(json_row, "effectivePropagationDelay", pim_if_effective_propagation_delay_msec(ifp));
      json_object_int_add(json_row, "effectiveOverrideInterval", pim_if_effective_override_interval_msec(ifp));
      json_object_int_add(json_row, "joinPruneOverrideInterval", pim_if_jp_override_interval_msec(ifp));

      json_object_int_add(json_row, "propagationDelay", pim_ifp->pim_propagation_delay_msec);
      json_object_int_add(json_row, "propagationDelayHighest", pim_ifp->pim_neighbors_highest_propagation_delay_msec);
      json_object_int_add(json_row, "overrideInterval", pim_ifp->pim_override_interval_msec);
      json_object_int_add(json_row, "overrideIntervalHighest", pim_ifp->pim_neighbors_highest_override_interval_msec);

      json_object_object_add(json, ifp->name, json_row);
      vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
      json_object_free(json);

    } else {
      vty_out(vty, "Interface : %s%s", ifp->name, VTY_NEWLINE);
      vty_out(vty, "State     : %s%s", if_is_up(ifp) ? "up" : "down", VTY_NEWLINE);
      vty_out(vty, "Address   : %s%s", inet_ntoa(ifaddr), VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);

      // PIM neighbors
      print_header = 1;

      for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode, neigh)) {

        if (print_header) {
          vty_out(vty, "PIM Neighbors%s", VTY_NEWLINE);
          vty_out(vty, "-------------%s", VTY_NEWLINE);
          print_header = 0;
        }

        pim_inet4_dump("<src?>", neigh->source_addr, neigh_src_str, sizeof(neigh_src_str));
        pim_time_uptime(uptime, sizeof(uptime), now - neigh->creation);
        pim_time_timer_to_hhmmss(expire, sizeof(expire), neigh->t_expire_timer);
        vty_out(vty, "%-15s : up for %s, holdtime expires in %s%s", neigh_src_str, uptime, expire, VTY_NEWLINE);
      }

      if (!print_header) {
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
      }

      vty_out(vty, "Designated Router%s", VTY_NEWLINE);
      vty_out(vty, "-----------------%s", VTY_NEWLINE);
      vty_out(vty, "Address   : %s%s", dr_str, VTY_NEWLINE);
      vty_out(vty, "Priority  : %d%s", pim_ifp->pim_dr_priority, VTY_NEWLINE);
      vty_out(vty, "Uptime    : %s%s", dr_uptime, VTY_NEWLINE);
      vty_out(vty, "Elections : %d%s", pim_ifp->pim_dr_election_count, VTY_NEWLINE);
      vty_out(vty, "Changes   : %d%s", pim_ifp->pim_dr_election_changes, VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);

      // FHR
      print_header = 1;
      for (ALL_LIST_ELEMENTS_RO(qpim_upstream_list, upnode, up)) {
        if (strcmp(ifp->name, up->rpf.source_nexthop.interface->name) == 0) {
          if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR) {

            if (print_header) {
              vty_out(vty, "FHR - First Hop Router%s", VTY_NEWLINE);
              vty_out(vty, "----------------------%s", VTY_NEWLINE);
              print_header = 0;
            }

            pim_inet4_dump("<src?>", up->sg.src, src_str, sizeof(src_str));
            pim_inet4_dump("<grp?>", up->sg.grp, grp_str, sizeof(grp_str));
            pim_time_uptime(uptime, sizeof(uptime), now - up->state_transition);
            vty_out(vty, "%s : %s is a source, uptime is %s%s", grp_str, src_str, uptime, VTY_NEWLINE);
          }
        }
      }

      if (!print_header) {
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
      }

      vty_out(vty, "Hellos%s", VTY_NEWLINE);
      vty_out(vty, "------%s", VTY_NEWLINE);
      vty_out(vty, "Period         : %d%s", pim_ifp->pim_hello_period, VTY_NEWLINE);
      vty_out(vty, "Timer          : %s%s", hello_timer, VTY_NEWLINE);
      vty_out(vty, "StatStart      : %s%s", stat_uptime, VTY_NEWLINE);
      vty_out(vty, "Receive        : %d%s", pim_ifp->pim_ifstat_hello_recv, VTY_NEWLINE);
      vty_out(vty, "Receive Failed : %d%s", pim_ifp->pim_ifstat_hello_recvfail, VTY_NEWLINE);
      vty_out(vty, "Send           : %d%s", pim_ifp->pim_ifstat_hello_sent, VTY_NEWLINE);
      vty_out(vty, "Send Failed    : %d%s", pim_ifp->pim_ifstat_hello_sendfail, VTY_NEWLINE);
      vty_out(vty, "Generation ID  : %08x%s", pim_ifp->pim_generation_id, VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);

      pim_print_ifp_flags(vty, ifp, mloop);

      vty_out(vty, "Join Prune Interval%s", VTY_NEWLINE);
      vty_out(vty, "-------------------%s", VTY_NEWLINE);
      vty_out(vty, "LAN Delay                    : %s%s", pim_if_lan_delay_enabled(ifp) ? "yes" : "no", VTY_NEWLINE);
      vty_out(vty, "Effective Propagation Delay  : %d msec%s", pim_if_effective_propagation_delay_msec(ifp), VTY_NEWLINE);
      vty_out(vty, "Effective Override Interval  : %d msec%s", pim_if_effective_override_interval_msec(ifp), VTY_NEWLINE);
      vty_out(vty, "Join Prune Override Interval : %d msec%s", pim_if_jp_override_interval_msec(ifp), VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);

      vty_out(vty, "LAN Prune Delay%s", VTY_NEWLINE);
      vty_out(vty, "---------------%s", VTY_NEWLINE);
      vty_out(vty, "Propagation Delay           : %d msec%s", pim_ifp->pim_propagation_delay_msec, VTY_NEWLINE);
      vty_out(vty, "Propagation Delay (Highest) : %d msec%s", pim_ifp->pim_neighbors_highest_propagation_delay_msec, VTY_NEWLINE);
      vty_out(vty, "Override Interval           : %d msec%s", pim_ifp->pim_override_interval_msec, VTY_NEWLINE);
      vty_out(vty, "Override Interval (Highest) : %d msec%s", pim_ifp->pim_neighbors_highest_override_interval_msec, VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);
      vty_out(vty, "%s", VTY_NEWLINE);
    }
  }

  if (!found_ifname)
    {
      if (uj)
        vty_out (vty, "{}%s", VTY_NEWLINE);
      else
        vty_out (vty, "%% No such interface%s", VTY_NEWLINE);
    }
}


static void pim_show_interfaces(struct vty *vty, u_char uj)
{
  struct in_addr ifaddr;
  struct interface *ifp;
  struct listnode  *node;
  struct listnode *upnode;
  struct pim_interface *pim_ifp;
  struct pim_upstream *up;
  int fhr = 0;
  int pim_dr_local = 0;
  int pim_nbrs = 0;
  json_object *json = NULL;
  json_object *json_row = NULL;
  
  if (uj) {
    json = json_object_new_object();
  } else {
    vty_out(vty, "Interface  State          Address  PIM Nbrs           PIM DR  FHR%s", VTY_NEWLINE);
  }

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    if (pim_ifp->pim_sock_fd < 0)
      continue;

    ifaddr = pim_ifp->primary_address;
    pim_nbrs = pim_ifp->pim_neighbor_list->count;
    pim_dr_local = 0;
    fhr = 0;

    if (pim_ifp->pim_dr_addr.s_addr == pim_ifp->primary_address.s_addr)
      pim_dr_local = 1;

    for (ALL_LIST_ELEMENTS_RO(qpim_upstream_list, upnode, up))
      if (strcmp(ifp->name, up->rpf.source_nexthop.interface->name) == 0)
        if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR)
          fhr++;

    if (uj) {
      json_row = json_object_new_object();
      json_object_pim_ifp_add(json_row, ifp);
      json_object_int_add(json_row, "pimNeighbors", pim_nbrs);
      json_object_int_add(json_row, "firstHopRouter", fhr);
      json_object_string_add(json_row, "pimDesignatedRouter", inet_ntoa(pim_ifp->pim_dr_addr));
      json_object_object_add(json, ifp->name, json_row);

    } else {
      vty_out(vty, "%-9s  %5s  %15s  %8d  %15s  %3d%s",
              ifp->name,
              if_is_up(ifp) ? "up" : "down",
              inet_ntoa(ifaddr),
              pim_nbrs,
              pim_dr_local ? "local" : inet_ntoa(pim_ifp->pim_dr_addr),
              fhr,
              VTY_NEWLINE);
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void pim_show_join(struct vty *vty, u_char uj)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  time_t            now;
  json_object *json = NULL;
  json_object *json_iface = NULL;
  json_object *json_row = NULL;
  
  now = pim_time_monotonic_sec();

  if (uj)
    json = json_object_new_object();
  else
    vty_out(vty,
            "Interface Address         Source          Group           State  Uptime   Expire Prune%s",
            VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *ch_node;
    struct pim_ifchannel *ch;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, ch_node, ch)) {
      char ch_src_str[100];
      char ch_grp_str[100];
      char uptime[10];
      char expire[10];
      char prune[10];

      pim_inet4_dump("<ch_src?>", ch->sg.src,
		     ch_src_str, sizeof(ch_src_str));
      pim_inet4_dump("<ch_grp?>", ch->sg.grp,
		     ch_grp_str, sizeof(ch_grp_str));

      pim_time_uptime_begin(uptime, sizeof(uptime), now, ch->ifjoin_creation);
      pim_time_timer_to_mmss(expire, sizeof(expire),
			     ch->t_ifjoin_expiry_timer);
      pim_time_timer_to_mmss(prune, sizeof(prune),
			     ch->t_ifjoin_prune_pending_timer);

      if (uj) {
        json_object_object_get_ex(json, ifp->name, &json_iface);

        if (!json_iface) {
          json_iface = json_object_new_object();
          json_object_pim_ifp_add(json_iface, ifp);
          json_object_object_add(json, ifp->name, json_iface);
        }

        json_row = json_object_new_object();
        json_object_string_add(json_row, "source", ch_src_str);
        json_object_string_add(json_row, "group", ch_grp_str);
        json_object_string_add(json_row, "upTime", uptime);
        json_object_string_add(json_row, "expire", expire);
        json_object_string_add(json_row, "prune", prune);
        json_object_string_add(json_row, "channelJoinName", pim_ifchannel_ifjoin_name(ch->ifjoin_state));
        json_object_object_add(json_iface, ch_grp_str, json_row);

      } else {
        vty_out(vty, "%-9s %-15s %-15s %-15s %-6s %8s %-6s %5s%s",
                ifp->name,
                inet_ntoa(ifaddr),
                ch_src_str,
                ch_grp_str,
                pim_ifchannel_ifjoin_name(ch->ifjoin_state),
                uptime,
                expire,
                prune,
                VTY_NEWLINE);
      }
    } /* scan interface channels */
  } /* scan interfaces */

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void pim_show_neighbors_single(struct vty *vty, const char *neighbor, u_char uj)
{
  struct listnode  *node;
  struct listnode *neighnode;
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  struct pim_neighbor *neigh;
  time_t            now;
  int found_neighbor = 0;
  int option_address_list;
  int option_dr_priority;
  int option_generation_id;
  int option_holdtime;
  int option_lan_prune_delay;
  int option_t_bit;
  char uptime[10];
  char expire[10];
  char neigh_src_str[100];

  json_object *json = NULL;
  json_object *json_ifp = NULL;
  json_object *json_row = NULL;

  now = pim_time_monotonic_sec();

  if (uj)
    json = json_object_new_object();

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    pim_ifp = ifp->info;

    if (!pim_ifp)
      continue;

    if (pim_ifp->pim_sock_fd < 0)
      continue;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode, neigh)) {
      pim_inet4_dump("<src?>", neigh->source_addr,
		     neigh_src_str, sizeof(neigh_src_str));

      /*
       * The user can specify either the interface name or the PIM neighbor IP.
       * If this pim_ifp matches neither then skip.
       */
      if (strcmp(neighbor, ifp->name) && strcmp(neighbor, neigh_src_str))
        continue;

      found_neighbor = 1;
      pim_time_uptime(uptime, sizeof(uptime), now - neigh->creation);
      pim_time_timer_to_hhmmss(expire, sizeof(expire), neigh->t_expire_timer);

      option_address_list = 0;
      option_dr_priority = 0;
      option_generation_id = 0;
      option_holdtime = 0;
      option_lan_prune_delay = 0;
      option_t_bit = 0;

      if (PIM_OPTION_IS_SET(neigh->hello_options, PIM_OPTION_MASK_ADDRESS_LIST))
        option_address_list = 1;

      if (PIM_OPTION_IS_SET(neigh->hello_options, PIM_OPTION_MASK_DR_PRIORITY))
        option_dr_priority = 1;

      if (PIM_OPTION_IS_SET(neigh->hello_options, PIM_OPTION_MASK_GENERATION_ID))
        option_generation_id = 1;

      if (PIM_OPTION_IS_SET(neigh->hello_options, PIM_OPTION_MASK_HOLDTIME))
        option_holdtime = 1;

      if (PIM_OPTION_IS_SET(neigh->hello_options, PIM_OPTION_MASK_LAN_PRUNE_DELAY))
        option_lan_prune_delay = 1;

      if (PIM_OPTION_IS_SET(neigh->hello_options, PIM_OPTION_MASK_CAN_DISABLE_JOIN_SUPPRESSION))
        option_t_bit = 1;

      if (uj) {

        /* Does this ifp live in json?  If not create it. */
        json_object_object_get_ex(json, ifp->name, &json_ifp);

        if (!json_ifp) {
          json_ifp = json_object_new_object();
          json_object_pim_ifp_add(json_ifp, ifp);
          json_object_object_add(json, ifp->name, json_ifp);
        }

        json_row = json_object_new_object();
        json_object_string_add(json_row, "interface", ifp->name);
        json_object_string_add(json_row, "address", neigh_src_str);
        json_object_string_add(json_row, "upTime", uptime);
        json_object_string_add(json_row, "holdtime", expire);
        json_object_int_add(json_row, "drPriority", neigh->dr_priority);
        json_object_int_add(json_row, "generationId", neigh->generation_id);

        if (option_address_list)
          json_object_boolean_true_add(json_row, "helloOptionAddressList");

        if (option_dr_priority)
          json_object_boolean_true_add(json_row, "helloOptionDrPriority");

        if (option_generation_id)
          json_object_boolean_true_add(json_row, "helloOptionGenerationId");

        if (option_holdtime)
          json_object_boolean_true_add(json_row, "helloOptionHoldtime");

        if (option_lan_prune_delay)
          json_object_boolean_true_add(json_row, "helloOptionLanPruneDelay");

        if (option_t_bit)
          json_object_boolean_true_add(json_row, "helloOptionTBit");

        json_object_object_add(json_ifp, neigh_src_str, json_row);

      } else {
        vty_out(vty, "Interface : %s%s", ifp->name, VTY_NEWLINE);
        vty_out(vty, "Neighbor  : %s%s", neigh_src_str, VTY_NEWLINE);
        vty_out(vty, "    Uptime                         : %s%s", uptime, VTY_NEWLINE);
        vty_out(vty, "    Holdtime                       : %s%s", expire, VTY_NEWLINE);
        vty_out(vty, "    DR Priority                    : %d%s", neigh->dr_priority, VTY_NEWLINE);
        vty_out(vty, "    Generation ID                  : %08x%s", neigh->generation_id, VTY_NEWLINE);
        vty_out(vty, "    Override Interval (msec)       : %d%s", neigh->override_interval_msec, VTY_NEWLINE);
        vty_out(vty, "    Propagation Delay (msec)       : %d%s", neigh->propagation_delay_msec, VTY_NEWLINE);
        vty_out(vty, "    Hello Option - Address List    : %s%s", option_address_list ? "yes" : "no", VTY_NEWLINE);
        vty_out(vty, "    Hello Option - DR Priority     : %s%s", option_dr_priority ? "yes" : "no", VTY_NEWLINE);
        vty_out(vty, "    Hello Option - Generation ID   : %s%s", option_generation_id? "yes" : "no", VTY_NEWLINE);
        vty_out(vty, "    Hello Option - Holdtime        : %s%s", option_holdtime ? "yes" : "no", VTY_NEWLINE);
        vty_out(vty, "    Hello Option - LAN Prune Delay : %s%s", option_lan_prune_delay ? "yes" : "no", VTY_NEWLINE);
        vty_out(vty, "    Hello Option - T-bit           : %s%s", option_t_bit ? "yes" : "no", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
      }
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  } else {
    {
      if (!found_neighbor)
        vty_out (vty, "%% No such interface or neighbor%s", VTY_NEWLINE);
    }
  }
}

static void pim_show_neighbors(struct vty *vty, u_char uj)
{
  struct listnode *node;
  struct listnode *neighnode;
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  struct pim_neighbor *neigh;
  time_t now;
  char uptime[10];
  char expire[10];
  char neigh_src_str[100];
  json_object *json = NULL;
  json_object *json_ifp_rows = NULL;
  json_object *json_row = NULL;

  now = pim_time_monotonic_sec();

  if (uj) {
    json = json_object_new_object();
  } else {
    vty_out(vty, "Interface         Neighbor    Uptime  Holdtime  DR Pri%s", VTY_NEWLINE);
  }

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    pim_ifp = ifp->info;

    if (!pim_ifp)
      continue;

    if (pim_ifp->pim_sock_fd < 0)
      continue;

    if (uj)
      json_ifp_rows = json_object_new_object();

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode, neigh)) {
      pim_inet4_dump("<src?>", neigh->source_addr,
		     neigh_src_str, sizeof(neigh_src_str));
      pim_time_uptime(uptime, sizeof(uptime), now - neigh->creation);
      pim_time_timer_to_hhmmss(expire, sizeof(expire), neigh->t_expire_timer);

      if (uj) {
        json_row = json_object_new_object();
        json_object_string_add(json_row, "interface", ifp->name);
        json_object_string_add(json_row, "neighbor", neigh_src_str);
        json_object_string_add(json_row, "upTime", uptime);
        json_object_string_add(json_row, "holdTime", expire);
        json_object_int_add(json_row, "holdTimeMax", neigh->holdtime);
        json_object_int_add(json_row, "drPriority", neigh->dr_priority);
        json_object_object_add(json_ifp_rows, neigh_src_str, json_row);

      } else {
        vty_out(vty, "%-9s  %15s  %8s  %8s  %6d%s",
                ifp->name,
                neigh_src_str,
                uptime,
                expire,
                neigh->dr_priority,
                VTY_NEWLINE);
      }
    }

    if (uj) {
      json_object_object_add(json, ifp->name, json_ifp_rows);
      json_ifp_rows = NULL;
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void pim_show_neighbors_secondary(struct vty *vty)
{
  struct listnode  *node;
  struct interface *ifp;

  vty_out(vty, "Interface Address         Neighbor        Secondary      %s", VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct listnode *neighnode;
    struct pim_neighbor *neigh;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    if (pim_ifp->pim_sock_fd < 0)
      continue;

    ifaddr = pim_ifp->primary_address;

    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode, neigh)) {
      char neigh_src_str[100];
      struct listnode *prefix_node;
      struct prefix *p;

      if (!neigh->prefix_list)
	continue;

      pim_inet4_dump("<src?>", neigh->source_addr,
		     neigh_src_str, sizeof(neigh_src_str));

      for (ALL_LIST_ELEMENTS_RO(neigh->prefix_list, prefix_node, p)) {
	char neigh_sec_str[100];

	if (p->family != AF_INET)
	  continue;

	pim_inet4_dump("<src?>", p->u.prefix4,
		       neigh_sec_str, sizeof(neigh_sec_str));

	vty_out(vty, "%-9s %-15s %-15s %-15s%s",
		ifp->name,
		inet_ntoa(ifaddr),
		neigh_src_str,
		neigh_sec_str,
		VTY_NEWLINE);
      }
    }
  }
}

static void
json_object_pim_upstream_add (json_object *json, struct pim_upstream *up)
{
  if (up->flags & PIM_UPSTREAM_FLAG_MASK_DR_JOIN_DESIRED)
    json_object_boolean_true_add(json, "drJoinDesired");

  if (up->flags & PIM_UPSTREAM_FLAG_MASK_DR_JOIN_DESIRED_UPDATED)
    json_object_boolean_true_add(json, "drJoinDesiredUpdated");

  if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR)
    json_object_boolean_true_add(json, "firstHopRouter");

  if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_IGMP)
    json_object_boolean_true_add(json, "sourceIgmp");

  if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_PIM)
    json_object_boolean_true_add(json, "sourcePim");

  if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_STREAM)
    json_object_boolean_true_add(json, "sourceStream");
}

static void pim_show_upstream(struct vty *vty, u_char uj)
{
  struct listnode     *upnode;
  struct pim_upstream *up;
  time_t               now;
  json_object *json = NULL;
  json_object *json_group = NULL;
  json_object *json_row = NULL;

  now = pim_time_monotonic_sec();

  if (uj)
    json = json_object_new_object();
  else
    vty_out(vty, "Iif       Source          Group           State       Uptime   JoinTimer RSTimer   KATimer   RefCnt%s", VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO(qpim_upstream_list, upnode, up)) {
    char src_str[100];
    char grp_str[100];
    char uptime[10];
    char join_timer[10];
    char rs_timer[10];
    char ka_timer[10];

    pim_inet4_dump("<src?>", up->sg.src, src_str, sizeof(src_str));
    pim_inet4_dump("<grp?>", up->sg.grp, grp_str, sizeof(grp_str));
    pim_time_uptime(uptime, sizeof(uptime), now - up->state_transition);
    pim_time_timer_to_hhmmss (join_timer, sizeof(join_timer), up->t_join_timer);
    pim_time_timer_to_hhmmss (rs_timer, sizeof (rs_timer), up->t_rs_timer);
    pim_time_timer_to_hhmmss (ka_timer, sizeof (ka_timer), up->t_ka_timer);

    if (uj) {
      json_object_object_get_ex(json, grp_str, &json_group);

      if (!json_group) {
        json_group = json_object_new_object();
        json_object_object_add(json, grp_str, json_group);
      }

      json_row = json_object_new_object();
      json_object_pim_upstream_add(json_row, up);
      json_object_string_add(json_row, "inboundInterface", up->rpf.source_nexthop.interface->name);
      json_object_string_add(json_row, "source", src_str);
      json_object_string_add(json_row, "group", grp_str);
      json_object_string_add(json_row, "state", pim_upstream_state2str (up->join_state));
      json_object_string_add(json_row, "upTime", uptime);
      json_object_string_add(json_row, "joinTimer", join_timer);
      json_object_string_add(json_row, "resetTimer", rs_timer);
      json_object_string_add(json_row, "keepaliveTimer", ka_timer);
      json_object_int_add(json_row, "refCount", up->ref_count);
      json_object_object_add(json_group, src_str, json_row);
    } else {
      vty_out(vty, "%-10s%-15s %-15s %-11s %-8s %-9s %-9s %-9s %6d%s",
              up->rpf.source_nexthop.interface->name,
              src_str,
              grp_str,
              pim_upstream_state2str (up->join_state),
              uptime,
              join_timer,
              rs_timer,
              ka_timer,
              up->ref_count,
              VTY_NEWLINE);
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void pim_show_join_desired(struct vty *vty, u_char uj)
{
  struct listnode      *ifnode;
  struct listnode      *chnode;
  struct interface     *ifp;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;
  char src_str[100];
  char grp_str[100];
  json_object *json = NULL;
  json_object *json_group = NULL;
  json_object *json_row = NULL;

  if (uj)
    json = json_object_new_object();
  else
    vty_out(vty,
            "Interface Source          Group           LostAssert Joins PimInclude JoinDesired EvalJD%s",
            VTY_NEWLINE);

  /* scan all interfaces */
  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    pim_ifp = ifp->info;
    if (!pim_ifp)
      continue;

    /* scan per-interface (S,G) state */
    for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_ifchannel_list, chnode, ch)) {
      struct pim_upstream *up = ch->upstream;

      pim_inet4_dump("<src?>", up->sg.src, src_str, sizeof(src_str));
      pim_inet4_dump("<grp?>", up->sg.grp, grp_str, sizeof(grp_str));

      if (uj) {
        json_object_object_get_ex(json, grp_str, &json_group);

        if (!json_group) {
          json_group = json_object_new_object();
          json_object_object_add(json, grp_str, json_group);
        }

        json_row = json_object_new_object();
        json_object_pim_upstream_add(json_row, up);
        json_object_string_add(json_row, "interface", ifp->name);
        json_object_string_add(json_row, "source", src_str);
        json_object_string_add(json_row, "group", grp_str);

        if (pim_macro_ch_lost_assert(ch))
          json_object_boolean_true_add(json_row, "lostAssert");

        if (pim_macro_chisin_joins(ch))
          json_object_boolean_true_add(json_row, "joins");

        if (pim_macro_chisin_pim_include(ch))
          json_object_boolean_true_add(json_row, "pimInclude");

        if (pim_upstream_evaluate_join_desired(up))
          json_object_boolean_true_add(json_row, "evaluateJoinDesired");

        json_object_object_add(json_group, src_str, json_row);

      } else {
        vty_out(vty, "%-9s %-15s %-15s %-10s %-5s %-10s %-11s %-6s%s",
                ifp->name,
                src_str,
                grp_str,
                pim_macro_ch_lost_assert(ch) ? "yes" : "no",
                pim_macro_chisin_joins(ch) ? "yes" : "no",
                pim_macro_chisin_pim_include(ch) ? "yes" : "no",
                PIM_UPSTREAM_FLAG_TEST_DR_JOIN_DESIRED(up->flags) ? "yes" : "no",
                pim_upstream_evaluate_join_desired(up) ? "yes" : "no",
                VTY_NEWLINE);
      }
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void pim_show_upstream_rpf(struct vty *vty, u_char uj)
{
  struct listnode     *upnode;
  struct pim_upstream *up;
  json_object *json = NULL;
  json_object *json_group = NULL;
  json_object *json_row = NULL;

  if (uj)
    json = json_object_new_object();
  else
    vty_out(vty,
            "Source          Group           RpfIface RibNextHop      RpfAddress     %s",
            VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO(qpim_upstream_list, upnode, up)) {
    char src_str[100];
    char grp_str[100];
    char rpf_nexthop_str[100];
    char rpf_addr_str[100];
    struct pim_rpf *rpf;
    const char *rpf_ifname;

    rpf = &up->rpf;

    pim_inet4_dump("<src?>", up->sg.src, src_str, sizeof(src_str));
    pim_inet4_dump("<grp?>", up->sg.grp, grp_str, sizeof(grp_str));
    pim_addr_dump("<nexthop?>", &rpf->source_nexthop.mrib_nexthop_addr, rpf_nexthop_str, sizeof(rpf_nexthop_str));
    pim_addr_dump("<rpf?>", &rpf->rpf_addr, rpf_addr_str, sizeof(rpf_addr_str));

    rpf_ifname = rpf->source_nexthop.interface ? rpf->source_nexthop.interface->name : "<ifname?>";

    if (uj) {
        json_object_object_get_ex(json, grp_str, &json_group);

        if (!json_group) {
          json_group = json_object_new_object();
          json_object_object_add(json, grp_str, json_group);
        }

        json_row = json_object_new_object();
        json_object_pim_upstream_add(json_row, up);
        json_object_string_add(json_row, "source", src_str);
        json_object_string_add(json_row, "group", grp_str);
        json_object_string_add(json_row, "rpfInterface", rpf_ifname);
        json_object_string_add(json_row, "ribNexthop", rpf_nexthop_str);
        json_object_string_add(json_row, "rpfAddress", rpf_addr_str);
        json_object_object_add(json_group, src_str, json_row);
    } else {
        vty_out(vty, "%-15s %-15s %-8s %-15s %-15s%s",
                src_str,
                grp_str,
                rpf_ifname,
                rpf_nexthop_str,
                rpf_addr_str,
                VTY_NEWLINE);
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void show_rpf_refresh_stats(struct vty *vty, time_t now, json_object *json)
{
  char refresh_uptime[10];

  pim_time_uptime_begin(refresh_uptime, sizeof(refresh_uptime), now, qpim_rpf_cache_refresh_last);

  if (json) {
    json_object_int_add(json, "rpfCacheRefreshDelayMsecs", qpim_rpf_cache_refresh_delay_msec);
    json_object_int_add(json, "rpfCacheRefreshTimer", pim_time_timer_remain_msec(qpim_rpf_cache_refresher));
    json_object_int_add(json, "rpfCacheRefreshRequests", qpim_rpf_cache_refresh_requests);
    json_object_int_add(json, "rpfCacheRefreshEvents", qpim_rpf_cache_refresh_events);
    json_object_string_add(json, "rpfCacheRefreshLast", refresh_uptime);
    json_object_int_add(json, "nexthopLookups", qpim_nexthop_lookups);
  } else {
    vty_out(vty,
            "RPF Cache Refresh Delay:    %ld msecs%s"
            "RPF Cache Refresh Timer:    %ld msecs%s"
            "RPF Cache Refresh Requests: %lld%s"
            "RPF Cache Refresh Events:   %lld%s"
            "RPF Cache Refresh Last:     %s%s"
            "Nexthop Lookups:            %lld%s",
            qpim_rpf_cache_refresh_delay_msec, VTY_NEWLINE,
            pim_time_timer_remain_msec(qpim_rpf_cache_refresher), VTY_NEWLINE,
            (long long)qpim_rpf_cache_refresh_requests, VTY_NEWLINE,
            (long long)qpim_rpf_cache_refresh_events, VTY_NEWLINE,
            refresh_uptime, VTY_NEWLINE,
            (long long) qpim_nexthop_lookups, VTY_NEWLINE);
  }
}

static void show_scan_oil_stats(struct vty *vty, time_t now)
{
  char uptime_scan_oil[10];
  char uptime_mroute_add[10];
  char uptime_mroute_del[10];

  pim_time_uptime_begin(uptime_scan_oil, sizeof(uptime_scan_oil), now, qpim_scan_oil_last);
  pim_time_uptime_begin(uptime_mroute_add, sizeof(uptime_mroute_add), now, qpim_mroute_add_last);
  pim_time_uptime_begin(uptime_mroute_del, sizeof(uptime_mroute_del), now, qpim_mroute_del_last);

  vty_out(vty,
          "Scan OIL - Last: %s  Events: %lld%s"
          "MFC Add  - Last: %s  Events: %lld%s"
          "MFC Del  - Last: %s  Events: %lld%s",
          uptime_scan_oil,   (long long) qpim_scan_oil_events,   VTY_NEWLINE,
          uptime_mroute_add, (long long) qpim_mroute_add_events, VTY_NEWLINE,
          uptime_mroute_del, (long long) qpim_mroute_del_events, VTY_NEWLINE);
}

static void pim_show_rpf(struct vty *vty, u_char uj)
{
  struct listnode     *up_node;
  struct pim_upstream *up;
  time_t               now = pim_time_monotonic_sec();
  json_object *json = NULL;
  json_object *json_group = NULL;
  json_object *json_row = NULL;

  if (uj) {
    json = json_object_new_object();
    show_rpf_refresh_stats(vty, now, json);
  } else {
    show_rpf_refresh_stats(vty, now, json);
    vty_out(vty, "%s", VTY_NEWLINE);
    vty_out(vty,
            "Source          Group           RpfIface RpfAddress      RibNextHop      Metric Pref%s",
            VTY_NEWLINE);
  }

  for (ALL_LIST_ELEMENTS_RO(qpim_upstream_list, up_node, up)) {
    char src_str[100];
    char grp_str[100];
    char rpf_addr_str[100];
    char rib_nexthop_str[100];
    const char *rpf_ifname;
    struct pim_rpf  *rpf = &up->rpf;
    
    pim_inet4_dump("<src?>", up->sg.src, src_str, sizeof(src_str));
    pim_inet4_dump("<grp?>", up->sg.grp, grp_str, sizeof(grp_str));
    pim_addr_dump("<rpf?>", &rpf->rpf_addr, rpf_addr_str, sizeof(rpf_addr_str));
    pim_addr_dump("<nexthop?>", &rpf->source_nexthop.mrib_nexthop_addr, rib_nexthop_str, sizeof(rib_nexthop_str));
    
    rpf_ifname = rpf->source_nexthop.interface ? rpf->source_nexthop.interface->name : "<ifname?>";
    
    if (uj) {
      json_object_object_get_ex(json, grp_str, &json_group);

      if (!json_group) {
        json_group = json_object_new_object();
        json_object_object_add(json, grp_str, json_group);
      }

      json_row = json_object_new_object();
      json_object_string_add(json_row, "source", src_str);
      json_object_string_add(json_row, "group", grp_str);
      json_object_string_add(json_row, "rpfInterface", rpf_ifname);
      json_object_string_add(json_row, "rpfAddress", rpf_addr_str);
      json_object_string_add(json_row, "ribNexthop", rib_nexthop_str);
      json_object_int_add(json_row, "routeMetric", rpf->source_nexthop.mrib_route_metric);
      json_object_int_add(json_row, "routePreference", rpf->source_nexthop.mrib_metric_preference);
      json_object_object_add(json_group, src_str, json_row);

    } else {
      vty_out(vty, "%-15s %-15s %-8s %-15s %-15s %6d %4d%s",
              src_str,
              grp_str,
              rpf_ifname,
              rpf_addr_str,
              rib_nexthop_str,
              rpf->source_nexthop.mrib_route_metric,
              rpf->source_nexthop.mrib_metric_preference,
              VTY_NEWLINE);
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

static void igmp_show_groups(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  time_t            now;

  now = pim_time_monotonic_sec();

  vty_out(vty, "Interface Address         Group           Mode Timer    Srcs V Uptime  %s", VTY_NEWLINE);

  /* scan interfaces */
  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp = ifp->info;
    struct listnode  *sock_node;
    struct igmp_sock *igmp;
    
    if (!pim_ifp)
      continue;
    
    /* scan igmp sockets */
    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
      char ifaddr_str[100];
      struct listnode *grpnode;
      struct igmp_group *grp;

      pim_inet4_dump("<ifaddr?>", igmp->ifaddr, ifaddr_str, sizeof(ifaddr_str));

      /* scan igmp groups */
      for (ALL_LIST_ELEMENTS_RO(igmp->igmp_group_list, grpnode, grp)) {
	char group_str[100];
	char hhmmss[10];
	char uptime[10];

	pim_inet4_dump("<group?>", grp->group_addr, group_str, sizeof(group_str));
	pim_time_timer_to_hhmmss(hhmmss, sizeof(hhmmss), grp->t_group_timer);
	pim_time_uptime(uptime, sizeof(uptime), now - grp->group_creation);

	vty_out(vty, "%-9s %-15s %-15s %4s %8s %4d %d %8s%s",
		ifp->name,
		ifaddr_str,
		group_str,
		grp->group_filtermode_isexcl ? "EXCL" : "INCL",
		hhmmss,
		grp->group_source_list ? listcount(grp->group_source_list) : 0,
		igmp_group_compat_mode(igmp, grp),
		uptime,
		VTY_NEWLINE);

      } /* scan igmp groups */
    } /* scan igmp sockets */
  } /* scan interfaces */
}

static void igmp_show_group_retransmission(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;

  vty_out(vty, "Interface Address         Group           RetTimer Counter RetSrcs%s", VTY_NEWLINE);

  /* scan interfaces */
  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp = ifp->info;
    struct listnode  *sock_node;
    struct igmp_sock *igmp;
    
    if (!pim_ifp)
      continue;
    
    /* scan igmp sockets */
    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
      char ifaddr_str[100];
      struct listnode *grpnode;
      struct igmp_group *grp;

      pim_inet4_dump("<ifaddr?>", igmp->ifaddr, ifaddr_str, sizeof(ifaddr_str));

      /* scan igmp groups */
      for (ALL_LIST_ELEMENTS_RO(igmp->igmp_group_list, grpnode, grp)) {
	char group_str[100];
	char grp_retr_mmss[10];
	struct listnode    *src_node;
	struct igmp_source *src;
	int grp_retr_sources = 0;

	pim_inet4_dump("<group?>", grp->group_addr, group_str, sizeof(group_str));
	pim_time_timer_to_mmss(grp_retr_mmss, sizeof(grp_retr_mmss), grp->t_group_query_retransmit_timer);


	/* count group sources with retransmission state */
	for (ALL_LIST_ELEMENTS_RO(grp->group_source_list, src_node, src)) {
	  if (src->source_query_retransmit_count > 0) {
	    ++grp_retr_sources;
	  }
	}

	vty_out(vty, "%-9s %-15s %-15s %-8s %7d %7d%s",
		ifp->name,
		ifaddr_str,
		group_str,
		grp_retr_mmss,
		grp->group_specific_query_retransmit_count,
		grp_retr_sources,
		VTY_NEWLINE);

      } /* scan igmp groups */
    } /* scan igmp sockets */
  } /* scan interfaces */
}

static void igmp_show_sources(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;
  time_t            now;

  now = pim_time_monotonic_sec();

  vty_out(vty, "Interface Address         Group           Source          Timer Fwd Uptime  %s", VTY_NEWLINE);

  /* scan interfaces */
  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp = ifp->info;
    struct listnode  *sock_node;
    struct igmp_sock *igmp;
    
    if (!pim_ifp)
      continue;
    
    /* scan igmp sockets */
    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
      char ifaddr_str[100];
      struct listnode   *grpnode;
      struct igmp_group *grp;

      pim_inet4_dump("<ifaddr?>", igmp->ifaddr, ifaddr_str, sizeof(ifaddr_str));

      /* scan igmp groups */
      for (ALL_LIST_ELEMENTS_RO(igmp->igmp_group_list, grpnode, grp)) {
	char group_str[100];
	struct listnode    *srcnode;
	struct igmp_source *src;

	pim_inet4_dump("<group?>", grp->group_addr, group_str, sizeof(group_str));
	
	/* scan group sources */
	for (ALL_LIST_ELEMENTS_RO(grp->group_source_list, srcnode, src)) {
	  char source_str[100];
	  char mmss[10];
	  char uptime[10];

	  pim_inet4_dump("<source?>", src->source_addr, source_str, sizeof(source_str));

	  pim_time_timer_to_mmss(mmss, sizeof(mmss), src->t_source_timer);

	  pim_time_uptime(uptime, sizeof(uptime), now - src->source_creation);

	  vty_out(vty, "%-9s %-15s %-15s %-15s %5s %3s %8s%s",
		  ifp->name,
		  ifaddr_str,
		  group_str,
		  source_str,
		  mmss,
		  IGMP_SOURCE_TEST_FORWARDING(src->source_flags) ? "Y" : "N",
		  uptime,
		  VTY_NEWLINE);
	  
	} /* scan group sources */
      } /* scan igmp groups */
    } /* scan igmp sockets */
  } /* scan interfaces */
}

static void igmp_show_source_retransmission(struct vty *vty)
{
  struct listnode  *ifnode;
  struct interface *ifp;

  vty_out(vty, "Interface Address         Group           Source          Counter%s", VTY_NEWLINE);

  /* scan interfaces */
  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp = ifp->info;
    struct listnode  *sock_node;
    struct igmp_sock *igmp;
    
    if (!pim_ifp)
      continue;
    
    /* scan igmp sockets */
    for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
      char ifaddr_str[100];
      struct listnode   *grpnode;
      struct igmp_group *grp;

      pim_inet4_dump("<ifaddr?>", igmp->ifaddr, ifaddr_str, sizeof(ifaddr_str));

      /* scan igmp groups */
      for (ALL_LIST_ELEMENTS_RO(igmp->igmp_group_list, grpnode, grp)) {
	char group_str[100];
	struct listnode    *srcnode;
	struct igmp_source *src;

	pim_inet4_dump("<group?>", grp->group_addr, group_str, sizeof(group_str));
	
	/* scan group sources */
	for (ALL_LIST_ELEMENTS_RO(grp->group_source_list, srcnode, src)) {
	  char source_str[100];

	  pim_inet4_dump("<source?>", src->source_addr, source_str, sizeof(source_str));

	  vty_out(vty, "%-9s %-15s %-15s %-15s %7d%s",
		  ifp->name,
		  ifaddr_str,
		  group_str,
		  source_str,
		  src->source_query_retransmit_count,
		  VTY_NEWLINE);
	  
	} /* scan group sources */
      } /* scan igmp groups */
    } /* scan igmp sockets */
  } /* scan interfaces */
}

static void clear_igmp_interfaces()
{
  struct listnode  *ifnode;
  struct listnode  *ifnextnode;
  struct interface *ifp;

  for (ALL_LIST_ELEMENTS (vrf_iflist (VRF_DEFAULT), ifnode, ifnextnode, ifp)) {
    pim_if_addr_del_all_igmp(ifp);
  }

  for (ALL_LIST_ELEMENTS (vrf_iflist (VRF_DEFAULT), ifnode, ifnextnode, ifp)) {
    pim_if_addr_add_all(ifp);
  }
}

static void clear_pim_interfaces()
{
  struct listnode  *ifnode;
  struct listnode  *ifnextnode;
  struct interface *ifp;

  for (ALL_LIST_ELEMENTS (vrf_iflist (VRF_DEFAULT), ifnode, ifnextnode, ifp)) {
    if (ifp->info) {
      pim_neighbor_delete_all(ifp, "interface cleared");
    }
  }
}

static void clear_interfaces()
{
  clear_igmp_interfaces();
  clear_pim_interfaces();
}

DEFUN (pim_interface,
       pim_interface_cmd,
       "interface IFNAME",
       "Select an interface to configure\n"
       "Interface's name\n")
{
  struct interface *ifp;
  const char *ifname = argv[0];
  size_t sl;

  sl = strlen(ifname);
  if (sl > INTERFACE_NAMSIZ) {
    vty_out(vty, "%% Interface name %s is invalid: length exceeds "
	    "%d characters%s",
	    ifname, INTERFACE_NAMSIZ, VTY_NEWLINE);
    return CMD_WARNING;
  }

  ifp = if_lookup_by_name_len(ifname, sl);
  if (!ifp) {
    vty_out(vty, "%% Interface %s does not exist%s", ifname, VTY_NEWLINE);

    /* Returning here would prevent pimd from booting when there are
       interface commands in pimd.conf, since all interfaces are
       unknown at pimd boot time (the zebra daemon has not been
       contacted for interface discovery). */
    
    ifp = if_get_by_name_len(ifname, sl);
    if (!ifp) {
      vty_out(vty, "%% Could not create interface %s%s", ifname, VTY_NEWLINE);
      return CMD_WARNING;
    }
  }

  vty->index = ifp;
  vty->node = INTERFACE_NODE;

  return CMD_SUCCESS;
}

DEFUN (clear_ip_interfaces,
       clear_ip_interfaces_cmd,
       "clear ip interfaces",
       CLEAR_STR
       IP_STR
       "Reset interfaces\n")
{
  clear_interfaces();

  return CMD_SUCCESS;
}

DEFUN (clear_ip_igmp_interfaces,
       clear_ip_igmp_interfaces_cmd,
       "clear ip igmp interfaces",
       CLEAR_STR
       IP_STR
       CLEAR_IP_IGMP_STR
       "Reset IGMP interfaces\n")
{
  clear_igmp_interfaces();

  return CMD_SUCCESS;
}

static void mroute_add_all()
{
  struct listnode    *node;
  struct channel_oil *c_oil;

  for (ALL_LIST_ELEMENTS_RO(qpim_channel_oil_list, node, c_oil)) {
    if (pim_mroute_add(c_oil)) {
      /* just log warning */
      char source_str[100];
      char group_str[100];
      pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, source_str, sizeof(source_str));
      pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, group_str, sizeof(group_str));
      zlog_warn("%s %s: (S,G)=(%s,%s) failure writing MFC",
                __FILE__, __PRETTY_FUNCTION__,
                source_str, group_str);
    }
  }
}

static void mroute_del_all()
{
  struct listnode    *node;
  struct channel_oil *c_oil;

  for (ALL_LIST_ELEMENTS_RO(qpim_channel_oil_list, node, c_oil)) {
    if (pim_mroute_del(c_oil)) {
      /* just log warning */
      char source_str[100];
      char group_str[100];
      pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, source_str, sizeof(source_str));
      pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, group_str, sizeof(group_str));
      zlog_warn("%s %s: (S,G)=(%s,%s) failure clearing MFC",
                __FILE__, __PRETTY_FUNCTION__,
                source_str, group_str);
    }
  }
}

static void static_mroute_add_all()
{
  struct listnode     *node;
  struct static_route *s_route;

  for (ALL_LIST_ELEMENTS_RO(qpim_static_route_list, node, s_route)) {
    if (pim_mroute_add(&s_route->c_oil)) {
      /* just log warning */
      char source_str[100];
      char group_str[100];
      pim_inet4_dump("<source?>", s_route->c_oil.oil.mfcc_origin, source_str, sizeof(source_str));
      pim_inet4_dump("<group?>", s_route->c_oil.oil.mfcc_mcastgrp, group_str, sizeof(group_str));
      zlog_warn("%s %s: (S,G)=(%s,%s) failure writing MFC",
      __FILE__, __PRETTY_FUNCTION__,
      source_str, group_str);
    }
  }
}

static void static_mroute_del_all()
{
   struct listnode     *node;
   struct static_route *s_route;

   for (ALL_LIST_ELEMENTS_RO(qpim_static_route_list, node, s_route)) {
     if (pim_mroute_del(&s_route->c_oil)) {
       /* just log warning */
       char source_str[100];
       char group_str[100];
       pim_inet4_dump("<source?>", s_route->c_oil.oil.mfcc_origin, source_str, sizeof(source_str));
       pim_inet4_dump("<group?>", s_route->c_oil.oil.mfcc_mcastgrp, group_str, sizeof(group_str));
       zlog_warn("%s %s: (S,G)=(%s,%s) failure clearing MFC",
       __FILE__, __PRETTY_FUNCTION__,
       source_str, group_str);
     }
   }
}

DEFUN (clear_ip_mroute,
       clear_ip_mroute_cmd,
       "clear ip mroute",
       CLEAR_STR
       IP_STR
       "Reset multicast routes\n")
{
  mroute_del_all();
  mroute_add_all();

  return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_interfaces,
       clear_ip_pim_interfaces_cmd,
       "clear ip pim interfaces",
       CLEAR_STR
       IP_STR
       CLEAR_IP_PIM_STR
       "Reset PIM interfaces\n")
{
  clear_pim_interfaces();

  return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_oil,
       clear_ip_pim_oil_cmd,
       "clear ip pim oil",
       CLEAR_STR
       IP_STR
       CLEAR_IP_PIM_STR
       "Rescan PIM OIL (output interface list)\n")
{
  pim_scan_oil();

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_interface,
       show_ip_igmp_interface_cmd,
       "show ip igmp interface {json}",
       SHOW_STR
       IP_STR
       IGMP_STR
       "IGMP interface information\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  igmp_show_interfaces(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_interface_single,
       show_ip_igmp_interface_single_cmd,
       "show ip igmp interface WORD {json}",
       SHOW_STR
       IP_STR
       IGMP_STR
       "IGMP interface information\n"
       "interface name\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  igmp_show_interfaces_single(vty, argv[0], uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_join,
       show_ip_igmp_join_cmd,
       "show ip igmp join",
       SHOW_STR
       IP_STR
       IGMP_STR
       "IGMP static join information\n")
{
  igmp_show_interface_join(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_groups,
       show_ip_igmp_groups_cmd,
       "show ip igmp groups",
       SHOW_STR
       IP_STR
       IGMP_STR
       IGMP_GROUP_STR)
{
  igmp_show_groups(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_groups_retransmissions,
       show_ip_igmp_groups_retransmissions_cmd,
       "show ip igmp groups retransmissions",
       SHOW_STR
       IP_STR
       IGMP_STR
       IGMP_GROUP_STR
       "IGMP group retransmissions\n")
{
  igmp_show_group_retransmission(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_sources,
       show_ip_igmp_sources_cmd,
       "show ip igmp sources",
       SHOW_STR
       IP_STR
       IGMP_STR
       IGMP_SOURCE_STR)
{
  igmp_show_sources(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_sources_retransmissions,
       show_ip_igmp_sources_retransmissions_cmd,
       "show ip igmp sources retransmissions",
       SHOW_STR
       IP_STR
       IGMP_STR
       IGMP_SOURCE_STR
       "IGMP source retransmissions\n")
{
  igmp_show_source_retransmission(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert,
       show_ip_pim_assert_cmd,
       "show ip pim assert",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface assert\n")
{
  pim_show_assert(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert_internal,
       show_ip_pim_assert_internal_cmd,
       "show ip pim assert-internal",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface internal assert state\n")
{
  pim_show_assert_internal(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert_metric,
       show_ip_pim_assert_metric_cmd,
       "show ip pim assert-metric",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface assert metric\n")
{
  pim_show_assert_metric(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert_winner_metric,
       show_ip_pim_assert_winner_metric_cmd,
       "show ip pim assert-winner-metric",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface assert winner metric\n")
{
  pim_show_assert_winner_metric(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_interface,
       show_ip_pim_interface_cmd,
       "show ip pim interface {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface information\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_interfaces(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_interface_single,
       show_ip_pim_interface_single_cmd,
       "show ip pim interface WORD {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface information\n"
       "interface name\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_interfaces_single(vty, argv[0], uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_join,
       show_ip_pim_join_cmd,
       "show ip pim join {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface join information\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_join(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_local_membership,
       show_ip_pim_local_membership_cmd,
       "show ip pim local-membership {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM interface local-membership\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_membership(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_neighbor,
       show_ip_pim_neighbor_cmd,
       "show ip pim neighbor {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM neighbor information\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_neighbors(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_neighbor_single,
       show_ip_pim_neighbor_single_cmd,
       "show ip pim neighbor WORD {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM neighbor information\n"
       "Name of interface or neighbor\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_neighbors_single(vty, argv[0], uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_secondary,
       show_ip_pim_secondary_cmd,
       "show ip pim secondary",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM neighbor addresses\n")
{
  pim_show_neighbors_secondary(vty);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_upstream,
       show_ip_pim_upstream_cmd,
       "show ip pim upstream {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM upstream information\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_upstream(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_upstream_join_desired,
       show_ip_pim_upstream_join_desired_cmd,
       "show ip pim upstream-join-desired {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM upstream join-desired\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_join_desired(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_upstream_rpf,
       show_ip_pim_upstream_rpf_cmd,
       "show ip pim upstream-rpf {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM upstream source rpf\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_upstream_rpf(vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_rp,
       show_ip_pim_rp_cmd,
       "show ip pim rp-info {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM RP information\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_rp_show_information (vty, uj);

  return CMD_SUCCESS;
}

DEFUN (show_ip_pim_rpf,
       show_ip_pim_rpf_cmd,
       "show ip pim rpf {json}",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM cached source rpf information\n"
       "JavaScript Object Notation\n")
{
  u_char uj = use_json(argc, argv);
  pim_show_rpf(vty, uj);

  return CMD_SUCCESS;
}

static void show_multicast_interfaces(struct vty *vty)
{
  struct listnode  *node;
  struct interface *ifp;

  vty_out(vty, "%s", VTY_NEWLINE);
  
  vty_out(vty, "Interface Address         ifi Vif  PktsIn PktsOut    BytesIn   BytesOut%s",
	  VTY_NEWLINE);

  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), node, ifp)) {
    struct pim_interface *pim_ifp;
    struct in_addr ifaddr;
    struct sioc_vif_req vreq;

    pim_ifp = ifp->info;
    
    if (!pim_ifp)
      continue;

    memset(&vreq, 0, sizeof(vreq));
    vreq.vifi = pim_ifp->mroute_vif_index;

    if (ioctl(qpim_mroute_socket_fd, SIOCGETVIFCNT, &vreq)) {
      zlog_warn("ioctl(SIOCGETVIFCNT=%lu) failure for interface %s vif_index=%d: errno=%d: %s%s",
		(unsigned long)SIOCGETVIFCNT,
		ifp->name,
		pim_ifp->mroute_vif_index,
		errno,
		safe_strerror(errno),
		VTY_NEWLINE);
    }

    ifaddr = pim_ifp->primary_address;

    vty_out(vty, "%-9s %-15s %3d %3d %7lu %7lu %10lu %10lu%s",
	    ifp->name,
	    inet_ntoa(ifaddr),
	    ifp->ifindex,
	    pim_ifp->mroute_vif_index,
	    (unsigned long) vreq.icount,
	    (unsigned long) vreq.ocount,
	    (unsigned long) vreq.ibytes,
	    (unsigned long) vreq.obytes,
	    VTY_NEWLINE);
  }
}

DEFUN (show_ip_multicast,
       show_ip_multicast_cmd,
       "show ip multicast",
       SHOW_STR
       IP_STR
       "Multicast global information\n")
{
  time_t now = pim_time_monotonic_sec();

  if (PIM_MROUTE_IS_ENABLED) {
    char uptime[10];

    vty_out(vty, "Mroute socket descriptor: %d%s",
	    qpim_mroute_socket_fd,
	    VTY_NEWLINE);

    pim_time_uptime(uptime, sizeof(uptime), now - qpim_mroute_socket_creation);
    vty_out(vty, "Mroute socket uptime: %s%s",
	    uptime,
	    VTY_NEWLINE);
  }
  else {
    vty_out(vty, "Multicast disabled%s",
	    VTY_NEWLINE);
  }

  vty_out(vty, "%s", VTY_NEWLINE);
  vty_out(vty, "Zclient update socket: ");
  if (qpim_zclient_update) {
    vty_out(vty, "%d failures=%d%s", qpim_zclient_update->sock,
	    qpim_zclient_update->fail, VTY_NEWLINE);
  }
  else {
    vty_out(vty, "<null zclient>%s", VTY_NEWLINE);
  }

  pim_zlookup_show_ip_multicast (vty);

  vty_out(vty, "%s", VTY_NEWLINE);
  vty_out(vty, "Current highest VifIndex: %d%s",
	  qpim_mroute_oif_highest_vif_index,
	  VTY_NEWLINE);
  vty_out(vty, "Maximum highest VifIndex: %d%s",
	  PIM_MAX_USABLE_VIFS,
	  VTY_NEWLINE);

  vty_out(vty, "%s", VTY_NEWLINE);
  vty_out(vty, "Upstream Join Timer: %d secs%s",
	  qpim_t_periodic,
	  VTY_NEWLINE);
  vty_out(vty, "Join/Prune Holdtime: %d secs%s",
	  PIM_JP_HOLDTIME,
	  VTY_NEWLINE);

  vty_out(vty, "%s", VTY_NEWLINE);

  show_rpf_refresh_stats(vty, now, NULL);

  vty_out(vty, "%s", VTY_NEWLINE);

  show_scan_oil_stats(vty, now);

  show_multicast_interfaces(vty);
  
  return CMD_SUCCESS;
}

static void show_mroute(struct vty *vty, u_char uj)
{
  struct listnode    *node;
  struct channel_oil *c_oil;
  struct static_route *s_route;
  time_t              now;
  json_object *json = NULL;
  json_object *json_group = NULL;
  json_object *json_source = NULL;
  json_object *json_ifp_in = NULL;
  json_object *json_ifp_out = NULL;
  int found_oif = 0;

  if (uj) {
    json = json_object_new_object();
  } else {
    vty_out(vty, "Proto: I=IGMP P=PIM S=STATIC O=SOURCE%s%s", VTY_NEWLINE, VTY_NEWLINE);
    vty_out(vty, "Source          Group           Proto Input iVifI Output oVifI TTL Uptime  %s",
            VTY_NEWLINE);
  }

  now = pim_time_monotonic_sec();

  /* print list of PIM and IGMP routes */
  for (ALL_LIST_ELEMENTS_RO(qpim_channel_oil_list, node, c_oil)) {
    char grp_str[100];
    char src_str[100];
    char in_ifname[16];
    char out_ifname[16];
    int oif_vif_index;
    char proto[5];
    struct interface *ifp_in;
    found_oif = 0;

    if (!c_oil->installed)
      continue;

    pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, grp_str, sizeof(grp_str));
    pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, src_str, sizeof(src_str));
    ifp_in = pim_if_find_by_vif_index(c_oil->oil.mfcc_parent);

    if (ifp_in)
      strcpy(in_ifname, ifp_in->name);
    else
      strcpy(in_ifname, "<iif?>");

    if (uj) {

      /* Find the group, create it if it doesn't exist */
      json_object_object_get_ex(json, grp_str, &json_group);

      if (!json_group) {
        json_group = json_object_new_object();
        json_object_object_add(json, grp_str, json_group);
      }

      /* Find the source nested under the group, create it if it doesn't exist */
      json_object_object_get_ex(json_group, src_str, &json_source);

      if (!json_source) {
        json_source = json_object_new_object();
        json_object_object_add(json_group, src_str, json_source);
      }

      /* Find the inbound interface nested under the source, create it if it doesn't exist */
      json_object_object_get_ex(json_source, in_ifname, &json_ifp_in);

      if (!json_ifp_in) {
        json_ifp_in = json_object_new_object();
        json_object_object_add(json_source, in_ifname, json_ifp_in);
      }
    }

    for (oif_vif_index = 0; oif_vif_index < MAXVIFS; ++oif_vif_index) {
      struct interface *ifp_out;
      char oif_uptime[10];
      int ttl;

      ttl = c_oil->oil.mfcc_ttls[oif_vif_index];
      if (ttl < 1)
	continue;

      ifp_out = pim_if_find_by_vif_index(oif_vif_index);
      pim_time_uptime(oif_uptime, sizeof(oif_uptime), now - c_oil->oif_creation[oif_vif_index]);
      found_oif = 1;

      if (ifp_out)
        strcpy(out_ifname, ifp_out->name);
      else
        strcpy(out_ifname, "<oif?>");

      if (uj) {
        json_ifp_out = json_object_new_object();
        json_object_string_add(json_ifp_out, "source", src_str);
        json_object_string_add(json_ifp_out, "group", grp_str);

        if (c_oil->oif_flags[oif_vif_index] & PIM_OIF_FLAG_PROTO_PIM)
          json_object_boolean_true_add(json_ifp_out, "protocolPim");

        if (c_oil->oif_flags[oif_vif_index] & PIM_OIF_FLAG_PROTO_IGMP)
          json_object_boolean_true_add(json_ifp_out, "protocolIgmp");

        if (c_oil->oif_flags[oif_vif_index] & PIM_OIF_FLAG_PROTO_SOURCE)
          json_object_boolean_true_add(json_ifp_out, "protocolSource");

        json_object_string_add(json_ifp_out, "inboundInterface", in_ifname);
        json_object_int_add(json_ifp_out, "iVifI", c_oil->oil.mfcc_parent);
        json_object_string_add(json_ifp_out, "outboundInterface", out_ifname);
        json_object_int_add(json_ifp_out, "oVifI", oif_vif_index);
        json_object_int_add(json_ifp_out, "ttl", ttl);
        json_object_string_add(json_ifp_out, "upTime", oif_uptime);
        json_object_object_add(json_ifp_in, out_ifname, json_ifp_out);
      } else {
        proto[0] = '\0';

        if (c_oil->oif_flags[oif_vif_index] & PIM_OIF_FLAG_PROTO_PIM) {
          strcat(proto, "P");
        }

        if (c_oil->oif_flags[oif_vif_index] & PIM_OIF_FLAG_PROTO_IGMP) {
          strcat(proto, "I");
        }

        if (c_oil->oif_flags[oif_vif_index] & PIM_OIF_FLAG_PROTO_SOURCE) {
          strcat(proto, "O");
        }

        vty_out(vty, "%-15s %-15s %-5s %-5s %5d %-6s %5d %3d %8s %s",
                src_str,
                grp_str,
                proto,
                in_ifname,
                c_oil->oil.mfcc_parent,
                out_ifname,
                oif_vif_index,
                ttl,
                oif_uptime,
                VTY_NEWLINE);
      }
    }

    if (!uj && !found_oif) {
      vty_out(vty, "%-15s %-15s %-5s %-5s %5d %-6s %5d %3d %8s %s",
              src_str,
              grp_str,
              proto,
              in_ifname,
              c_oil->oil.mfcc_parent,
              "none",
              0,
              0,
              "--:--:--",
              VTY_NEWLINE);
    }
  }

  /* Print list of static routes */
  for (ALL_LIST_ELEMENTS_RO(qpim_static_route_list, node, s_route)) {
    char grp_str[100];
    char src_str[100];
    char in_ifname[16];
    char out_ifname[16];
    int oif_vif_index;
    struct interface *ifp_in;
    char proto[5];

    if (!s_route->c_oil.installed)
      continue;

    pim_inet4_dump("<group?>", s_route->group, grp_str, sizeof(grp_str));
    pim_inet4_dump("<source?>", s_route->source, src_str, sizeof(src_str));
    ifp_in  = pim_if_find_by_vif_index(s_route->iif);
    found_oif = 0;

    if (ifp_in)
      strcpy(in_ifname, ifp_in->name);
    else
      strcpy(in_ifname, "<iif?>");

    if (uj) {

      /* Find the group, create it if it doesn't exist */
      json_object_object_get_ex(json, grp_str, &json_group);

      if (!json_group) {
        json_group = json_object_new_object();
        json_object_object_add(json, grp_str, json_group);
      }

      /* Find the source nested under the group, create it if it doesn't exist */
      json_object_object_get_ex(json_group, src_str, &json_source);

      if (!json_source) {
        json_source = json_object_new_object();
        json_object_object_add(json_group, src_str, json_source);
      }

      /* Find the inbound interface nested under the source, create it if it doesn't exist */
      json_object_object_get_ex(json_source, in_ifname, &json_ifp_in);

      if (!json_ifp_in) {
        json_ifp_in = json_object_new_object();
        json_object_object_add(json_source, in_ifname, json_ifp_in);
      }

    } else {
      proto[0] = '\0';
      strcat(proto, "S");
    }

    for (oif_vif_index = 0; oif_vif_index < MAXVIFS; ++oif_vif_index) {
      struct interface *ifp_out;
      char oif_uptime[10];
      int ttl;

      ttl = s_route->oif_ttls[oif_vif_index];
      if (ttl < 1)
         continue;

      ifp_out = pim_if_find_by_vif_index(oif_vif_index);
      pim_time_uptime(oif_uptime, sizeof(oif_uptime), now - s_route->c_oil.oif_creation[oif_vif_index]);
      found_oif = 1;

      if (ifp_out)
        strcpy(out_ifname, ifp_out->name);
      else
        strcpy(out_ifname, "<oif?>");

      if (uj) {
        json_ifp_out = json_object_new_object();
        json_object_string_add(json_ifp_out, "source", src_str);
        json_object_string_add(json_ifp_out, "group", grp_str);
        json_object_boolean_true_add(json_ifp_out, "protocolStatic");
        json_object_string_add(json_ifp_out, "inboundInterface", in_ifname);
        json_object_int_add(json_ifp_out, "iVifI", c_oil->oil.mfcc_parent);
        json_object_string_add(json_ifp_out, "outboundInterface", out_ifname);
        json_object_int_add(json_ifp_out, "oVifI", oif_vif_index);
        json_object_int_add(json_ifp_out, "ttl", ttl);
        json_object_string_add(json_ifp_out, "upTime", oif_uptime);
        json_object_object_add(json_ifp_in, out_ifname, json_ifp_out);
      } else {
        vty_out(vty, "%-15s %-15s %-5s %-5s %5d %-6s %5d %3d %8s %s",
                src_str,
                grp_str,
                proto,
                in_ifname,
                s_route->iif,
                out_ifname,
                oif_vif_index,
                ttl,
                oif_uptime,
                VTY_NEWLINE);
      }
    }

    if (!uj && !found_oif) {
        vty_out(vty, "%-15s %-15s %-5s %-5s %5d %-6s %5d %3d %8s %s",
                src_str,
                grp_str,
                proto,
                in_ifname,
                c_oil->oil.mfcc_parent,
                "none",
                0,
                0,
                "--:--:--",
                VTY_NEWLINE);
    }
  }

  if (uj) {
    vty_out (vty, "%s%s", json_object_to_json_string(json), VTY_NEWLINE);
    json_object_free(json);
  }
}

DEFUN (show_ip_mroute,
       show_ip_mroute_cmd,
       "show ip mroute {json}",
       SHOW_STR
       IP_STR
       MROUTE_STR)
{
  u_char uj = use_json(argc, argv);
  show_mroute(vty, uj);
  return CMD_SUCCESS;
}

static void show_mroute_count(struct vty *vty)
{
  struct listnode    *node;
  struct channel_oil *c_oil;
  struct static_route *s_route;

  vty_out(vty, "%s", VTY_NEWLINE);
  
  vty_out(vty, "Source          Group           Packets      Bytes WrongIf  %s",
	  VTY_NEWLINE);

  /* Print PIM and IGMP route counts */
  for (ALL_LIST_ELEMENTS_RO(qpim_channel_oil_list, node, c_oil)) {
    char group_str[100]; 
    char source_str[100];

    if (!c_oil->installed)
      continue;

    pim_mroute_update_counters (c_oil);

    pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, group_str, sizeof(group_str));
    pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, source_str, sizeof(source_str));

    vty_out(vty, "%-15s %-15s %7ld %10ld %7ld %s",
	    source_str,
	    group_str,
	    c_oil->cc.pktcnt,
	    c_oil->cc.bytecnt,
	    c_oil->cc.wrong_if,
	    VTY_NEWLINE);
  }

   /* Print static route counts */
  for (ALL_LIST_ELEMENTS_RO(qpim_static_route_list, node, s_route)) {
    char group_str[100];
    char source_str[100];

    if (!s_route->c_oil.installed)
      continue;

    pim_mroute_update_counters (&s_route->c_oil);

    pim_inet4_dump("<group?>", s_route->c_oil.oil.mfcc_mcastgrp, group_str, sizeof(group_str));
    pim_inet4_dump("<source?>", s_route->c_oil.oil.mfcc_origin, source_str, sizeof(source_str));

    vty_out(vty, "%-15s %-15s %7ld %10ld %7ld %s",
       source_str,
       group_str,
       s_route->c_oil.cc.pktcnt,
       s_route->c_oil.cc.bytecnt,
       s_route->c_oil.cc.wrong_if,
       VTY_NEWLINE);
  }
}

DEFUN (show_ip_mroute_count,
       show_ip_mroute_count_cmd,
       "show ip mroute count",
       SHOW_STR
       IP_STR
       MROUTE_STR
       "Route and packet count data\n")
{
  show_mroute_count(vty);
  return CMD_SUCCESS;
}

DEFUN (show_ip_rib,
       show_ip_rib_cmd,
       "show ip rib A.B.C.D",
       SHOW_STR
       IP_STR
       RIB_STR
       "Unicast address\n")
{
  struct in_addr addr;
  const char *addr_str;
  struct pim_nexthop nexthop;
  char nexthop_addr_str[100];
  int result;

  addr_str = argv[0];
  result = inet_pton(AF_INET, addr_str, &addr);
  if (result <= 0) {
    vty_out(vty, "Bad unicast address %s: errno=%d: %s%s",
	    addr_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  if (pim_nexthop_lookup(&nexthop, addr, 0)) {
    vty_out(vty, "Failure querying RIB nexthop for unicast address %s%s",
	    addr_str, VTY_NEWLINE);
    return CMD_WARNING;
  }

  vty_out(vty, "Address         NextHop         Interface Metric Preference%s",
	  VTY_NEWLINE);

  pim_addr_dump("<nexthop?>", &nexthop.mrib_nexthop_addr,
		nexthop_addr_str, sizeof(nexthop_addr_str));

  vty_out(vty, "%-15s %-15s %-9s %6d %10d%s",
	  addr_str,
	  nexthop_addr_str,
	  nexthop.interface ? nexthop.interface->name : "<ifname?>",
	  nexthop.mrib_route_metric,
	  nexthop.mrib_metric_preference,
	  VTY_NEWLINE);

  return CMD_SUCCESS;
}

static void show_ssmpingd(struct vty *vty)
{
  struct listnode      *node;
  struct ssmpingd_sock *ss;
  time_t                now;

  vty_out(vty, "Source          Socket Address          Port Uptime   Requests%s",
	  VTY_NEWLINE);

  if (!qpim_ssmpingd_list)
    return;

  now = pim_time_monotonic_sec();

  for (ALL_LIST_ELEMENTS_RO(qpim_ssmpingd_list, node, ss)) {
    char source_str[100];
    char ss_uptime[10];
    struct sockaddr_in bind_addr;
    socklen_t len = sizeof(bind_addr);
    char bind_addr_str[100];

    pim_inet4_dump("<src?>", ss->source_addr, source_str, sizeof(source_str));

    if (pim_socket_getsockname(ss->sock_fd, (struct sockaddr *) &bind_addr, &len)) {
      vty_out(vty, "%% Failure reading socket name for ssmpingd source %s on fd=%d%s",
	      source_str, ss->sock_fd, VTY_NEWLINE);
    }

    pim_inet4_dump("<addr?>", bind_addr.sin_addr, bind_addr_str, sizeof(bind_addr_str));
    pim_time_uptime(ss_uptime, sizeof(ss_uptime), now - ss->creation);

    vty_out(vty, "%-15s %6d %-15s %5d %8s %8lld%s",
	    source_str,
	    ss->sock_fd,
	    bind_addr_str,
	    ntohs(bind_addr.sin_port),
	    ss_uptime,
	    (long long)ss->requests,
	    VTY_NEWLINE);
  }
}

DEFUN (show_ip_ssmpingd,
       show_ip_ssmpingd_cmd,
       "show ip ssmpingd",
       SHOW_STR
       IP_STR
       SHOW_SSMPINGD_STR)
{
  show_ssmpingd(vty);
  return CMD_SUCCESS;
}

static int
pim_rp_cmd_worker (struct vty *vty, const char *rp, const char *group, const char *plist)
{
  int result;

  result = pim_rp_new (rp, group, plist);

  if (result == PIM_MALLOC_FAIL)
    {
      vty_out (vty, "%% Out of memory%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_GROUP_BAD_ADDRESS)
    {
      vty_out (vty, "%% Bad group address specified: %s%s", group, VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_RP_BAD_ADDRESS)
    {
      vty_out (vty, "%% Bad RP address specified: %s%s", rp, VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_RP_NO_PATH)
    {
      vty_out (vty, "%% No Path to RP address specified: %s%s", rp, VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_GROUP_OVERLAP)
    {
      vty_out (vty, "%% Group range specified cannot overlap%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_GROUP_PFXLIST_OVERLAP)
    {
      vty_out (vty, "%% This group is already covered by a RP prefix-list%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_RP_PFXLIST_IN_USE)
    {
      vty_out (vty, "%% The same prefix-list cannot be applied to multiple RPs%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  return CMD_SUCCESS;
}

DEFUN (ip_pim_keep_alive,
       ip_pim_keep_alive_cmd,
       "ip pim keep-alive-timer <31-60000>",
       IP_STR
       "pim multicast routing\n"
       "Keep alive Timer"
       "Seconds")
{
  VTY_GET_INTEGER_RANGE ("keep-alive-timer", qpim_keep_alive_time,
			 argv[0], 31, 60000);
  return CMD_SUCCESS;
}

DEFUN (no_ip_pim_keep_alive,
       no_ip_pim_keep_alive_cmd,
       "no ip pim keep-alive-timer <31-60000>",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Keep alive Timer\n"
       "Seconds\n")
{
  qpim_keep_alive_time = PIM_KEEPALIVE_PERIOD;
  return CMD_SUCCESS;
}

DEFUN (ip_pim_rp_keep_alive,
       ip_pim_rp_keep_alive_cmd,
       "ip pim rp keep-alive-timer <31-60000>",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "Keep alive Timer\n"
       "Seconds\n")
{
  VTY_GET_INTEGER_RANGE ("keep-alive-timer", qpim_rp_keep_alive_time,
			 argv[0], 31, 600);
  return CMD_SUCCESS;
}

DEFUN (no_ip_pim_rp_keep_alive,
       no_ip_pim_rp_keep_alive_cmd,
       "no ip pim rp keep-alive-timer <31-60000>",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "Keep alive Timer\n"
       "Seconds\n")
{
  qpim_rp_keep_alive_time = PIM_RP_KEEPALIVE_PERIOD;
  return CMD_SUCCESS;
}

DEFUN (ip_pim_rp,
       ip_pim_rp_cmd,
       "ip pim rp A.B.C.D",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n")
{
  return pim_rp_cmd_worker (vty, argv[0], NULL, NULL);
}

DEFUN (ip_pim_rp_range,
       ip_pim_rp_range_cmd,
       "ip pim rp A.B.C.D A.B.C.D/M",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "Group range for RP\n")
{
  return pim_rp_cmd_worker (vty, argv[0], argv[1], NULL);
}

DEFUN (ip_pim_rp_prefix_list,
       ip_pim_rp_prefix_list_cmd,
       "ip pim rp A.B.C.D prefix-list WORD",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "group prefix-list filter\n"
       "Name of a prefix-list\n")
{
  return pim_rp_cmd_worker (vty, argv[0], NULL, argv[1]);
}

static int
pim_no_rp_cmd_worker (struct vty *vty, const char *rp, const char *group,
                      const char *plist)
{
  int result = pim_rp_del (rp, group, plist);

  if (result == PIM_GROUP_BAD_ADDRESS)
    {
      vty_out (vty, "%% Bad group address specified: %s%s", group, VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_RP_BAD_ADDRESS)
    {
      vty_out (vty, "%% Bad RP address specified: %s%s", rp, VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (result == PIM_RP_NOT_FOUND)
    {
      vty_out (vty, "%% Unable to find specified RP%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  return CMD_SUCCESS;
}

DEFUN (no_ip_pim_rp_range,
       no_ip_pim_rp_range_cmd,
       "no ip pim rp A.B.C.D A.B.C.D/M",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n")
{
  return pim_no_rp_cmd_worker (vty, argv[0], argv[1], NULL);
}

DEFUN (no_ip_pim_rp_prefix_list,
       no_ip_pim_rp_prefix_list_cmd,
       "no ip pim rp A.B.C.D prefix-list WORD",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "group prefix-list filter\n"
       "Name of a prefix-list\n")
{
  return pim_no_rp_cmd_worker (vty, argv[0], NULL, argv[1]);
}

DEFUN (ip_multicast_routing,
       ip_multicast_routing_cmd,
       PIM_CMD_IP_MULTICAST_ROUTING,
       IP_STR
       "Enable IP multicast forwarding\n")
{
  pim_mroute_socket_enable();
  pim_if_add_vif_all();
  mroute_add_all();
  static_mroute_add_all();
  return CMD_SUCCESS;
}

DEFUN (no_ip_multicast_routing,
       no_ip_multicast_routing_cmd,
       PIM_CMD_NO " " PIM_CMD_IP_MULTICAST_ROUTING,
       NO_STR
       IP_STR
       "Global IP configuration subcommands\n"
       "Enable IP multicast forwarding\n")
{
  mroute_del_all();
  static_mroute_del_all();
  pim_if_del_vif_all();
  pim_mroute_socket_disable();
  return CMD_SUCCESS;
}

DEFUN (ip_ssmpingd,
       ip_ssmpingd_cmd,
       "ip ssmpingd [A.B.C.D]",
       IP_STR
       CONF_SSMPINGD_STR
       "Source address\n")
{
  int result;
  struct in_addr source_addr;
  const char *source_str = (argc > 0) ? argv[0] : "0.0.0.0";

  result = inet_pton(AF_INET, source_str, &source_addr);
  if (result <= 0) {
    vty_out(vty, "%% Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  result = pim_ssmpingd_start(source_addr);
  if (result) {
    vty_out(vty, "%% Failure starting ssmpingd for source %s: %d%s",
	    source_str, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (no_ip_ssmpingd,
       no_ip_ssmpingd_cmd,
       "no ip ssmpingd [A.B.C.D]",
       NO_STR
       IP_STR
       CONF_SSMPINGD_STR
       "Source address\n")
{
  int result;
  struct in_addr source_addr;
  const char *source_str = (argc > 0) ? argv[0] : "0.0.0.0";

  result = inet_pton(AF_INET, source_str, &source_addr);
  if (result <= 0) {
    vty_out(vty, "%% Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  result = pim_ssmpingd_stop(source_addr);
  if (result) {
    vty_out(vty, "%% Failure stopping ssmpingd for source %s: %d%s",
	    source_str, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (interface_ip_igmp,
       interface_ip_igmp_cmd,
       "ip igmp",
       IP_STR
       IFACE_IGMP_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    pim_ifp = pim_if_new(ifp, 1 /* igmp=true */, 0 /* pim=false */);
    if (!pim_ifp) {
      vty_out(vty, "Could not enable IGMP on interface %s%s",
	      ifp->name, VTY_NEWLINE);
      return CMD_WARNING;
    }
  }
  else {
    PIM_IF_DO_IGMP(pim_ifp->options);
  }

  pim_if_addr_add_all(ifp);
  pim_if_membership_refresh(ifp);

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_igmp,
       interface_no_ip_igmp_cmd,
       "no ip igmp",
       NO_STR
       IP_STR
       IFACE_IGMP_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;

  ifp = vty->index;
  pim_ifp = ifp->info;
  if (!pim_ifp)
    return CMD_SUCCESS;

  PIM_IF_DONT_IGMP(pim_ifp->options);

  pim_if_membership_clear(ifp);

  pim_if_addr_del_all_igmp(ifp);

  if (!PIM_IF_TEST_PIM(pim_ifp->options)) {
    pim_if_delete(ifp);
  }

  return CMD_SUCCESS;
}

DEFUN (interface_ip_igmp_join,
       interface_ip_igmp_join_cmd,
       "ip igmp join A.B.C.D A.B.C.D",
       IP_STR
       IFACE_IGMP_STR
       "IGMP join multicast group\n"
       "Multicast group address\n"
       "Source address\n")
{
  struct interface *ifp;
  const char *group_str;
  const char *source_str;
  struct in_addr group_addr;
  struct in_addr source_addr;
  int result;

  ifp = vty->index;

  /* Group address */
  group_str = argv[0];
  result = inet_pton(AF_INET, group_str, &group_addr);
  if (result <= 0) {
    vty_out(vty, "Bad group address %s: errno=%d: %s%s",
	    group_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Source address */
  source_str = argv[1];
  result = inet_pton(AF_INET, source_str, &source_addr);
  if (result <= 0) {
    vty_out(vty, "Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  result = pim_if_igmp_join_add(ifp, group_addr, source_addr);
  if (result) {
    vty_out(vty, "%% Failure joining IGMP group %s source %s on interface %s: %d%s",
	    group_str, source_str, ifp->name, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_igmp_join,
       interface_no_ip_igmp_join_cmd,
       "no ip igmp join A.B.C.D A.B.C.D",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       "IGMP join multicast group\n"
       "Multicast group address\n"
       "Source address\n")
{
  struct interface *ifp;
  const char *group_str;
  const char *source_str;
  struct in_addr group_addr;
  struct in_addr source_addr;
  int result;

  ifp = vty->index;

  /* Group address */
  group_str = argv[0];
  result = inet_pton(AF_INET, group_str, &group_addr);
  if (result <= 0) {
    vty_out(vty, "Bad group address %s: errno=%d: %s%s",
	    group_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Source address */
  source_str = argv[1];
  result = inet_pton(AF_INET, source_str, &source_addr);
  if (result <= 0) {
    vty_out(vty, "Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  result = pim_if_igmp_join_del(ifp, group_addr, source_addr);
  if (result) {
    vty_out(vty, "%% Failure leaving IGMP group %s source %s on interface %s: %d%s",
	    group_str, source_str, ifp->name, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

/*
  CLI reconfiguration affects the interface level (struct pim_interface).
  This function propagates the reconfiguration to every active socket
  for that interface.
 */
static void igmp_sock_query_interval_reconfig(struct igmp_sock *igmp)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;

  zassert(igmp);

  /* other querier present? */

  if (igmp->t_other_querier_timer)
    return;

  /* this is the querier */

  zassert(igmp->interface);
  zassert(igmp->interface->info);

  ifp = igmp->interface;
  pim_ifp = ifp->info;

  if (PIM_DEBUG_IGMP_TRACE) {
    char ifaddr_str[100];
    pim_inet4_dump("<ifaddr?>", igmp->ifaddr, ifaddr_str, sizeof(ifaddr_str));
    zlog_debug("%s: Querier %s on %s reconfig query_interval=%d",
	       __PRETTY_FUNCTION__,
	       ifaddr_str,
	       ifp->name,
	       pim_ifp->igmp_default_query_interval);
  }

  /*
    igmp_startup_mode_on() will reset QQI:

    igmp->querier_query_interval = pim_ifp->igmp_default_query_interval;
  */
  igmp_startup_mode_on(igmp);
}

static void igmp_sock_query_reschedule(struct igmp_sock *igmp)
{
  if (igmp->t_igmp_query_timer) {
    /* other querier present */
    zassert(igmp->t_igmp_query_timer);
    zassert(!igmp->t_other_querier_timer);

    pim_igmp_general_query_off(igmp);
    pim_igmp_general_query_on(igmp);

    zassert(igmp->t_igmp_query_timer);
    zassert(!igmp->t_other_querier_timer);
  }
  else {
    /* this is the querier */

    zassert(!igmp->t_igmp_query_timer);
    zassert(igmp->t_other_querier_timer);

    pim_igmp_other_querier_timer_off(igmp);
    pim_igmp_other_querier_timer_on(igmp);

    zassert(!igmp->t_igmp_query_timer);
    zassert(igmp->t_other_querier_timer);
  }
}

static void change_query_interval(struct pim_interface *pim_ifp,
				  int query_interval)
{
  struct listnode  *sock_node;
  struct igmp_sock *igmp;

  pim_ifp->igmp_default_query_interval = query_interval;

  for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
    igmp_sock_query_interval_reconfig(igmp);
    igmp_sock_query_reschedule(igmp);
  }
}

static void change_query_max_response_time(struct pim_interface *pim_ifp,
					   int query_max_response_time_dsec)
{
  struct listnode  *sock_node;
  struct igmp_sock *igmp;

  pim_ifp->igmp_query_max_response_time_dsec = query_max_response_time_dsec;

  /*
    Below we modify socket/group/source timers in order to quickly
    reflect the change. Otherwise, those timers would eventually catch
    up.
   */

  /* scan all sockets */
  for (ALL_LIST_ELEMENTS_RO(pim_ifp->igmp_socket_list, sock_node, igmp)) {
    struct listnode   *grp_node;
    struct igmp_group *grp;

    /* reschedule socket general query */
    igmp_sock_query_reschedule(igmp);

    /* scan socket groups */
    for (ALL_LIST_ELEMENTS_RO(igmp->igmp_group_list, grp_node, grp)) {
      struct listnode    *src_node;
      struct igmp_source *src;

      /* reset group timers for groups in EXCLUDE mode */
      if (grp->group_filtermode_isexcl) {
	igmp_group_reset_gmi(grp);
      }

      /* scan group sources */
      for (ALL_LIST_ELEMENTS_RO(grp->group_source_list, src_node, src)) {

	/* reset source timers for sources with running timers */
	if (src->t_source_timer) {
	  igmp_source_reset_gmi(igmp, grp, src);
	}
      }
    }
  }
}

#define IGMP_QUERY_INTERVAL_MIN (1)
#define IGMP_QUERY_INTERVAL_MAX (1800)

DEFUN (interface_ip_igmp_query_interval,
       interface_ip_igmp_query_interval_cmd,
       PIM_CMD_IP_IGMP_QUERY_INTERVAL " <1-1800>",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_INTERVAL_STR
       "Query interval in seconds\n")
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  int query_interval;
  int query_interval_dsec;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty,
	    "IGMP not enabled on interface %s. Please enable IGMP first.%s",
	    ifp->name,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  query_interval = atoi(argv[0]);
  query_interval_dsec = 10 * query_interval;

  /*
    It seems we don't need to check bounds since command.c does it
    already, but we verify them anyway for extra safety.
  */
  if (query_interval < IGMP_QUERY_INTERVAL_MIN) {
    vty_out(vty, "General query interval %d lower than minimum %d%s",
	    query_interval,
	    IGMP_QUERY_INTERVAL_MIN,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }
  if (query_interval > IGMP_QUERY_INTERVAL_MAX) {
    vty_out(vty, "General query interval %d higher than maximum %d%s",
	    query_interval,
	    IGMP_QUERY_INTERVAL_MAX,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  if (query_interval_dsec <= pim_ifp->igmp_query_max_response_time_dsec) {
    vty_out(vty,
	    "Can't set general query interval %d dsec <= query max response time %d dsec.%s",
	    query_interval_dsec, pim_ifp->igmp_query_max_response_time_dsec,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  change_query_interval(pim_ifp, query_interval);

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_igmp_query_interval,
       interface_no_ip_igmp_query_interval_cmd,
       PIM_CMD_NO " " PIM_CMD_IP_IGMP_QUERY_INTERVAL,
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_INTERVAL_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  int default_query_interval_dsec;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp)
    return CMD_SUCCESS;

  default_query_interval_dsec = IGMP_GENERAL_QUERY_INTERVAL * 10;

  if (default_query_interval_dsec <= pim_ifp->igmp_query_max_response_time_dsec) {
    vty_out(vty,
	    "Can't set default general query interval %d dsec <= query max response time %d dsec.%s",
	    default_query_interval_dsec, pim_ifp->igmp_query_max_response_time_dsec,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  change_query_interval(pim_ifp, IGMP_GENERAL_QUERY_INTERVAL);

  return CMD_SUCCESS;
}

#define IGMP_QUERY_MAX_RESPONSE_TIME_MIN (1)
#define IGMP_QUERY_MAX_RESPONSE_TIME_MAX (25)

DEFUN (interface_ip_igmp_query_max_response_time,
       interface_ip_igmp_query_max_response_time_cmd,
       PIM_CMD_IP_IGMP_QUERY_MAX_RESPONSE_TIME " <1-25>",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_STR
       "Query response value in seconds\n")
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  int query_max_response_time;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty,
	    "IGMP not enabled on interface %s. Please enable IGMP first.%s",
	    ifp->name,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  query_max_response_time = atoi(argv[0]);

  /*
    It seems we don't need to check bounds since command.c does it
    already, but we verify them anyway for extra safety.
  */
  if (query_max_response_time < IGMP_QUERY_MAX_RESPONSE_TIME_MIN) {
    vty_out(vty, "Query max response time %d sec lower than minimum %d sec%s",
	    query_max_response_time,
	    IGMP_QUERY_MAX_RESPONSE_TIME_MIN,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }
  if (query_max_response_time > IGMP_QUERY_MAX_RESPONSE_TIME_MAX) {
    vty_out(vty, "Query max response time %d sec higher than maximum %d sec%s",
	    query_max_response_time,
	    IGMP_QUERY_MAX_RESPONSE_TIME_MAX,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  if (query_max_response_time >= pim_ifp->igmp_default_query_interval) {
    vty_out(vty,
	    "Can't set query max response time %d sec >= general query interval %d sec%s",
	    query_max_response_time, pim_ifp->igmp_default_query_interval,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  change_query_max_response_time(pim_ifp, 10 * query_max_response_time);

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_igmp_query_max_response_time,
       interface_no_ip_igmp_query_max_response_time_cmd,
       PIM_CMD_NO " " PIM_CMD_IP_IGMP_QUERY_MAX_RESPONSE_TIME,
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  int default_query_interval_dsec;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp)
    return CMD_SUCCESS;

  default_query_interval_dsec = 10 * pim_ifp->igmp_default_query_interval;

  if (IGMP_QUERY_MAX_RESPONSE_TIME_DSEC >= default_query_interval_dsec) {
    vty_out(vty,
	    "Can't set default query max response time %d dsec >= general query interval %d dsec.%s",
	    IGMP_QUERY_MAX_RESPONSE_TIME_DSEC, default_query_interval_dsec,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  change_query_max_response_time(pim_ifp, IGMP_QUERY_MAX_RESPONSE_TIME_DSEC);

  return CMD_SUCCESS;
}

#define IGMP_QUERY_MAX_RESPONSE_TIME_MIN_DSEC (10)
#define IGMP_QUERY_MAX_RESPONSE_TIME_MAX_DSEC (250)

DEFUN (interface_ip_igmp_query_max_response_time_dsec,
       interface_ip_igmp_query_max_response_time_dsec_cmd,
       PIM_CMD_IP_IGMP_QUERY_MAX_RESPONSE_TIME_DSEC " <10-250>",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_DSEC_STR
       "Query response value in deciseconds\n")
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  int query_max_response_time_dsec;
  int default_query_interval_dsec;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty,
	    "IGMP not enabled on interface %s. Please enable IGMP first.%s",
	    ifp->name,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  query_max_response_time_dsec = atoi(argv[0]);

  /*
    It seems we don't need to check bounds since command.c does it
    already, but we verify them anyway for extra safety.
  */
  if (query_max_response_time_dsec < IGMP_QUERY_MAX_RESPONSE_TIME_MIN_DSEC) {
    vty_out(vty, "Query max response time %d dsec lower than minimum %d dsec%s",
	    query_max_response_time_dsec,
	    IGMP_QUERY_MAX_RESPONSE_TIME_MIN_DSEC,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }
  if (query_max_response_time_dsec > IGMP_QUERY_MAX_RESPONSE_TIME_MAX_DSEC) {
    vty_out(vty, "Query max response time %d dsec higher than maximum %d dsec%s",
	    query_max_response_time_dsec,
	    IGMP_QUERY_MAX_RESPONSE_TIME_MAX_DSEC,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  default_query_interval_dsec = 10 * pim_ifp->igmp_default_query_interval;

  if (query_max_response_time_dsec >= default_query_interval_dsec) {
    vty_out(vty,
	    "Can't set query max response time %d dsec >= general query interval %d dsec%s",
	    query_max_response_time_dsec, default_query_interval_dsec,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  change_query_max_response_time(pim_ifp, query_max_response_time_dsec);

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_igmp_query_max_response_time_dsec,
       interface_no_ip_igmp_query_max_response_time_dsec_cmd,
       PIM_CMD_NO " " PIM_CMD_IP_IGMP_QUERY_MAX_RESPONSE_TIME_DSEC,
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_DSEC_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  int default_query_interval_dsec;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp)
    return CMD_SUCCESS;

  default_query_interval_dsec = 10 * pim_ifp->igmp_default_query_interval;

  if (IGMP_QUERY_MAX_RESPONSE_TIME_DSEC >= default_query_interval_dsec) {
    vty_out(vty,
	    "Can't set default query max response time %d dsec >= general query interval %d dsec.%s",
	    IGMP_QUERY_MAX_RESPONSE_TIME_DSEC, default_query_interval_dsec,
	    VTY_NEWLINE);
    return CMD_WARNING;
  }

  change_query_max_response_time(pim_ifp, IGMP_QUERY_MAX_RESPONSE_TIME_DSEC);

  return CMD_SUCCESS;
}

DEFUN (interface_ip_pim_drprio,
       interface_ip_pim_drprio_cmd,
       "ip pim drpriority <1-4294967295>",
       IP_STR
       PIM_STR
       "Set the Designated Router Election Priority\n"
       "Value of the new DR Priority\n")
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;
  uint32_t old_dr_prio;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty, "Please enable PIM on interface, first%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  old_dr_prio = pim_ifp->pim_dr_priority;

  pim_ifp->pim_dr_priority = strtol(argv[0], NULL, 10);

  if (old_dr_prio != pim_ifp->pim_dr_priority) {
    if (pim_if_dr_election(ifp))
      pim_hello_restart_now(ifp);
  }

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_pim_drprio,
       interface_no_ip_pim_drprio_cmd,
       "no ip pim drpriority {<1-4294967295>}",
       IP_STR
       PIM_STR
       "Revert the Designated Router Priority to default\n"
       "Old Value of the Priority\n")
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty, "Pim not enabled on this interface%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  if (pim_ifp->pim_dr_priority != PIM_DEFAULT_DR_PRIORITY) {
    pim_ifp->pim_dr_priority = PIM_DEFAULT_DR_PRIORITY;
    if (pim_if_dr_election(ifp))
      pim_hello_restart_now(ifp);
  }

  return CMD_SUCCESS;
}

static int
pim_cmd_interface_add (struct interface *ifp, enum pim_interface_type itype)
{
  struct pim_interface *pim_ifp = ifp->info;
  struct in_addr null = { .s_addr = 0 };

  if (!pim_ifp) {
    pim_ifp = pim_if_new(ifp, 0 /* igmp=false */, 1 /* pim=true */);
    if (!pim_ifp) {
      return 0;
    }
  }
  else {
    PIM_IF_DO_PIM(pim_ifp->options);
  }

  pim_ifp->itype = itype;
  pim_if_addr_add_all(ifp);
  pim_if_membership_refresh(ifp);

  pim_rp_check_rp (null, pim_ifp->primary_address);
  return 1;
}


DEFUN (interface_ip_pim_ssm,
       interface_ip_pim_ssm_cmd,
       "ip pim ssm",
       IP_STR
       PIM_STR
       IFACE_PIM_STR)
{
  struct interface *ifp;

  ifp = vty->index;

  if (!pim_cmd_interface_add(ifp, PIM_INTERFACE_SSM)) {
    vty_out(vty, "Could not enable PIM SSM on interface%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (interface_ip_pim_sm,
       interface_ip_pim_sm_cmd,
       "ip pim sm",
       IP_STR
       PIM_STR
       IFACE_PIM_SM_STR)
{
  struct interface *ifp;

  ifp = vty->index;
  if (!pim_cmd_interface_add(ifp, PIM_INTERFACE_SM)) {
    vty_out(vty, "Could not enable PIM SM on interface%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  pim_if_create_pimreg();

  return CMD_SUCCESS;
}

static int
pim_cmd_interface_delete (struct interface *ifp)
{
  struct pim_interface *pim_ifp = ifp->info;

  if (!pim_ifp)
    return 1;

  PIM_IF_DONT_PIM(pim_ifp->options);

  pim_if_membership_clear(ifp);

  /*
    pim_if_addr_del_all() removes all sockets from
    pim_ifp->igmp_socket_list.
   */
  pim_if_addr_del_all(ifp);

  /*
    pim_sock_delete() removes all neighbors from
    pim_ifp->pim_neighbor_list.
   */
  pim_sock_delete(ifp, "pim unconfigured on interface");

  if (!PIM_IF_TEST_IGMP(pim_ifp->options)) {
    pim_if_delete(ifp);
  }

  return 1;
}

DEFUN (interface_no_ip_pim_ssm,
       interface_no_ip_pim_ssm_cmd,
       "no ip pim ssm",
       NO_STR
       IP_STR
       PIM_STR
       IFACE_PIM_STR)
{
  struct interface *ifp;

  ifp = vty->index;
  if (!pim_cmd_interface_delete(ifp)) {
    vty_out(vty, "Unable to delete interface information%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (interface_no_ip_pim_sm,
       interface_no_ip_pim_sm_cmd,
       "no ip pim sm",
       NO_STR
       IP_STR
       PIM_STR
       IFACE_PIM_SM_STR)
{
  struct interface *ifp;

  ifp = vty->index;
  if (!pim_cmd_interface_delete(ifp)) {
    vty_out(vty, "Unable to delete interface information%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (interface_ip_mroute,
       interface_ip_mroute_cmd,
       "ip mroute INTERFACE A.B.C.D",
       IP_STR
       "Add multicast route\n"
       "Outgoing interface name\n"
       "Group address\n")
{
   struct interface *iif;
   struct interface *oif;
   const char       *oifname;
   const char       *grp_str;
   struct in_addr    grp_addr;
   struct in_addr    src_addr;
   int               result;

   iif = vty->index;

   oifname = argv[0];
   oif = if_lookup_by_name(oifname);
   if (!oif) {
     vty_out(vty, "No such interface name %s%s",
        oifname, VTY_NEWLINE);
     return CMD_WARNING;
   }

   grp_str = argv[1];
   result = inet_pton(AF_INET, grp_str, &grp_addr);
   if (result <= 0) {
     vty_out(vty, "Bad group address %s: errno=%d: %s%s",
        grp_str, errno, safe_strerror(errno), VTY_NEWLINE);
     return CMD_WARNING;
   }

   src_addr.s_addr = INADDR_ANY;

   if (pim_static_add(iif, oif, grp_addr, src_addr)) {
      vty_out(vty, "Failed to add route%s", VTY_NEWLINE);
      return CMD_WARNING;
   }

   return CMD_SUCCESS;
}

DEFUN (interface_ip_mroute_source,
       interface_ip_mroute_source_cmd,
       "ip mroute INTERFACE A.B.C.D A.B.C.D",
       IP_STR
       "Add multicast route\n"
       "Outgoing interface name\n"
       "Group address\n"
       "Source address\n")
{
   struct interface *iif;
   struct interface *oif;
   const char       *oifname;
   const char       *grp_str;
   struct in_addr    grp_addr;
   const char       *src_str;
   struct in_addr    src_addr;
   int               result;

   iif = vty->index;

   oifname = argv[0];
   oif = if_lookup_by_name(oifname);
   if (!oif) {
     vty_out(vty, "No such interface name %s%s",
        oifname, VTY_NEWLINE);
     return CMD_WARNING;
   }

   grp_str = argv[1];
   result = inet_pton(AF_INET, grp_str, &grp_addr);
   if (result <= 0) {
     vty_out(vty, "Bad group address %s: errno=%d: %s%s",
        grp_str, errno, safe_strerror(errno), VTY_NEWLINE);
     return CMD_WARNING;
   }

   src_str = argv[2];
   result = inet_pton(AF_INET, src_str, &src_addr);
   if (result <= 0) {
     vty_out(vty, "Bad source address %s: errno=%d: %s%s",
        src_str, errno, safe_strerror(errno), VTY_NEWLINE);
     return CMD_WARNING;
   }

   if (pim_static_add(iif, oif, grp_addr, src_addr)) {
      vty_out(vty, "Failed to add route%s", VTY_NEWLINE);
      return CMD_WARNING;
   }

   return CMD_SUCCESS;
}

DEFUN (interface_no_ip_mroute,
       interface_no_ip_mroute_cmd,
       "no ip mroute INTERFACE A.B.C.D",
       NO_STR
       IP_STR
       "Add multicast route\n"
       "Outgoing interface name\n"
       "Group Address\n")
{
   struct interface *iif;
   struct interface *oif;
   const char       *oifname;
   const char       *grp_str;
   struct in_addr    grp_addr;
   struct in_addr    src_addr;
   int               result;

   iif = vty->index;

   oifname = argv[0];
   oif = if_lookup_by_name(oifname);
   if (!oif) {
     vty_out(vty, "No such interface name %s%s",
        oifname, VTY_NEWLINE);
     return CMD_WARNING;
   }

   grp_str = argv[1];
   result = inet_pton(AF_INET, grp_str, &grp_addr);
   if (result <= 0) {
     vty_out(vty, "Bad group address %s: errno=%d: %s%s",
        grp_str, errno, safe_strerror(errno), VTY_NEWLINE);
     return CMD_WARNING;
   }

   src_addr.s_addr = INADDR_ANY;

   if (pim_static_del(iif, oif, grp_addr, src_addr)) {
      vty_out(vty, "Failed to remove route%s", VTY_NEWLINE);
      return CMD_WARNING;
   }

   return CMD_SUCCESS;
}

DEFUN (interface_no_ip_mroute_source,
       interface_no_ip_mroute_source_cmd,
       "no ip mroute INTERFACE A.B.C.D A.B.C.D",
       NO_STR
       IP_STR
       "Add multicast route\n"
       "Outgoing interface name\n"
       "Group Address\n"
       "Source Address\n")
{
   struct interface *iif;
   struct interface *oif;
   const char       *oifname;
   const char       *grp_str;
   struct in_addr    grp_addr;
   const char       *src_str;
   struct in_addr    src_addr;
   int               result;

   iif = vty->index;

   oifname = argv[0];
   oif = if_lookup_by_name(oifname);
   if (!oif) {
     vty_out(vty, "No such interface name %s%s",
        oifname, VTY_NEWLINE);
     return CMD_WARNING;
   }

   grp_str = argv[1];
   result = inet_pton(AF_INET, grp_str, &grp_addr);
   if (result <= 0) {
     vty_out(vty, "Bad group address %s: errno=%d: %s%s",
        grp_str, errno, safe_strerror(errno), VTY_NEWLINE);
     return CMD_WARNING;
   }

   src_str = argv[2];
   result = inet_pton(AF_INET, src_str, &src_addr);
   if (result <= 0) {
     vty_out(vty, "Bad source address %s: errno=%d: %s%s",
        src_str, errno, safe_strerror(errno), VTY_NEWLINE);
     return CMD_WARNING;
   }

   if (pim_static_del(iif, oif, grp_addr, src_addr)) {
      vty_out(vty, "Failed to remove route%s", VTY_NEWLINE);
      return CMD_WARNING;
   }

   return CMD_SUCCESS;
}

DEFUN (interface_ip_pim_hello,
       interface_ip_pim_hello_cmd,
       "ip pim hello <1-180>",
       IP_STR
       PIM_STR
       IFACE_PIM_HELLO_STR
       IFACE_PIM_HELLO_TIME_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty, "Pim not enabled on this interface%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  pim_ifp->pim_hello_period = strtol(argv[0], NULL, 10);

  if (argc == 2)
    pim_ifp->pim_default_holdtime = strtol(argv[1], NULL, 10);

  return CMD_SUCCESS;
}

ALIAS (interface_ip_pim_hello,
       interface_ip_pim_hello_hold_cmd,
       "ip pim hello <1-180> <1-180>",
       IP_STR
       PIM_STR
       IFACE_PIM_HELLO_STR
       IFACE_PIM_HELLO_TIME_STR
       IFACE_PIM_HELLO_HOLD_STR)


DEFUN (interface_no_ip_pim_hello,
       interface_no_ip_pim_hello_cmd,
       "no ip pim hello {<1-180> <1-180>}",
       NO_STR
       IP_STR
       PIM_STR
       IFACE_PIM_HELLO_STR
       IFACE_PIM_HELLO_TIME_STR
       IFACE_PIM_HELLO_HOLD_STR)
{
  struct interface *ifp;
  struct pim_interface *pim_ifp;

  ifp = vty->index;
  pim_ifp = ifp->info;

  if (!pim_ifp) {
    vty_out(vty, "Pim not enabled on this interface%s", VTY_NEWLINE);
    return CMD_WARNING;
  }

  pim_ifp->pim_hello_period     = PIM_DEFAULT_HELLO_PERIOD;
  pim_ifp->pim_default_holdtime = -1;

  return CMD_SUCCESS;
}

DEFUN (debug_igmp,
       debug_igmp_cmd,
       "debug igmp",
       DEBUG_STR
       DEBUG_IGMP_STR)
{
  PIM_DO_DEBUG_IGMP_EVENTS;
  PIM_DO_DEBUG_IGMP_PACKETS;
  PIM_DO_DEBUG_IGMP_TRACE;
  return CMD_SUCCESS;
}

DEFUN (no_debug_igmp,
       no_debug_igmp_cmd,
       "no debug igmp",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR)
{
  PIM_DONT_DEBUG_IGMP_EVENTS;
  PIM_DONT_DEBUG_IGMP_PACKETS;
  PIM_DONT_DEBUG_IGMP_TRACE;
  return CMD_SUCCESS;
}

ALIAS (no_debug_igmp,
       undebug_igmp_cmd,
       "undebug igmp",
       UNDEBUG_STR
       DEBUG_IGMP_STR)

DEFUN (debug_igmp_events,
       debug_igmp_events_cmd,
       "debug igmp events",
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_EVENTS_STR)
{
  PIM_DO_DEBUG_IGMP_EVENTS;
  return CMD_SUCCESS;
}

DEFUN (no_debug_igmp_events,
       no_debug_igmp_events_cmd,
       "no debug igmp events",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_EVENTS_STR)
{
  PIM_DONT_DEBUG_IGMP_EVENTS;
  return CMD_SUCCESS;
}

ALIAS (no_debug_igmp_events,
       undebug_igmp_events_cmd,
       "undebug igmp events",
       UNDEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_EVENTS_STR)

DEFUN (debug_igmp_packets,
       debug_igmp_packets_cmd,
       "debug igmp packets",
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_PACKETS_STR)
{
  PIM_DO_DEBUG_IGMP_PACKETS;
  return CMD_SUCCESS;
}

DEFUN (no_debug_igmp_packets,
       no_debug_igmp_packets_cmd,
       "no debug igmp packets",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_PACKETS_STR)
{
  PIM_DONT_DEBUG_IGMP_PACKETS;
  return CMD_SUCCESS;
}

ALIAS (no_debug_igmp_packets,
       undebug_igmp_packets_cmd,
       "undebug igmp packets",
       UNDEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_PACKETS_STR)

DEFUN (debug_igmp_trace,
       debug_igmp_trace_cmd,
       "debug igmp trace",
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_TRACE_STR)
{
  PIM_DO_DEBUG_IGMP_TRACE;
  return CMD_SUCCESS;
}

DEFUN (no_debug_igmp_trace,
       no_debug_igmp_trace_cmd,
       "no debug igmp trace",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_TRACE_STR)
{
  PIM_DONT_DEBUG_IGMP_TRACE;
  return CMD_SUCCESS;
}

ALIAS (no_debug_igmp_trace,
       undebug_igmp_trace_cmd,
       "undebug igmp trace",
       UNDEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_TRACE_STR)

DEFUN (debug_mroute,
       debug_mroute_cmd,
       "debug mroute",
       DEBUG_STR
       DEBUG_MROUTE_STR)
{
  PIM_DO_DEBUG_MROUTE;
  return CMD_SUCCESS;
}

DEFUN (debug_mroute_detail,
       debug_mroute_detail_cmd,
       "debug mroute detail",
       DEBUG_STR
       DEBUG_MROUTE_STR
       "detailed\n")
{
  PIM_DO_DEBUG_MROUTE_DETAIL;
  return CMD_SUCCESS;
}

DEFUN (no_debug_mroute,
       no_debug_mroute_cmd,
       "no debug mroute",
       NO_STR
       DEBUG_STR
       DEBUG_MROUTE_STR)
{
  PIM_DONT_DEBUG_MROUTE;
  return CMD_SUCCESS;
}

DEFUN (no_debug_mroute_detail,
       no_debug_mroute_detail_cmd,
       "no debug mroute detail",
       NO_STR
       DEBUG_STR
       DEBUG_MROUTE_STR
       "detailed\n")
{
  PIM_DONT_DEBUG_MROUTE_DETAIL;
  return CMD_SUCCESS;
}

ALIAS (no_debug_mroute,
       undebug_mroute_cmd,
       "undebug mroute",
       UNDEBUG_STR
       DEBUG_MROUTE_STR)

DEFUN (debug_static,
       debug_static_cmd,
       "debug static",
       DEBUG_STR
       DEBUG_STATIC_STR)
{
  PIM_DO_DEBUG_STATIC;
  return CMD_SUCCESS;
}

DEFUN (no_debug_static,
       no_debug_static_cmd,
       "no debug static",
       NO_STR
       DEBUG_STR
       DEBUG_STATIC_STR)
{
  PIM_DONT_DEBUG_STATIC;
  return CMD_SUCCESS;
}

ALIAS (no_debug_static,
       undebug_static_cmd,
       "undebug static",
       UNDEBUG_STR
       DEBUG_STATIC_STR)

DEFUN (debug_pim,
       debug_pim_cmd,
       "debug pim",
       DEBUG_STR
       DEBUG_PIM_STR)
{
  PIM_DO_DEBUG_PIM_EVENTS;
  PIM_DO_DEBUG_PIM_PACKETS;
  PIM_DO_DEBUG_PIM_TRACE;
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim,
       no_debug_pim_cmd,
       "no debug pim",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR)
{
  PIM_DONT_DEBUG_PIM_EVENTS;
  PIM_DONT_DEBUG_PIM_PACKETS;
  PIM_DONT_DEBUG_PIM_TRACE;

  PIM_DONT_DEBUG_PIM_PACKETDUMP_SEND;
  PIM_DONT_DEBUG_PIM_PACKETDUMP_RECV;

  return CMD_SUCCESS;
}

ALIAS (no_debug_pim,
       undebug_pim_cmd,
       "undebug pim",
       UNDEBUG_STR
       DEBUG_PIM_STR)

DEFUN (debug_pim_events,
       debug_pim_events_cmd,
       "debug pim events",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_EVENTS_STR)
{
  PIM_DO_DEBUG_PIM_EVENTS;
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_events,
       no_debug_pim_events_cmd,
       "no debug pim events",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_EVENTS_STR)
{
  PIM_DONT_DEBUG_PIM_EVENTS;
  return CMD_SUCCESS;
}

ALIAS (no_debug_pim_events,
       undebug_pim_events_cmd,
       "undebug pim events",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_EVENTS_STR)

DEFUN (debug_pim_packets,
       debug_pim_packets_cmd,
       "debug pim packets",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR)
{
    PIM_DO_DEBUG_PIM_PACKETS;
    vty_out (vty, "PIM Packet debugging is on %s", VTY_NEWLINE);
    return CMD_SUCCESS;
}

DEFUN (debug_pim_packets_filter,
       debug_pim_packets_filter_cmd,
       "debug pim packets (hello|joins|register)",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR
       DEBUG_PIM_HELLO_PACKETS_STR
       DEBUG_PIM_J_P_PACKETS_STR
       DEBUG_PIM_PIM_REG_PACKETS_STR)
{
    if (strncmp(argv[0],"h",1) == 0) 
    {
      PIM_DO_DEBUG_PIM_HELLO;
      vty_out (vty, "PIM Hello debugging is on%s", VTY_NEWLINE);
    }
    else if (strncmp(argv[0],"j",1) == 0)
    {
      PIM_DO_DEBUG_PIM_J_P;
      vty_out (vty, "PIM Join/Prune debugging is on%s", VTY_NEWLINE);
    }
    else if (strncmp(argv[0],"r",1) == 0)
    {
      PIM_DO_DEBUG_PIM_REG;
      vty_out (vty, "PIM Register debugging is on%s", VTY_NEWLINE);
    }
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packets,
       no_debug_pim_packets_cmd,
       "no debug pim packets",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR
       DEBUG_PIM_HELLO_PACKETS_STR
       DEBUG_PIM_J_P_PACKETS_STR)
{
  PIM_DONT_DEBUG_PIM_PACKETS;
  vty_out (vty, "PIM Packet debugging is off %s", VTY_NEWLINE);
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packets_filter,
       no_debug_pim_packets_filter_cmd,
       "no debug pim packets (hello|joins|register)",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR
       DEBUG_PIM_HELLO_PACKETS_STR
       DEBUG_PIM_J_P_PACKETS_STR)
{
    if (strncmp(argv[0],"h",1) == 0) 
    {
      PIM_DONT_DEBUG_PIM_HELLO;
      vty_out (vty, "PIM Hello debugging is off %s", VTY_NEWLINE);
    }
    else if (strncmp(argv[0],"j",1) == 0)
    {
      PIM_DONT_DEBUG_PIM_J_P;
      vty_out (vty, "PIM Join/Prune debugging is off %s", VTY_NEWLINE);
    }
    else if (strncmp (argv[0], "r", 1) == 0)
    {
      PIM_DONT_DEBUG_PIM_REG;
      vty_out (vty, "PIM Register debugging is off%s", VTY_NEWLINE);
    }
    return CMD_SUCCESS;
}

ALIAS (no_debug_pim_packets,
       undebug_pim_packets_cmd,
       "undebug pim packets",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR)

DEFUN (debug_pim_packetdump_send,
       debug_pim_packetdump_send_cmd,
       "debug pim packet-dump send",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_SEND_STR)
{
  PIM_DO_DEBUG_PIM_PACKETDUMP_SEND;
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packetdump_send,
       no_debug_pim_packetdump_send_cmd,
       "no debug pim packet-dump send",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_SEND_STR)
{
  PIM_DONT_DEBUG_PIM_PACKETDUMP_SEND;
  return CMD_SUCCESS;
}

ALIAS (no_debug_pim_packetdump_send,
       undebug_pim_packetdump_send_cmd,
       "undebug pim packet-dump send",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_SEND_STR)

DEFUN (debug_pim_packetdump_recv,
       debug_pim_packetdump_recv_cmd,
       "debug pim packet-dump receive",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_RECV_STR)
{
  PIM_DO_DEBUG_PIM_PACKETDUMP_RECV;
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packetdump_recv,
       no_debug_pim_packetdump_recv_cmd,
       "no debug pim packet-dump receive",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_RECV_STR)
{
  PIM_DONT_DEBUG_PIM_PACKETDUMP_RECV;
  return CMD_SUCCESS;
}

ALIAS (no_debug_pim_packetdump_recv,
       undebug_pim_packetdump_recv_cmd,
       "undebug pim packet-dump receive",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_RECV_STR)

DEFUN (debug_pim_trace,
       debug_pim_trace_cmd,
       "debug pim trace",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR)
{
  PIM_DO_DEBUG_PIM_TRACE;
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_trace,
       no_debug_pim_trace_cmd,
       "no debug pim trace",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR)
{
  PIM_DONT_DEBUG_PIM_TRACE;
  return CMD_SUCCESS;
}

ALIAS (no_debug_pim_trace,
       undebug_pim_trace_cmd,
       "undebug pim trace",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR)

DEFUN (debug_ssmpingd,
       debug_ssmpingd_cmd,
       "debug ssmpingd",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_SSMPINGD_STR)
{
  PIM_DO_DEBUG_SSMPINGD;
  return CMD_SUCCESS;
}

DEFUN (no_debug_ssmpingd,
       no_debug_ssmpingd_cmd,
       "no debug ssmpingd",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_SSMPINGD_STR)
{
  PIM_DONT_DEBUG_SSMPINGD;
  return CMD_SUCCESS;
}

ALIAS (no_debug_ssmpingd,
       undebug_ssmpingd_cmd,
       "undebug ssmpingd",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_SSMPINGD_STR)

DEFUN (debug_pim_zebra,
       debug_pim_zebra_cmd,
       "debug pim zebra",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_ZEBRA_STR)
{
  PIM_DO_DEBUG_ZEBRA;
  return CMD_SUCCESS;
}

DEFUN (no_debug_pim_zebra,
       no_debug_pim_zebra_cmd,
       "no debug pim zebra",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_ZEBRA_STR)
{
  PIM_DONT_DEBUG_ZEBRA;
  return CMD_SUCCESS;
}

ALIAS (no_debug_pim_zebra,
       undebug_pim_zebra_cmd,
       "undebug pim zebra",
       UNDEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_ZEBRA_STR)

DEFUN (show_debugging_pim,
       show_debugging_pim_cmd,
       "show debugging pim",
       SHOW_STR
       DEBUG_STR
       PIM_STR)
{
  pim_debug_config_write(vty);
  return CMD_SUCCESS;
}

static struct igmp_sock *find_igmp_sock_by_fd(int fd)
{
  struct listnode  *ifnode;
  struct interface *ifp;

  /* scan all interfaces */
  for (ALL_LIST_ELEMENTS_RO (vrf_iflist (VRF_DEFAULT), ifnode, ifp)) {
    struct pim_interface *pim_ifp;
    struct igmp_sock     *igmp;
    
    if (!ifp->info)
      continue;

    pim_ifp = ifp->info;

    /* lookup igmp socket under current interface */
    igmp = igmp_sock_lookup_by_fd(pim_ifp->igmp_socket_list, fd);
    if (igmp)
      return igmp;
  }

  return 0;
}

DEFUN (test_igmp_receive_report,
       test_igmp_receive_report_cmd,
       "test igmp receive report <0-65535> A.B.C.D <1-6> .LINE",
       "Test\n"
       "Test IGMP protocol\n"
       "Test IGMP message\n"
       "Test IGMP report\n"
       "Socket\n"
       "IGMP group address\n"
       "Record type\n"
       "Sources\n")
{
  char              buf[1000];
  char             *igmp_msg;
  struct ip        *ip_hdr;
  size_t            ip_hlen; /* ip header length in bytes */
  int               ip_msg_len;
  int               igmp_msg_len;
  const char       *socket;
  int               socket_fd;
  const char       *grp_str;
  struct in_addr    grp_addr;
  const char       *record_type_str;
  int               record_type;
  const char       *src_str;
  int               result;
  struct igmp_sock *igmp;
  char             *group_record;
  int               num_sources;
  struct in_addr   *sources;
  struct in_addr   *src_addr;
  int               argi;

  socket = argv[0];
  socket_fd = atoi(socket);
  igmp = find_igmp_sock_by_fd(socket_fd);
  if (!igmp) {
    vty_out(vty, "Could not find IGMP socket %s: fd=%d%s",
	    socket, socket_fd, VTY_NEWLINE);
    return CMD_WARNING;
  }

  grp_str = argv[1];
  result = inet_pton(AF_INET, grp_str, &grp_addr);
  if (result <= 0) {
    vty_out(vty, "Bad group address %s: errno=%d: %s%s",
	    grp_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  record_type_str = argv[2];
  record_type = atoi(record_type_str);

  /*
    Tweak IP header
   */
  ip_hdr = (struct ip *) buf;
  ip_hdr->ip_p = PIM_IP_PROTO_IGMP;
  ip_hlen = PIM_IP_HEADER_MIN_LEN; /* ip header length in bytes */
  ip_hdr->ip_hl = ip_hlen >> 2;    /* ip header length in 4-byte words */
  ip_hdr->ip_src = igmp->ifaddr;
  ip_hdr->ip_dst = igmp->ifaddr;

  /*
    Build IGMP v3 report message
   */
  igmp_msg = buf + ip_hlen;
  group_record = igmp_msg + IGMP_V3_REPORT_GROUPPRECORD_OFFSET;
  *igmp_msg = PIM_IGMP_V3_MEMBERSHIP_REPORT; /* type */
  *(uint16_t *)      (igmp_msg + IGMP_V3_CHECKSUM_OFFSET) = 0; /* for computing checksum */
  *(uint16_t *)      (igmp_msg + IGMP_V3_REPORT_NUMGROUPS_OFFSET) = htons(1); /* one group record */
  *(uint8_t  *)      (group_record + IGMP_V3_GROUP_RECORD_TYPE_OFFSET) = record_type;
  memcpy(group_record + IGMP_V3_GROUP_RECORD_GROUP_OFFSET, &grp_addr, sizeof(struct in_addr));

  /* Scan LINE sources */
  sources = (struct in_addr *) (group_record + IGMP_V3_GROUP_RECORD_SOURCE_OFFSET);
  src_addr = sources;
  for (argi = 3; argi < argc; ++argi,++src_addr) {
    src_str = argv[argi];
    result = inet_pton(AF_INET, src_str, src_addr);
    if (result <= 0) {
      vty_out(vty, "Bad source address %s: errno=%d: %s%s",
	      src_str, errno, safe_strerror(errno), VTY_NEWLINE);
      return CMD_WARNING;
    }
  }
  num_sources = src_addr - sources;

  *(uint16_t *)(group_record + IGMP_V3_GROUP_RECORD_NUMSOURCES_OFFSET) = htons(num_sources);

  igmp_msg_len = IGMP_V3_MSG_MIN_SIZE + (num_sources << 4);   /* v3 report for one single group record */

  /* compute checksum */
  *(uint16_t *)(igmp_msg + IGMP_V3_CHECKSUM_OFFSET) = in_cksum(igmp_msg, igmp_msg_len);

  /* "receive" message */

  ip_msg_len = ip_hlen + igmp_msg_len;
  result = pim_igmp_packet(igmp, buf, ip_msg_len);
  if (result) {
    vty_out(vty, "pim_igmp_packet(len=%d) returned: %d%s",
	    ip_msg_len, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

static int hexval(uint8_t ch)
{
  return isdigit(ch) ? (ch - '0') : (10 + tolower(ch) - 'a');
}

DEFUN (test_pim_receive_dump,
       test_pim_receive_dump_cmd,
       "test pim receive dump INTERFACE A.B.C.D .LINE",
       "Test\n"
       "Test PIM protocol\n"
       "Test PIM message reception\n"
       "Test PIM packet dump reception from neighbor\n"
       "Interface\n"
       "Neighbor address\n"
       "Packet dump\n")
{
  uint8_t           buf[1000];
  uint8_t          *pim_msg;
  struct ip        *ip_hdr;
  size_t            ip_hlen; /* ip header length in bytes */
  int               ip_msg_len;
  int               pim_msg_size;
  const char       *neigh_str;
  struct in_addr    neigh_addr;
  const char       *ifname;
  struct interface *ifp;
  int               argi;
  int               result;

  /* Find interface */
  ifname = argv[0];
  ifp = if_lookup_by_name(ifname);
  if (!ifp) {
    vty_out(vty, "No such interface name %s%s",
	    ifname, VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Neighbor address */
  neigh_str = argv[1];
  result = inet_pton(AF_INET, neigh_str, &neigh_addr);
  if (result <= 0) {
    vty_out(vty, "Bad neighbor address %s: errno=%d: %s%s",
	    neigh_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /*
    Tweak IP header
   */
  ip_hdr = (struct ip *) buf;
  ip_hdr->ip_p = PIM_IP_PROTO_PIM;
  ip_hlen = PIM_IP_HEADER_MIN_LEN; /* ip header length in bytes */
  ip_hdr->ip_hl = ip_hlen >> 2;    /* ip header length in 4-byte words */
  ip_hdr->ip_src = neigh_addr;
  ip_hdr->ip_dst = qpim_all_pim_routers_addr;

  /*
    Build PIM hello message
  */
  pim_msg = buf + ip_hlen;
  pim_msg_size = 0;

  /* Scan LINE dump into buffer */
  for (argi = 2; argi < argc; ++argi) {
    const char *str = argv[argi];
    int str_len = strlen(str);
    int str_last = str_len - 1;
    int i;

    if (str_len % 2) {
      vty_out(vty, "%% Uneven hex array arg %d=%s%s",
	      argi, str, VTY_NEWLINE);
      return CMD_WARNING;
    }

    for (i = 0; i < str_last; i += 2) {
      uint8_t octet;
      int left;
      uint8_t h1 = str[i];
      uint8_t h2 = str[i + 1];

      if (!isxdigit(h1) || !isxdigit(h2)) {
	vty_out(vty, "%% Non-hex octet %c%c at hex array arg %d=%s%s",
		h1, h2, argi, str, VTY_NEWLINE);
	return CMD_WARNING;
      }
      octet = (hexval(h1) << 4) + hexval(h2);

      left = sizeof(buf) - ip_hlen - pim_msg_size;
      if (left < 1) {
	vty_out(vty, "%% Overflow buf_size=%zu buf_left=%d at hex array arg %d=%s octet %02x%s",
		sizeof(buf), left, argi, str, octet, VTY_NEWLINE);
	return CMD_WARNING;
      }
      
      pim_msg[pim_msg_size++] = octet;
    }
  }

  ip_msg_len = ip_hlen + pim_msg_size;

  vty_out(vty, "Receiving: buf_size=%zu ip_msg_size=%d pim_msg_size=%d%s",
	  sizeof(buf), ip_msg_len, pim_msg_size, VTY_NEWLINE);

  /* "receive" message */

  result = pim_pim_packet(ifp, buf, ip_msg_len);
  if (result) {
    vty_out(vty, "%% pim_pim_packet(len=%d) returned failure: %d%s",
	    ip_msg_len, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (test_pim_receive_hello,
       test_pim_receive_hello_cmd,
       "test pim receive hello INTERFACE A.B.C.D <0-65535> <0-65535> <0-65535> <0-32767> <0-65535> <0-1>[LINE]",
       "Test\n"
       "Test PIM protocol\n"
       "Test PIM message reception\n"
       "Test PIM hello reception from neighbor\n"
       "Interface\n"
       "Neighbor address\n"
       "Neighbor holdtime\n"
       "Neighbor DR priority\n"
       "Neighbor generation ID\n"
       "Neighbor propagation delay (msec)\n"
       "Neighbor override interval (msec)\n"
       "Neighbor LAN prune delay T-bit\n"
       "Neighbor secondary addresses\n")
{
  uint8_t           buf[1000];
  uint8_t          *pim_msg;
  struct ip        *ip_hdr;
  size_t            ip_hlen; /* ip header length in bytes */
  int               ip_msg_len;
  int               pim_tlv_size;
  int               pim_msg_size;
  const char       *neigh_str;
  struct in_addr    neigh_addr;
  const char       *ifname;
  struct interface *ifp;
  uint16_t          neigh_holdtime;
  uint16_t          neigh_propagation_delay;
  uint16_t          neigh_override_interval;
  int               neigh_can_disable_join_suppression;
  uint32_t          neigh_dr_priority;
  uint32_t          neigh_generation_id;
  int               argi;
  int               result;

  /* Find interface */
  ifname = argv[0];
  ifp = if_lookup_by_name(ifname);
  if (!ifp) {
    vty_out(vty, "No such interface name %s%s",
	    ifname, VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Neighbor address */
  neigh_str = argv[1];
  result = inet_pton(AF_INET, neigh_str, &neigh_addr);
  if (result <= 0) {
    vty_out(vty, "Bad neighbor address %s: errno=%d: %s%s",
	    neigh_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  neigh_holdtime                     = atoi(argv[2]);
  neigh_dr_priority                  = atoi(argv[3]);
  neigh_generation_id                = atoi(argv[4]);
  neigh_propagation_delay            = atoi(argv[5]);
  neigh_override_interval            = atoi(argv[6]);
  neigh_can_disable_join_suppression = atoi(argv[7]);

  /*
    Tweak IP header
   */
  ip_hdr = (struct ip *) buf;
  ip_hdr->ip_p = PIM_IP_PROTO_PIM;
  ip_hlen = PIM_IP_HEADER_MIN_LEN; /* ip header length in bytes */
  ip_hdr->ip_hl = ip_hlen >> 2;    /* ip header length in 4-byte words */
  ip_hdr->ip_src = neigh_addr;
  ip_hdr->ip_dst = qpim_all_pim_routers_addr;

  /*
    Build PIM hello message
  */
  pim_msg = buf + ip_hlen;

  /* Scan LINE addresses */
  for (argi = 8; argi < argc; ++argi) {
    const char *sec_str = argv[argi];
    struct in_addr sec_addr;
    result = inet_pton(AF_INET, sec_str, &sec_addr);
    if (result <= 0) {
      vty_out(vty, "Bad neighbor secondary address %s: errno=%d: %s%s",
	      sec_str, errno, safe_strerror(errno), VTY_NEWLINE);
      return CMD_WARNING;
    }

    vty_out(vty,
	    "FIXME WRITEME consider neighbor secondary address %s%s",
	    sec_str, VTY_NEWLINE);
  }

  pim_tlv_size = pim_hello_build_tlv(ifp->name,
				     pim_msg + PIM_PIM_MIN_LEN,
				     sizeof(buf) - ip_hlen - PIM_PIM_MIN_LEN,
				     neigh_holdtime,
				     neigh_dr_priority,
				     neigh_generation_id,
				     neigh_propagation_delay,
				     neigh_override_interval,
				     neigh_can_disable_join_suppression,
				     0 /* FIXME secondary address list */);
  if (pim_tlv_size < 0) {
    vty_out(vty, "pim_hello_build_tlv() returned failure: %d%s",
	    pim_tlv_size, VTY_NEWLINE);
    return CMD_WARNING;
  }

  pim_msg_size = pim_tlv_size + PIM_PIM_MIN_LEN;

  pim_msg_build_header(pim_msg, pim_msg_size,
		       PIM_MSG_TYPE_HELLO);

  /* "receive" message */

  ip_msg_len = ip_hlen + pim_msg_size;
  result = pim_pim_packet(ifp, buf, ip_msg_len);
  if (result) {
    vty_out(vty, "pim_pim_packet(len=%d) returned failure: %d%s",
	    ip_msg_len, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (test_pim_receive_assert,
       test_pim_receive_assert_cmd,
       "test pim receive assert INTERFACE A.B.C.D A.B.C.D A.B.C.D <0-65535> <0-65535> <0-1>",
       "Test\n"
       "Test PIM protocol\n"
       "Test PIM message reception\n"
       "Test reception of PIM assert\n"
       "Interface\n"
       "Neighbor address\n"
       "Assert multicast group address\n"
       "Assert unicast source address\n"
       "Assert metric preference\n"
       "Assert route metric\n"
       "Assert RPT bit flag\n")
{
  uint8_t           buf[1000];
  uint8_t          *buf_pastend = buf + sizeof(buf);
  uint8_t          *pim_msg;
  struct ip        *ip_hdr;
  size_t            ip_hlen; /* ip header length in bytes */
  int               ip_msg_len;
  int               pim_msg_size;
  const char       *neigh_str;
  struct in_addr    neigh_addr;
  const char       *group_str;
  struct in_addr    group_addr;
  const char       *source_str;
  struct in_addr    source_addr;
  const char       *ifname;
  struct interface *ifp;
  uint32_t          assert_metric_preference;
  uint32_t          assert_route_metric;
  uint32_t          assert_rpt_bit_flag;
  int               remain;
  int               result;

  /* Find interface */
  ifname = argv[0];
  ifp = if_lookup_by_name(ifname);
  if (!ifp) {
    vty_out(vty, "No such interface name %s%s",
	    ifname, VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Neighbor address */
  neigh_str = argv[1];
  result = inet_pton(AF_INET, neigh_str, &neigh_addr);
  if (result <= 0) {
    vty_out(vty, "Bad neighbor address %s: errno=%d: %s%s",
	    neigh_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Group address */
  group_str = argv[2];
  result = inet_pton(AF_INET, group_str, &group_addr);
  if (result <= 0) {
    vty_out(vty, "Bad group address %s: errno=%d: %s%s",
	    group_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Source address */
  source_str = argv[3];
  result = inet_pton(AF_INET, source_str, &source_addr);
  if (result <= 0) {
    vty_out(vty, "Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  assert_metric_preference = atoi(argv[4]);
  assert_route_metric      = atoi(argv[5]);
  assert_rpt_bit_flag      = atoi(argv[6]);

  remain = buf_pastend - buf;
  if (remain < (int) sizeof(struct ip)) {
    vty_out(vty, "No room for ip header: buf_size=%d < ip_header_size=%zu%s",
	    remain, sizeof(struct ip), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /*
    Tweak IP header
   */
  ip_hdr = (struct ip *) buf;
  ip_hdr->ip_p = PIM_IP_PROTO_PIM;
  ip_hlen = PIM_IP_HEADER_MIN_LEN; /* ip header length in bytes */
  ip_hdr->ip_hl = ip_hlen >> 2;    /* ip header length in 4-byte words */
  ip_hdr->ip_src = neigh_addr;
  ip_hdr->ip_dst = qpim_all_pim_routers_addr;

  /*
    Build PIM assert message
  */
  pim_msg = buf + ip_hlen; /* skip ip header */

  pim_msg_size = pim_assert_build_msg(pim_msg, buf_pastend - pim_msg, ifp,
				      group_addr, source_addr,
				      assert_metric_preference,
				      assert_route_metric,
				      assert_rpt_bit_flag);
  if (pim_msg_size < 0) {
    vty_out(vty, "Failure building PIM assert message: size=%d%s",
	    pim_msg_size, VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* "receive" message */

  ip_msg_len = ip_hlen + pim_msg_size;
  result = pim_pim_packet(ifp, buf, ip_msg_len);
  if (result) {
    vty_out(vty, "pim_pim_packet(len=%d) returned failure: %d%s",
	    ip_msg_len, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

static int recv_joinprune(struct vty *vty,
			  const char *argv[],
			  int src_is_join)
{
  uint8_t           buf[1000];
  const uint8_t    *buf_pastend = buf + sizeof(buf);
  uint8_t          *pim_msg;
  int               pim_msg_size;
  struct ip        *ip_hdr;
  size_t            ip_hlen; /* ip header length in bytes */
  int               ip_msg_len;
  uint16_t          neigh_holdtime;
  const char       *neigh_dst_str;
  struct in_addr    neigh_dst_addr;
  const char       *neigh_src_str;
  struct in_addr    neigh_src_addr;
  const char       *group_str;
  struct in_addr    group_addr;
  const char       *source_str;
  struct in_addr    source_addr;
  const char       *ifname;
  struct interface *ifp;
  int               result;

  /* Find interface */
  ifname = argv[0];
  ifp = if_lookup_by_name(ifname);
  if (!ifp) {
    vty_out(vty, "No such interface name %s%s",
	    ifname, VTY_NEWLINE);
    return CMD_WARNING;
  }

  neigh_holdtime = atoi(argv[1]);

  /* Neighbor destination address */
  neigh_dst_str = argv[2];
  result = inet_pton(AF_INET, neigh_dst_str, &neigh_dst_addr);
  if (result <= 0) {
    vty_out(vty, "Bad neighbor destination address %s: errno=%d: %s%s",
	    neigh_dst_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Neighbor source address */
  neigh_src_str = argv[3];
  result = inet_pton(AF_INET, neigh_src_str, &neigh_src_addr);
  if (result <= 0) {
    vty_out(vty, "Bad neighbor source address %s: errno=%d: %s%s",
	    neigh_src_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Multicast group address */
  group_str = argv[4];
  result = inet_pton(AF_INET, group_str, &group_addr);
  if (result <= 0) {
    vty_out(vty, "Bad group address %s: errno=%d: %s%s",
	    group_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Multicast source address */
  source_str = argv[5];
  result = inet_pton(AF_INET, source_str, &source_addr);
  if (result <= 0) {
    vty_out(vty, "Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /*
    Tweak IP header
   */
  ip_hdr = (struct ip *) buf;
  ip_hdr->ip_p = PIM_IP_PROTO_PIM;
  ip_hlen = PIM_IP_HEADER_MIN_LEN; /* ip header length in bytes */
  ip_hdr->ip_hl = ip_hlen >> 2;    /* ip header length in 4-byte words */
  ip_hdr->ip_src = neigh_src_addr;
  ip_hdr->ip_dst = qpim_all_pim_routers_addr;

  /*
    Build PIM message
  */
  pim_msg = buf + ip_hlen;

  pim_msg_size = pim_msg_join_prune_encode (pim_msg, buf_pastend - pim_msg, src_is_join,
					    source_addr, group_addr, neigh_dst_addr,
					    neigh_holdtime);

  /*
    "Receive" message
  */

  ip_msg_len = ip_hlen + pim_msg_size;
  result = pim_pim_packet(ifp, buf, ip_msg_len);
  if (result) {
    vty_out(vty, "pim_pim_packet(len=%d) returned failure: %d%s",
	    ip_msg_len, result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

DEFUN (test_pim_receive_join,
       test_pim_receive_join_cmd,
       "test pim receive join INTERFACE <0-65535> A.B.C.D A.B.C.D A.B.C.D A.B.C.D",
       "Test\n"
       "Test PIM protocol\n"
       "Test PIM message reception\n"
       "Test PIM join reception from neighbor\n"
       "Interface\n"
       "Neighbor holdtime\n"
       "Upstream neighbor unicast destination address\n"
       "Downstream neighbor unicast source address\n"
       "Multicast group address\n"
       "Unicast source address\n")
{
  return recv_joinprune(vty, argv, 1 /* src_is_join=true */);
}

DEFUN (test_pim_receive_prune,
       test_pim_receive_prune_cmd,
       "test pim receive prune INTERFACE <0-65535> A.B.C.D A.B.C.D A.B.C.D A.B.C.D",
       "Test\n"
       "Test PIM protocol\n"
       "Test PIM message reception\n"
       "Test PIM prune reception from neighbor\n"
       "Interface\n"
       "Neighbor holdtime\n"
       "Upstream neighbor unicast destination address\n"
       "Downstream neighbor unicast source address\n"
       "Multicast group address\n"
       "Unicast source address\n")
{
  return recv_joinprune(vty, argv, 0 /* src_is_join=false */);
}

DEFUN (test_pim_receive_upcall,
       test_pim_receive_upcall_cmd,
       "test pim receive upcall (nocache|wrongvif|wholepkt) <0-65535> A.B.C.D A.B.C.D",
       "Test\n"
       "Test PIM protocol\n"
       "Test PIM message reception\n"
       "Test reception of kernel upcall\n"
       "NOCACHE kernel upcall\n"
       "WRONGVIF kernel upcall\n"
       "WHOLEPKT kernel upcall\n"
       "Input interface vif index\n"
       "Multicast group address\n"
       "Multicast source address\n")
{
  struct igmpmsg msg;
  const char *upcall_type;
  const char *group_str;
  const char *source_str;
  int result;

  upcall_type = argv[0];

  if (upcall_type[0] == 'n')
    msg.im_msgtype = IGMPMSG_NOCACHE;
  else if (upcall_type[1] == 'r')
    msg.im_msgtype = IGMPMSG_WRONGVIF;
  else if (upcall_type[1] == 'h')
    msg.im_msgtype = IGMPMSG_WHOLEPKT;
  else {
    vty_out(vty, "Unknown kernel upcall type: %s%s",
	    upcall_type, VTY_NEWLINE);
    return CMD_WARNING;
  }

  msg.im_vif = atoi(argv[1]);

  /* Group address */
  group_str = argv[2];
  result = inet_pton(AF_INET, group_str, &msg.im_dst);
  if (result <= 0) {
    vty_out(vty, "Bad group address %s: errno=%d: %s%s",
	    group_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  /* Source address */
  source_str = argv[3];
  result = inet_pton(AF_INET, source_str, &msg.im_src);
  if (result <= 0) {
    vty_out(vty, "Bad source address %s: errno=%d: %s%s",
	    source_str, errno, safe_strerror(errno), VTY_NEWLINE);
    return CMD_WARNING;
  }

  msg.im_mbz = 0; /* Must be zero */

  result = pim_mroute_msg(-1, (char *) &msg, sizeof(msg));
  if (result) {
    vty_out(vty, "pim_mroute_msg(len=%zu) returned failure: %d%s",
	    sizeof(msg), result, VTY_NEWLINE);
    return CMD_WARNING;
  }

  return CMD_SUCCESS;
}

void pim_cmd_init()
{
  install_node (&pim_global_node, pim_global_config_write);       /* PIM_NODE */
  install_node (&interface_node, pim_interface_config_write); /* INTERFACE_NODE */

  install_element (CONFIG_NODE, &ip_multicast_routing_cmd);
  install_element (CONFIG_NODE, &no_ip_multicast_routing_cmd);
  install_element (CONFIG_NODE, &ip_pim_rp_cmd);
  install_element (CONFIG_NODE, &ip_pim_rp_range_cmd);
  install_element (CONFIG_NODE, &no_ip_pim_rp_range_cmd);
  install_element (CONFIG_NODE, &ip_pim_rp_prefix_list_cmd);
  install_element (CONFIG_NODE, &no_ip_pim_rp_prefix_list_cmd);
  install_element (CONFIG_NODE, &ip_pim_keep_alive_cmd);
  install_element (CONFIG_NODE, &no_ip_pim_keep_alive_cmd);
  install_element (CONFIG_NODE, &ip_pim_rp_keep_alive_cmd);
  install_element (CONFIG_NODE, &no_ip_pim_rp_keep_alive_cmd);
  install_element (CONFIG_NODE, &ip_ssmpingd_cmd);
  install_element (CONFIG_NODE, &no_ip_ssmpingd_cmd); 
  install_element (CONFIG_NODE, &pim_interface_cmd);
  install_element (CONFIG_NODE, &no_interface_cmd); /* from if.h */

  install_default (INTERFACE_NODE);
  install_element (INTERFACE_NODE, &interface_ip_igmp_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_igmp_cmd); 
  install_element (INTERFACE_NODE, &interface_ip_igmp_join_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_igmp_join_cmd); 
  install_element (INTERFACE_NODE, &interface_ip_igmp_query_interval_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_igmp_query_interval_cmd); 
  install_element (INTERFACE_NODE, &interface_ip_igmp_query_max_response_time_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_igmp_query_max_response_time_cmd); 
  install_element (INTERFACE_NODE, &interface_ip_igmp_query_max_response_time_dsec_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_igmp_query_max_response_time_dsec_cmd); 
  install_element (INTERFACE_NODE, &interface_ip_pim_ssm_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_pim_ssm_cmd);
  install_element (INTERFACE_NODE, &interface_ip_pim_sm_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_pim_sm_cmd);
  install_element (INTERFACE_NODE, &interface_ip_pim_drprio_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_pim_drprio_cmd);
  install_element (INTERFACE_NODE, &interface_ip_pim_hello_cmd);
  install_element (INTERFACE_NODE, &interface_ip_pim_hello_hold_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_pim_hello_cmd);

  // Static mroutes NEB
  install_element (INTERFACE_NODE, &interface_ip_mroute_cmd);
  install_element (INTERFACE_NODE, &interface_ip_mroute_source_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_mroute_cmd);
  install_element (INTERFACE_NODE, &interface_no_ip_mroute_source_cmd);

  install_element (VIEW_NODE, &show_ip_igmp_interface_cmd);
  install_element (VIEW_NODE, &show_ip_igmp_interface_single_cmd);
  install_element (VIEW_NODE, &show_ip_igmp_join_cmd);
  install_element (VIEW_NODE, &show_ip_igmp_groups_cmd);
  install_element (VIEW_NODE, &show_ip_igmp_groups_retransmissions_cmd);
  install_element (VIEW_NODE, &show_ip_igmp_sources_cmd);
  install_element (VIEW_NODE, &show_ip_igmp_sources_retransmissions_cmd);
  install_element (VIEW_NODE, &show_ip_pim_assert_cmd);
  install_element (VIEW_NODE, &show_ip_pim_assert_internal_cmd);
  install_element (VIEW_NODE, &show_ip_pim_assert_metric_cmd);
  install_element (VIEW_NODE, &show_ip_pim_assert_winner_metric_cmd);
  install_element (VIEW_NODE, &show_ip_pim_interface_cmd);
  install_element (VIEW_NODE, &show_ip_pim_interface_single_cmd);
  install_element (VIEW_NODE, &show_ip_pim_join_cmd);
  install_element (VIEW_NODE, &show_ip_pim_local_membership_cmd);
  install_element (VIEW_NODE, &show_ip_pim_neighbor_cmd);
  install_element (VIEW_NODE, &show_ip_pim_neighbor_single_cmd);
  install_element (VIEW_NODE, &show_ip_pim_rpf_cmd);
  install_element (VIEW_NODE, &show_ip_pim_secondary_cmd);
  install_element (VIEW_NODE, &show_ip_pim_upstream_cmd);
  install_element (VIEW_NODE, &show_ip_pim_upstream_join_desired_cmd);
  install_element (VIEW_NODE, &show_ip_pim_upstream_rpf_cmd);
  install_element (VIEW_NODE, &show_ip_pim_rp_cmd);
  install_element (VIEW_NODE, &show_ip_multicast_cmd);
  install_element (VIEW_NODE, &show_ip_mroute_cmd);
  install_element (VIEW_NODE, &show_ip_mroute_count_cmd);
  install_element (VIEW_NODE, &show_ip_rib_cmd);
  install_element (VIEW_NODE, &show_ip_ssmpingd_cmd);
  install_element (VIEW_NODE, &show_debugging_pim_cmd);

  install_element (ENABLE_NODE, &clear_ip_interfaces_cmd);
  install_element (ENABLE_NODE, &clear_ip_igmp_interfaces_cmd);
  install_element (ENABLE_NODE, &clear_ip_mroute_cmd);
  install_element (ENABLE_NODE, &clear_ip_pim_interfaces_cmd);
  install_element (ENABLE_NODE, &clear_ip_pim_oil_cmd);

  install_element (ENABLE_NODE, &show_ip_igmp_interface_cmd);
  install_element (ENABLE_NODE, &show_ip_igmp_interface_single_cmd);
  install_element (ENABLE_NODE, &show_ip_igmp_join_cmd);
  install_element (ENABLE_NODE, &show_ip_igmp_groups_cmd);
  install_element (ENABLE_NODE, &show_ip_igmp_groups_retransmissions_cmd);
  install_element (ENABLE_NODE, &show_ip_igmp_sources_cmd);
  install_element (ENABLE_NODE, &show_ip_igmp_sources_retransmissions_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_assert_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_assert_internal_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_assert_metric_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_assert_winner_metric_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_interface_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_interface_single_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_join_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_local_membership_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_neighbor_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_neighbor_single_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_rpf_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_secondary_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_upstream_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_upstream_join_desired_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_upstream_rpf_cmd);
  install_element (ENABLE_NODE, &show_ip_pim_rp_cmd);
  install_element (ENABLE_NODE, &show_ip_multicast_cmd);
  install_element (ENABLE_NODE, &show_ip_mroute_cmd);
  install_element (ENABLE_NODE, &show_ip_mroute_count_cmd);
  install_element (ENABLE_NODE, &show_ip_rib_cmd);
  install_element (ENABLE_NODE, &show_ip_ssmpingd_cmd);
  install_element (ENABLE_NODE, &show_debugging_pim_cmd);

  install_element (ENABLE_NODE, &test_igmp_receive_report_cmd);
  install_element (ENABLE_NODE, &test_pim_receive_assert_cmd);
  install_element (ENABLE_NODE, &test_pim_receive_dump_cmd);
  install_element (ENABLE_NODE, &test_pim_receive_hello_cmd);
  install_element (ENABLE_NODE, &test_pim_receive_join_cmd);
  install_element (ENABLE_NODE, &test_pim_receive_prune_cmd);
  install_element (ENABLE_NODE, &test_pim_receive_upcall_cmd);

  install_element (ENABLE_NODE, &debug_igmp_cmd);
  install_element (ENABLE_NODE, &no_debug_igmp_cmd);
  install_element (ENABLE_NODE, &undebug_igmp_cmd);
  install_element (ENABLE_NODE, &debug_igmp_events_cmd);
  install_element (ENABLE_NODE, &no_debug_igmp_events_cmd);
  install_element (ENABLE_NODE, &undebug_igmp_events_cmd);
  install_element (ENABLE_NODE, &debug_igmp_packets_cmd);
  install_element (ENABLE_NODE, &no_debug_igmp_packets_cmd);
  install_element (ENABLE_NODE, &undebug_igmp_packets_cmd);
  install_element (ENABLE_NODE, &debug_igmp_trace_cmd);
  install_element (ENABLE_NODE, &no_debug_igmp_trace_cmd);
  install_element (ENABLE_NODE, &undebug_igmp_trace_cmd);
  install_element (ENABLE_NODE, &debug_mroute_cmd);
  install_element (ENABLE_NODE, &debug_mroute_detail_cmd);
  install_element (ENABLE_NODE, &no_debug_mroute_cmd);
  install_element (ENABLE_NODE, &no_debug_mroute_detail_cmd);
  install_element (ENABLE_NODE, &debug_static_cmd);
  install_element (ENABLE_NODE, &no_debug_static_cmd);
  install_element (ENABLE_NODE, &debug_pim_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_cmd);
  install_element (ENABLE_NODE, &undebug_pim_cmd);
  install_element (ENABLE_NODE, &debug_pim_events_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_events_cmd);
  install_element (ENABLE_NODE, &undebug_pim_events_cmd);
  install_element (ENABLE_NODE, &debug_pim_packets_cmd);
  install_element (ENABLE_NODE, &debug_pim_packets_filter_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_packets_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_packets_filter_cmd);
  install_element (ENABLE_NODE, &undebug_pim_packets_cmd);
  install_element (ENABLE_NODE, &debug_pim_packetdump_send_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_packetdump_send_cmd);
  install_element (ENABLE_NODE, &undebug_pim_packetdump_send_cmd);
  install_element (ENABLE_NODE, &debug_pim_packetdump_recv_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_packetdump_recv_cmd);
  install_element (ENABLE_NODE, &undebug_pim_packetdump_recv_cmd);
  install_element (ENABLE_NODE, &debug_pim_trace_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_trace_cmd);
  install_element (ENABLE_NODE, &undebug_pim_trace_cmd);
  install_element (ENABLE_NODE, &debug_ssmpingd_cmd);
  install_element (ENABLE_NODE, &no_debug_ssmpingd_cmd);
  install_element (ENABLE_NODE, &undebug_ssmpingd_cmd);
  install_element (ENABLE_NODE, &debug_pim_zebra_cmd);
  install_element (ENABLE_NODE, &no_debug_pim_zebra_cmd);
  install_element (ENABLE_NODE, &undebug_pim_zebra_cmd);

  install_element (CONFIG_NODE, &debug_igmp_cmd);
  install_element (CONFIG_NODE, &no_debug_igmp_cmd);
  install_element (CONFIG_NODE, &undebug_igmp_cmd);
  install_element (CONFIG_NODE, &debug_igmp_events_cmd);
  install_element (CONFIG_NODE, &no_debug_igmp_events_cmd);
  install_element (CONFIG_NODE, &undebug_igmp_events_cmd);
  install_element (CONFIG_NODE, &debug_igmp_packets_cmd);
  install_element (CONFIG_NODE, &no_debug_igmp_packets_cmd);
  install_element (CONFIG_NODE, &undebug_igmp_packets_cmd);
  install_element (CONFIG_NODE, &debug_igmp_trace_cmd);
  install_element (CONFIG_NODE, &no_debug_igmp_trace_cmd);
  install_element (CONFIG_NODE, &undebug_igmp_trace_cmd);
  install_element (CONFIG_NODE, &debug_mroute_cmd);
  install_element (CONFIG_NODE, &debug_mroute_detail_cmd);
  install_element (CONFIG_NODE, &no_debug_mroute_cmd);
  install_element (CONFIG_NODE, &no_debug_mroute_detail_cmd);
  install_element (CONFIG_NODE, &debug_static_cmd);
  install_element (CONFIG_NODE, &no_debug_static_cmd);
  install_element (CONFIG_NODE, &debug_pim_cmd);
  install_element (CONFIG_NODE, &no_debug_pim_cmd);
  install_element (CONFIG_NODE, &undebug_pim_cmd);
  install_element (CONFIG_NODE, &debug_pim_events_cmd);
  install_element (CONFIG_NODE, &no_debug_pim_events_cmd);
  install_element (CONFIG_NODE, &undebug_pim_events_cmd);
  install_element (CONFIG_NODE, &debug_pim_packets_cmd);
  install_element (CONFIG_NODE, &debug_pim_packets_filter_cmd);
  install_element (CONFIG_NODE, &no_debug_pim_packets_cmd);
  install_element (CONFIG_NODE, &no_debug_pim_packets_filter_cmd);
  install_element (CONFIG_NODE, &undebug_pim_packets_cmd);
  install_element (CONFIG_NODE, &debug_pim_trace_cmd);
  install_element (CONFIG_NODE, &no_debug_pim_trace_cmd);
  install_element (CONFIG_NODE, &undebug_pim_trace_cmd);
  install_element (CONFIG_NODE, &debug_ssmpingd_cmd);
  install_element (CONFIG_NODE, &no_debug_ssmpingd_cmd);
  install_element (CONFIG_NODE, &undebug_ssmpingd_cmd);
  install_element (CONFIG_NODE, &debug_pim_zebra_cmd);
  install_element (CONFIG_NODE, &no_debug_pim_zebra_cmd);
  install_element (CONFIG_NODE, &undebug_pim_zebra_cmd);
}
