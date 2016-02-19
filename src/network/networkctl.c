/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <getopt.h>
#include <net/if.h>
#include <stdbool.h>

#include "sd-device.h"
#include "sd-hwdb.h"
#include "sd-lldp.h"
#include "sd-netlink.h"
#include "sd-network.h"

#include "alloc-util.h"
#include "arphrd-list.h"
#include "device-util.h"
#include "ether-addr-util.h"
#include "fd-util.h"
#include "hwdb-util.h"
#include "lldp.h"
#include "local-addresses.h"
#include "locale-util.h"
#include "netlink-util.h"
#include "pager.h"
#include "parse-util.h"
#include "socket-util.h"
#include "sparse-endian.h"
#include "stdio-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "util.h"
#include "verbs.h"

static bool arg_no_pager = false;
static bool arg_legend = true;
static bool arg_all = false;

static void pager_open_if_enabled(void) {

        if (arg_no_pager)
                return;

        pager_open(false);
}

static int link_get_type_string(unsigned short iftype, sd_device *d, char **ret) {
        const char *t;
        char *p;

        assert(ret);

        if (iftype == ARPHRD_ETHER && d) {
                const char *devtype = NULL, *id = NULL;
                /* WLANs have iftype ARPHRD_ETHER, but we want
                 * to show a more useful type string for
                 * them */

                (void)sd_device_get_devtype(d, &devtype);

                if (streq_ptr(devtype, "wlan"))
                        id = "wlan";
                else if (streq_ptr(devtype, "wwan"))
                        id = "wwan";

                if (id) {
                        p = strdup(id);
                        if (!p)
                                return -ENOMEM;

                        *ret = p;
                        return 1;
                }
        }

        t = arphrd_to_name(iftype);
        if (!t) {
                *ret = NULL;
                return 0;
        }

        p = strdup(t);
        if (!p)
                return -ENOMEM;

        ascii_strlower(p);
        *ret = p;

        return 0;
}

typedef struct LinkInfo {
        const char *name;
        int ifindex;
        unsigned short iftype;
} LinkInfo;

static int link_info_compare(const void *a, const void *b) {
        const LinkInfo *x = a, *y = b;

        return x->ifindex - y->ifindex;
}

static int decode_and_sort_links(sd_netlink_message *m, LinkInfo **ret) {
        _cleanup_free_ LinkInfo *links = NULL;
        size_t size = 0, c = 0;
        sd_netlink_message *i;
        int r;

        for (i = m; i; i = sd_netlink_message_next(i)) {
                const char *name;
                unsigned short iftype;
                uint16_t type;
                int ifindex;

                r = sd_netlink_message_get_type(i, &type);
                if (r < 0)
                        return r;

                if (type != RTM_NEWLINK)
                        continue;

                r = sd_rtnl_message_link_get_ifindex(i, &ifindex);
                if (r < 0)
                        return r;

                r = sd_netlink_message_read_string(i, IFLA_IFNAME, &name);
                if (r < 0)
                        return r;

                r = sd_rtnl_message_link_get_type(i, &iftype);
                if (r < 0)
                        return r;

                if (!GREEDY_REALLOC(links, size, c+1))
                        return -ENOMEM;

                links[c].name = name;
                links[c].ifindex = ifindex;
                links[c].iftype = iftype;
                c++;
        }

        qsort_safe(links, c, sizeof(LinkInfo), link_info_compare);

        *ret = links;
        links = NULL;

        return (int) c;
}

static void operational_state_to_color(const char *state, const char **on, const char **off) {
        assert(on);
        assert(off);

        if (streq_ptr(state, "routable")) {
                *on = ansi_highlight_green();
                *off = ansi_normal();
        } else if (streq_ptr(state, "degraded")) {
                *on = ansi_highlight_yellow();
                *off = ansi_normal();
        } else
                *on = *off = "";
}

static void setup_state_to_color(const char *state, const char **on, const char **off) {
        assert(on);
        assert(off);

        if (streq_ptr(state, "configured")) {
                *on = ansi_highlight_green();
                *off = ansi_normal();
        } else if (streq_ptr(state, "configuring")) {
                *on = ansi_highlight_yellow();
                *off = ansi_normal();
        } else if (streq_ptr(state, "failed") || streq_ptr(state, "linger")) {
                *on = ansi_highlight_red();
                *off = ansi_normal();
        } else
                *on = *off = "";
}

static int list_links(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_free_ LinkInfo *links = NULL;
        int r, c, i;

        pager_open_if_enabled();

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate links: %m");

        if (arg_legend)
                printf("%3s %-16s %-18s %-11s %-10s\n", "IDX", "LINK", "TYPE", "OPERATIONAL", "SETUP");

        c = decode_and_sort_links(reply, &links);
        if (c < 0)
                return rtnl_log_parse_error(c);

        for (i = 0; i < c; i++) {
                _cleanup_free_ char *setup_state = NULL, *operational_state = NULL;
                _cleanup_(sd_device_unrefp) sd_device *d = NULL;
                const char *on_color_operational, *off_color_operational,
                           *on_color_setup, *off_color_setup;
                char devid[2 + DECIMAL_STR_MAX(int)];
                _cleanup_free_ char *t = NULL;

                sd_network_link_get_operational_state(links[i].ifindex, &operational_state);
                operational_state_to_color(operational_state, &on_color_operational, &off_color_operational);

                sd_network_link_get_setup_state(links[i].ifindex, &setup_state);
                setup_state_to_color(setup_state, &on_color_setup, &off_color_setup);

                sprintf(devid, "n%i", links[i].ifindex);
                (void)sd_device_new_from_device_id(&d, devid);

                link_get_type_string(links[i].iftype, d, &t);

                printf("%3i %-16s %-18s %s%-11s%s %s%-10s%s\n",
                       links[i].ifindex, links[i].name, strna(t),
                       on_color_operational, strna(operational_state), off_color_operational,
                       on_color_setup, strna(setup_state), off_color_setup);
        }

        if (arg_legend)
                printf("\n%i links listed.\n", c);

        return 0;
}

/* IEEE Organizationally Unique Identifier vendor string */
static int ieee_oui(sd_hwdb *hwdb, struct ether_addr *mac, char **ret) {
        const char *description;
        char modalias[strlen("OUI:XXYYXXYYXXYY") + 1], *desc;
        int r;

        assert(ret);

        if (!hwdb)
                return -EINVAL;

        if (!mac)
                return -EINVAL;

        /* skip commonly misused 00:00:00 (Xerox) prefix */
        if (memcmp(mac, "\0\0\0", 3) == 0)
                return -EINVAL;

        xsprintf(modalias, "OUI:" ETHER_ADDR_FORMAT_STR,
                 ETHER_ADDR_FORMAT_VAL(*mac));

        r = sd_hwdb_get(hwdb, modalias, "ID_OUI_FROM_DATABASE", &description);
        if (r < 0)
                return r;

        desc = strdup(description);
        if (!desc)
                return -ENOMEM;

        *ret = desc;

        return 0;
}

static int get_gateway_description(
                sd_netlink *rtnl,
                sd_hwdb *hwdb,
                int ifindex,
                int family,
                union in_addr_union *gateway,
                char **gateway_description) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *m;
        int r;

        assert(rtnl);
        assert(ifindex >= 0);
        assert(family == AF_INET || family == AF_INET6);
        assert(gateway);
        assert(gateway_description);

        r = sd_rtnl_message_new_neigh(rtnl, &req, RTM_GETNEIGH, ifindex, family);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (m = reply; m; m = sd_netlink_message_next(m)) {
                union in_addr_union gw = {};
                struct ether_addr mac = {};
                uint16_t type;
                int ifi, fam;

                r = sd_netlink_message_get_errno(m);
                if (r < 0) {
                        log_error_errno(r, "got error: %m");
                        continue;
                }

                r = sd_netlink_message_get_type(m, &type);
                if (r < 0) {
                        log_error_errno(r, "could not get type: %m");
                        continue;
                }

                if (type != RTM_NEWNEIGH) {
                        log_error("type is not RTM_NEWNEIGH");
                        continue;
                }

                r = sd_rtnl_message_neigh_get_family(m, &fam);
                if (r < 0) {
                        log_error_errno(r, "could not get family: %m");
                        continue;
                }

                if (fam != family) {
                        log_error("family is not correct");
                        continue;
                }

                r = sd_rtnl_message_neigh_get_ifindex(m, &ifi);
                if (r < 0) {
                        log_error_errno(r, "could not get ifindex: %m");
                        continue;
                }

                if (ifindex > 0 && ifi != ifindex)
                        continue;

                switch (fam) {
                case AF_INET:
                        r = sd_netlink_message_read_in_addr(m, NDA_DST, &gw.in);
                        if (r < 0)
                                continue;

                        break;
                case AF_INET6:
                        r = sd_netlink_message_read_in6_addr(m, NDA_DST, &gw.in6);
                        if (r < 0)
                                continue;

                        break;
                default:
                        continue;
                }

                if (!in_addr_equal(fam, &gw, gateway))
                        continue;

                r = sd_netlink_message_read_ether_addr(m, NDA_LLADDR, &mac);
                if (r < 0)
                        continue;

                r = ieee_oui(hwdb, &mac, gateway_description);
                if (r < 0)
                        continue;

                return 0;
        }

        return -ENODATA;
}

static int dump_gateways(
                sd_netlink *rtnl,
                sd_hwdb *hwdb,
                const char *prefix,
                int ifindex) {
        _cleanup_free_ struct local_address *local = NULL;
        int r, n, i;

        n = local_gateways(rtnl, ifindex, AF_UNSPEC, &local);
        if (n < 0)
                return n;

        for (i = 0; i < n; i++) {
                _cleanup_free_ char *gateway = NULL, *description = NULL;

                r = in_addr_to_string(local[i].family, &local[i].address, &gateway);
                if (r < 0)
                        return r;

                r = get_gateway_description(rtnl, hwdb, local[i].ifindex, local[i].family, &local[i].address, &description);
                if (r < 0)
                        log_debug_errno(r, "Could not get description of gateway: %m");

                printf("%*s%s",
                       (int) strlen(prefix),
                       i == 0 ? prefix : "",
                       gateway);

                if (description)
                        printf(" (%s)", description);

                /* Show interface name for the entry if we show
                 * entries for all interfaces */
                if (ifindex <= 0) {
                        char name[IF_NAMESIZE+1];

                        if (if_indextoname(local[i].ifindex, name)) {
                                fputs(" on ", stdout);
                                fputs(name, stdout);
                        } else
                                printf(" on %%%i", local[i].ifindex);
                }

                fputc('\n', stdout);
        }

        return 0;
}

static int dump_addresses(
                sd_netlink *rtnl,
                const char *prefix,
                int ifindex) {

        _cleanup_free_ struct local_address *local = NULL;
        int r, n, i;

        n = local_addresses(rtnl, ifindex, AF_UNSPEC, &local);
        if (n < 0)
                return n;

        for (i = 0; i < n; i++) {
                _cleanup_free_ char *pretty = NULL;

                r = in_addr_to_string(local[i].family, &local[i].address, &pretty);
                if (r < 0)
                        return r;

                printf("%*s%s",
                       (int) strlen(prefix),
                       i == 0 ? prefix : "",
                       pretty);

                if (ifindex <= 0) {
                        char name[IF_NAMESIZE+1];

                        if (if_indextoname(local[i].ifindex, name)) {
                                fputs(" on ", stdout);
                                fputs(name, stdout);
                        } else
                                printf(" on %%%i", local[i].ifindex);
                }

                fputc('\n', stdout);
        }

        return 0;
}

static void dump_list(const char *prefix, char **l) {
        char **i;

        if (strv_isempty(l))
                return;

        STRV_FOREACH(i, l) {
                printf("%*s%s\n",
                       (int) strlen(prefix),
                       i == l ? prefix : "",
                       *i);
        }
}

static int link_status_one(
                sd_netlink *rtnl,
                sd_hwdb *hwdb,
                const char *name) {
        _cleanup_strv_free_ char **dns = NULL, **ntp = NULL, **search_domains = NULL, **route_domains = NULL;
        _cleanup_free_ char *setup_state = NULL, *operational_state = NULL, *tz = NULL;
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_(sd_device_unrefp) sd_device *d = NULL;
        char devid[2 + DECIMAL_STR_MAX(int)];
        _cleanup_free_ char *t = NULL, *network = NULL;
        const char *driver = NULL, *path = NULL, *vendor = NULL, *model = NULL, *link = NULL;
        const char *on_color_operational, *off_color_operational,
                   *on_color_setup, *off_color_setup;
        _cleanup_strv_free_ char **carrier_bound_to = NULL;
        _cleanup_strv_free_ char **carrier_bound_by = NULL;
        struct ether_addr e;
        unsigned short iftype;
        int r, ifindex;
        bool have_mac;
        uint32_t mtu;

        assert(rtnl);
        assert(name);

        if (parse_ifindex(name, &ifindex) >= 0)
                r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, ifindex);
        else {
                r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
                if (r < 0)
                        return rtnl_log_create_error(r);

                r = sd_netlink_message_append_string(req, IFLA_IFNAME, name);
        }

        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to query link: %m");

        r = sd_rtnl_message_link_get_ifindex(reply, &ifindex);
        if (r < 0)
                return rtnl_log_parse_error(r);

        r = sd_netlink_message_read_string(reply, IFLA_IFNAME, &name);
        if (r < 0)
                return rtnl_log_parse_error(r);

        r = sd_rtnl_message_link_get_type(reply, &iftype);
        if (r < 0)
                return rtnl_log_parse_error(r);

        have_mac = sd_netlink_message_read_ether_addr(reply, IFLA_ADDRESS, &e) >= 0;
        if (have_mac) {
                const uint8_t *p;
                bool all_zeroes = true;

                for (p = (uint8_t*) &e; p < (uint8_t*) &e + sizeof(e); p++)
                        if (*p != 0) {
                                all_zeroes = false;
                                break;
                        }

                if (all_zeroes)
                        have_mac = false;
        }

        (void) sd_netlink_message_read_u32(reply, IFLA_MTU, &mtu);

        (void) sd_network_link_get_operational_state(ifindex, &operational_state);
        operational_state_to_color(operational_state, &on_color_operational, &off_color_operational);

        (void) sd_network_link_get_setup_state(ifindex, &setup_state);
        setup_state_to_color(setup_state, &on_color_setup, &off_color_setup);

        (void) sd_network_link_get_dns(ifindex, &dns);
        (void) sd_network_link_get_search_domains(ifindex, &search_domains);
        (void) sd_network_link_get_route_domains(ifindex, &route_domains);
        (void) sd_network_link_get_ntp(ifindex, &ntp);

        sprintf(devid, "n%i", ifindex);

        (void) sd_device_new_from_device_id(&d, devid);

        if (d) {
                (void) sd_device_get_property_value(d, "ID_NET_LINK_FILE", &link);
                (void) sd_device_get_property_value(d, "ID_NET_DRIVER", &driver);
                (void) sd_device_get_property_value(d, "ID_PATH", &path);

                r = sd_device_get_property_value(d, "ID_VENDOR_FROM_DATABASE", &vendor);
                if (r < 0)
                        (void) sd_device_get_property_value(d, "ID_VENDOR", &vendor);

                r = sd_device_get_property_value(d, "ID_MODEL_FROM_DATABASE", &model);
                if (r < 0)
                        (void) sd_device_get_property_value(d, "ID_MODEL", &model);
        }

        link_get_type_string(iftype, d, &t);

        sd_network_link_get_network_file(ifindex, &network);

        sd_network_link_get_carrier_bound_to(ifindex, &carrier_bound_to);
        sd_network_link_get_carrier_bound_by(ifindex, &carrier_bound_by);

        printf("%s%s%s %i: %s\n", on_color_operational, draw_special_char(DRAW_BLACK_CIRCLE), off_color_operational, ifindex, name);

        printf("       Link File: %s\n"
               "    Network File: %s\n"
               "            Type: %s\n"
               "           State: %s%s%s (%s%s%s)\n",
               strna(link),
               strna(network),
               strna(t),
               on_color_operational, strna(operational_state), off_color_operational,
               on_color_setup, strna(setup_state), off_color_setup);

        if (path)
                printf("            Path: %s\n", path);
        if (driver)
                printf("          Driver: %s\n", driver);
        if (vendor)
                printf("          Vendor: %s\n", vendor);
        if (model)
                printf("           Model: %s\n", model);

        if (have_mac) {
                _cleanup_free_ char *description = NULL;
                char ea[ETHER_ADDR_TO_STRING_MAX];

                ieee_oui(hwdb, &e, &description);

                if (description)
                        printf("      HW Address: %s (%s)\n", ether_addr_to_string(&e, ea), description);
                else
                        printf("      HW Address: %s\n", ether_addr_to_string(&e, ea));
        }

        if (mtu > 0)
                printf("             MTU: %u\n", mtu);

        dump_addresses(rtnl, "         Address: ", ifindex);
        dump_gateways(rtnl, hwdb, "         Gateway: ", ifindex);

        dump_list("             DNS: ", dns);
        dump_list("  Search Domains: ", search_domains);
        dump_list("   Route Domains: ", route_domains);

        dump_list("             NTP: ", ntp);

        dump_list("Carrier Bound To: ", carrier_bound_to);
        dump_list("Carrier Bound By: ", carrier_bound_by);

        (void) sd_network_link_get_timezone(ifindex, &tz);
        if (tz)
                printf("       Time Zone: %s", tz);

        return 0;
}

static int system_status(sd_netlink *rtnl, sd_hwdb *hwdb) {
        _cleanup_free_ char *operational_state = NULL;
        _cleanup_strv_free_ char **dns = NULL, **ntp = NULL, **search_domains = NULL, **route_domains = NULL;
        const char *on_color_operational, *off_color_operational;

        assert(rtnl);

        sd_network_get_operational_state(&operational_state);
        operational_state_to_color(operational_state, &on_color_operational, &off_color_operational);

        printf("%s%s%s        State: %s%s%s\n",
               on_color_operational, draw_special_char(DRAW_BLACK_CIRCLE), off_color_operational,
               on_color_operational, strna(operational_state), off_color_operational);

        dump_addresses(rtnl, "       Address: ", 0);
        dump_gateways(rtnl, hwdb, "       Gateway: ", 0);

        sd_network_get_dns(&dns);
        dump_list("           DNS: ", dns);

        sd_network_get_search_domains(&search_domains);
        dump_list("Search Domains: ", search_domains);

        sd_network_get_route_domains(&route_domains);
        dump_list(" Route Domains: ", route_domains);

        sd_network_get_ntp(&ntp);
        dump_list("           NTP: ", ntp);

        return 0;
}

static int link_status(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        char **name;
        int r;

        pager_open_if_enabled();

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_hwdb_new(&hwdb);
        if (r < 0)
                log_debug_errno(r, "Failed to open hardware database: %m");

        if (argc <= 1 && !arg_all)
                return system_status(rtnl, hwdb);

        if (arg_all) {
                _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
                _cleanup_free_ LinkInfo *links = NULL;
                int c, i;

                r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
                if (r < 0)
                        return rtnl_log_create_error(r);

                r = sd_netlink_message_request_dump(req, true);
                if (r < 0)
                        return rtnl_log_create_error(r);

                r = sd_netlink_call(rtnl, req, 0, &reply);
                if (r < 0)
                        return log_error_errno(r, "Failed to enumerate links: %m");

                c = decode_and_sort_links(reply, &links);
                if (c < 0)
                        return rtnl_log_parse_error(c);

                for (i = 0; i < c; i++) {
                        if (i > 0)
                                fputc('\n', stdout);

                        link_status_one(rtnl, hwdb, links[i].name);
                }
        } else {
                STRV_FOREACH(name, argv + 1) {
                        if (name != argv + 1)
                                fputc('\n', stdout);

                        link_status_one(rtnl, hwdb, *name);
                }
        }

        return 0;
}

static char *lldp_capabilities_to_string(uint16_t x) {
        static const char characters[] = {
                'o', 'p', 'b', 'w', 'r', 't', 'd', 'a', 'c', 's', 'm',
        };
        char *ret;
        unsigned i;

        ret = new(char, ELEMENTSOF(characters) + 1);
        if (!ret)
                return NULL;

        for (i = 0; i < ELEMENTSOF(characters); i++)
                ret[i] = (x & (1U << i)) ? characters[i] : '.';

        ret[i] = 0;
        return ret;
}

static int link_lldp_status(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_free_ LinkInfo *links = NULL;
        int i, r, c, j;

        pager_open_if_enabled();

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate links: %m");

        c = decode_and_sort_links(reply, &links);
        if (c < 0)
                return rtnl_log_parse_error(c);

        if (arg_legend)
                printf("%-16s %-17s %-16s %-11s %-17s %-16s\n",
                       "LINK",
                       "CHASSIS ID",
                       "SYSTEM NAME",
                       "CAPS",
                       "PORT ID",
                       "PORT DESCRIPTION");

        for (i = j = 0; i < c; i++) {
                _cleanup_fclose_ FILE *f = NULL;
                _cleanup_free_ char *p = NULL;

                if (asprintf(&p, "/run/systemd/netif/lldp/%i", links[i].ifindex) < 0)
                        return log_oom();

                f = fopen(p, "re");
                if (!f) {
                        if (errno == ENOENT)
                                continue;

                        log_warning_errno(errno, "Failed to open %s, ignoring: %m", p);
                        continue;
                }

                for (;;) {
                        const char *chassis_id = NULL, *port_id = NULL, *system_name = NULL, *port_description = NULL, *capabilities = NULL;
                        _cleanup_(sd_lldp_neighbor_unrefp) sd_lldp_neighbor *n = NULL;
                        _cleanup_free_ void *raw = NULL;
                        uint16_t cc;
                        le64_t u;
                        size_t l;

                        l = fread(&u, 1, sizeof(u), f);
                        if (l == 0 && feof(f)) /* EOF */
                                break;
                        if (l != sizeof(u)) {
                                log_warning("Premature end of file, ignoring.");
                                break;
                        }

                        raw = new(uint8_t, le64toh(u));
                        if (!raw)
                                return log_oom();

                        if (fread(raw, 1, le64toh(u), f) != le64toh(u)) {
                                log_warning("Premature end of file, ignoring.");
                                break;
                        }

                        r = sd_lldp_neighbor_from_raw(&n, raw, le64toh(u));
                        if (r < 0) {
                                log_warning_errno(r, "Failed to parse LLDP data, ignoring: %m");
                                break;
                        }

                        (void) sd_lldp_neighbor_get_chassis_id_as_string(n, &chassis_id);
                        (void) sd_lldp_neighbor_get_port_id_as_string(n, &port_id);
                        (void) sd_lldp_neighbor_get_system_name(n, &system_name);
                        (void) sd_lldp_neighbor_get_port_description(n, &port_description);

                        if (sd_lldp_neighbor_get_enabled_capabilities(n, &cc) >= 0)
                                capabilities = lldp_capabilities_to_string(cc);

                        printf("%-16s %-17s %-16s %-11s %-17s %-16s\n",
                               links[i].name,
                               strna(chassis_id),
                               strna(system_name),
                               strna(capabilities),
                               strna(port_id),
                               strna(port_description));
                }
        }

        if (arg_legend)
                printf("\nCapabilities:\n"
                       "o - Other; p - Repeater;  b - Bridge; w - WLAN Access Point; r - Router;\n"
                       "t - Telephone; d - DOCSIS cable device; a - Station; c - Customer VLAN;\n"
                       "s - Service VLAN, m - Two-port MAC Relay (TPMR)\n\n"
                       "Total entries displayed: %i\n", j);

        return 0;
}

static void help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Query and control the networking subsystem.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "     --no-pager         Do not pipe output into a pager\n"
               "     --no-legend        Do not show the headers and footers\n"
               "  -a --all              Show status for all links\n\n"
               "Commands:\n"
               "  list                  List links\n"
               "  status [LINK...]      Show link status\n"
               "  lldp                  Show lldp neighbors\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_NO_LEGEND,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                { "no-pager",  no_argument,       NULL, ARG_NO_PAGER  },
                { "no-legend", no_argument,       NULL, ARG_NO_LEGEND },
                { "all",       no_argument,       NULL, 'a'           },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "ha", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case ARG_NO_LEGEND:
                        arg_legend = false;
                        break;

                case 'a':
                        arg_all = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

static int networkctl_main(int argc, char *argv[]) {
        const Verb verbs[] = {
                { "list", VERB_ANY, 1, VERB_DEFAULT, list_links },
                { "status", 1, VERB_ANY, 0, link_status },
                { "lldp", VERB_ANY, 1, VERB_DEFAULT, link_lldp_status },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

int main(int argc, char* argv[]) {
        int r;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = networkctl_main(argc, argv);

finish:
        pager_close();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
