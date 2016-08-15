#include <zebra.h>

#include "if.h"
#include "zebra/debug.h"
#include "zebra/zserv.h"
#include "zebra/rib.h"
#include "zebra/zebra_vrf.h"
#include "zebra/zebra_vxlan.h"

int
zebra_vxlan_if_up (struct interface *ifp)
{
  return 0;
}

int
zebra_vxlan_if_down (struct interface *ifp)
{
  return 0;
}

int
zebra_vxlan_if_add (struct interface *ifp, vni_t vni)
{
  return 0;
}

int
zebra_vxlan_if_del (struct interface *ifp)
{
  return 0;
}

int zebra_vxlan_remote_vtep_add (struct zserv *client, int sock,
                                 u_short length, struct zebra_vrf *zvrf)
{
  return 0;
}

int zebra_vxlan_remote_vtep_del (struct zserv *client, int sock,
                                 u_short length, struct zebra_vrf *zvrf)
{
  return 0;
}

int zebra_vxlan_advertise_vni (struct zserv *client, int sock,
                               u_short length, struct zebra_vrf *zvrf)
{
  return 0;
}

void
zebra_evpn_print_vni (struct vty *vty, struct zebra_vrf *zvrf, vni_t vni)
{
}

void
zebra_evpn_print_vnis (struct vty *vty, struct zebra_vrf *zvrf)
{
}

void
zebra_vxlan_init_tables (struct zebra_vrf *zvrf)
{
}

void
zebra_zvni_close (struct zebra_vrf *zvrf)
{
}