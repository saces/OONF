
/*
 * The olsr.org Optimized Link-State Routing daemon version 2 (olsrd2)
 * Copyright (c) 2004-2015, the olsr.org team - see HISTORY file
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

#include <errno.h>

#include "common/avl.h"
#include "common/avl_comp.h"
#include "common/common_types.h"
#include "common/list.h"
#include "common/netaddr.h"
#include "core/oonf_logging.h"
#include "subsystems/oonf_class.h"
#include "subsystems/oonf_rfc5444.h"
#include "subsystems/oonf_timer.h"

#include "nhdp/nhdp_db.h"
#include "nhdp/nhdp_domain.h"

#include "olsrv2/olsrv2_internal.h"
#include "olsrv2/olsrv2_originator.h"
#include "olsrv2/olsrv2_tc.h"
#include "olsrv2/olsrv2_routing.h"
#include "olsrv2/olsrv2.h"

/* Prototypes */
static struct olsrv2_routing_entry *_add_entry(
    struct nhdp_domain *, struct netaddr *prefix);
static void _remove_entry(struct olsrv2_routing_entry *);
static void _insert_into_working_tree(struct olsrv2_tc_target *target,
    struct nhdp_neighbor *neigh, uint32_t linkcost,
    uint32_t pathcost, uint8_t distance, bool single_hop);
static void _prepare_routes(struct nhdp_domain *);
static void _handle_working_queue(struct nhdp_domain *);
static void _handle_nhdp_routes(struct nhdp_domain *);
static void _add_route_to_kernel_queue(struct olsrv2_routing_entry *rtentry);
static void _process_dijkstra_result(struct nhdp_domain *);
static void _process_kernel_queue(void);
static void _cb_trigger_dijkstra(void *);
static void _cb_nhdp_update(struct nhdp_neighbor *);
static void _cb_route_new_finished(struct os_route *route, int error);
static void _cb_route_current_finished(struct os_route *route, int error);

/* Domain parameter of dijkstra algorithm */
static struct olsrv2_routing_domain _domain_parameter[NHDP_MAXIMUM_DOMAINS];

/* memory class for routing entries */
static struct oonf_class _rtset_entry = {
  .name = "Olsrv2 Routing Set Entry",
  .size = sizeof(struct olsrv2_routing_entry),
};

/* rate limitation for dijkstra algorithm */
static struct oonf_timer_class _dijkstra_timer_info = {
  .name = "Dijkstra rate limit timer",
  .callback = _cb_trigger_dijkstra,
};

static struct oonf_timer_instance _rate_limit_timer = {
  .class = &_dijkstra_timer_info
};

/* callback for NHDP domain events */
static struct nhdp_domain_listener _nhdp_listener = {
  .update = _cb_nhdp_update,
};

static bool _trigger_dijkstra = false;

/* global datastructures for routing */
static struct avl_tree _routing_tree[NHDP_MAXIMUM_DOMAINS];
static struct list_entity _routing_filter_list;

static struct avl_tree _dijkstra_working_tree;
static struct list_entity _kernel_queue;

static bool _initiate_shutdown = false;

/**
 * Initialize olsrv2 dijkstra and routing code
 */
void
olsrv2_routing_init(void) {
  int i;

  oonf_class_add(&_rtset_entry);
  oonf_timer_add(&_dijkstra_timer_info);

  for (i=0; i<NHDP_MAXIMUM_DOMAINS; i++) {
    avl_init(&_routing_tree[i], avl_comp_netaddr, false);
  }
  list_init_head(&_routing_filter_list);
  avl_init(&_dijkstra_working_tree, avl_comp_uint32, true);
  list_init_head(&_kernel_queue);

  nhdp_domain_listener_add(&_nhdp_listener);
}

/**
 * Trigger cleanup of olsrv2 dijkstra and routing code
 */
void
olsrv2_routing_initiate_shutdown(void) {
  struct olsrv2_routing_entry *entry, *e_it;
  int i;

  /* remember we are in shutdown */
  _initiate_shutdown = true;

  /* remove all routes */
  for (i=0; i<NHDP_MAXIMUM_DOMAINS; i++) {
    avl_for_each_element_safe(&_routing_tree[i], entry, _node, e_it) {
      if (entry->state_current) {
        entry->state_new = false;
        _add_route_to_kernel_queue(entry);
      }
    }
  }

  _process_kernel_queue();
}

/**
 * Finalize cleanup of olsrv2 dijkstra and routing code
 */
void
olsrv2_routing_cleanup(void) {
  struct olsrv2_routing_entry *entry, *e_it;
  struct olsrv2_routing_filter *filter, *f_it;
  int i;

  nhdp_domain_listener_remove(&_nhdp_listener);

  oonf_timer_stop(&_rate_limit_timer);

  for (i=0; i<NHDP_MAXIMUM_DOMAINS; i++) {
    avl_for_each_element_safe(&_routing_tree[i], entry, _node, e_it) {
      /* make sure route processing has stopped */
      entry->route_new.cb_finished = NULL;
      os_routing_interrupt(&entry->route_new);

      entry->route_current.cb_finished = NULL;
      os_routing_interrupt(&entry->route_current);

      /* remove entry from database */
      _remove_entry(entry);
    }
  }

  list_for_each_element_safe(&_routing_filter_list, filter, _node, f_it) {
    olsrv2_routing_filter_remove(filter);
  }

  oonf_timer_remove(&_dijkstra_timer_info);
  oonf_class_remove(&_rtset_entry);
}

/**
 * Trigger a new dijkstra as soon as we are back in the mainloop
 * (unless the rate limitation timer is active, then we will wait for it)
 */
void
olsrv2_routing_trigger_update(void) {
  _trigger_dijkstra = true;
  if (!oonf_timer_is_active(&_rate_limit_timer)) {
    /* trigger as soon as we hit the next time slice */
    oonf_timer_set(&_rate_limit_timer, 1);
  }

  OONF_DEBUG(LOG_OLSRV2_ROUTING, "Trigger routing update");
}

/**
 * Trigger dijkstra and routing update now
 * @param skip_wait true to ignore rate limitation timer
 */
void
olsrv2_routing_force_update(bool skip_wait) {
  struct nhdp_domain *domain;

  if (_initiate_shutdown) {
    /* no dijkstra anymore when in shutdown */
    return;
  }

  /* handle dijkstra rate limitation timer */
  if (oonf_timer_is_active(&_rate_limit_timer)) {
    if (!skip_wait) {
      /* trigger dijkstra later */
      _trigger_dijkstra = true;

      OONF_DEBUG(LOG_OLSRV2_ROUTING, "Delay Dijkstra");
      return;
    }
    oonf_timer_stop(&_rate_limit_timer);
  }


  OONF_DEBUG(LOG_OLSRV2_ROUTING, "Run Dijkstra");

  list_for_each_element(nhdp_domain_get_list(), domain, _node) {
    /* initialize dijkstra specific fields */
    _prepare_routes(domain);

    /* run dijkstra */
    while (!avl_is_empty(&_dijkstra_working_tree)) {
      _handle_working_queue(domain);
    }

    /* check if direct one-hop routes are quicker */
    _handle_nhdp_routes(domain);

    /* update kernel routes */
    _process_dijkstra_result(domain);
  }

  _process_kernel_queue();

  /* make sure dijkstra is not called too often */
  oonf_timer_set(&_rate_limit_timer, OLSRv2_DIJKSTRA_RATE_LIMITATION);
}

/**
 * Initialize the dijkstra code part of a tc node.
 * Should normally not be called by other parts of OLSRv2.
 * @param dijkstra pointer to dijkstra node
 */
void
olsrv2_routing_dijkstra_node_init(struct olsrv2_dijkstra_node *dijkstra) {
  dijkstra->_node.key = &dijkstra->path_cost;
}

/**
 * Set the domain parameters of olsrv2
 * @param domain pointer to NHDP domain
 * @param parameter pointer to new parameters
 */
void
olsrv2_routing_set_domain_parameter(struct nhdp_domain *domain,
    struct olsrv2_routing_domain *parameter) {
  struct olsrv2_routing_entry *rtentry;

  if (memcmp(parameter, &_domain_parameter[domain->index],
      sizeof(*parameter)) == 0) {
    /* no change */
    return;
  }

  /* copy parameters */
  memcpy(&_domain_parameter[domain->index], parameter, sizeof(*parameter));

  if (avl_is_empty(&_routing_tree[domain->index])) {
    /* no routes present */
    return;
  }

  /* remove old kernel routes */
  avl_for_each_element(&_routing_tree[domain->index], rtentry, _node) {
    if (rtentry->state_current) {
      if (rtentry->in_processing) {
        /* stop current processing */
        os_routing_interrupt(&rtentry->route_new);
        os_routing_interrupt(&rtentry->route_current);
      }

      /* remove current route */
      rtentry->state_new = false;
      _add_route_to_kernel_queue(rtentry);
    }
  }

  _process_kernel_queue();

  /* trigger a dijkstra to write new routes in 100 milliseconds */
  oonf_timer_set(&_rate_limit_timer, 100);
  _trigger_dijkstra = true;
}

struct avl_tree *
olsrv2_routing_get_tree(size_t idx) {
  return &_routing_tree[idx];
}

struct list_entity *
olsrv2_routing_get_filter_list(void) {
  return &_routing_filter_list;
}

/**
 * Add a new routing entry to the database
 * @param domain pointer to nhdp domain
 * @param prefix network prefix of routing entry
 * @return pointer to routing entry, NULL if our of memory.
 */
static struct olsrv2_routing_entry *
_add_entry(struct nhdp_domain *domain, struct netaddr *prefix) {
  struct olsrv2_routing_entry *rtentry;

  rtentry = avl_find_element(
      &_routing_tree[domain->index], prefix, rtentry, _node);
  if (rtentry) {
    return rtentry;
  }

  rtentry = oonf_class_malloc(&_rtset_entry);
  if (rtentry == NULL) {
    return NULL;
  }

  /* set key */
  memcpy(&rtentry->route_new.dst, prefix, sizeof(struct netaddr));
  memcpy(&rtentry->route_current.dst, prefix, sizeof(struct netaddr));
  rtentry->_node.key = &rtentry->route_new.dst;

  /* set domain */
  rtentry->domain = domain;

  /* initialize path costs and os-route callback */
  rtentry->cost = RFC7181_METRIC_INFINITE_PATH;
  rtentry->route_new.cb_finished = _cb_route_new_finished;
  rtentry->route_new.family = netaddr_get_address_family(prefix);
  rtentry->route_current.cb_finished = _cb_route_current_finished;
  rtentry->route_current.family = netaddr_get_address_family(prefix);

  avl_insert(&_routing_tree[domain->index], &rtentry->_node);
  return rtentry;
}

/**
 * Remove a routing entry from the global database
 * @param entry pointer to routing entry
 */
static void
_remove_entry(struct olsrv2_routing_entry *entry) {
  /* remove entry from database if its still there */
  if (list_is_node_added(&entry->_node.list)) {
    avl_remove(&_routing_tree[entry->domain->index], &entry->_node);
  }
  oonf_class_free(&_rtset_entry, entry);
}

/**
 * Insert a new entry into the dijkstra working queue
 * @param target pointer to tc target
 * @param neigh next hop through which the target can be reached
 * @param linkcost cost of the last hop of the path towards the target
 * @param pathcost remainder of the cost to the target
 * @param distance hopcount to be used for the route to the target
 * @param single_hop true if this is a single-hop route, false otherwise
 */
static void
_insert_into_working_tree(struct olsrv2_tc_target *target,
    struct nhdp_neighbor *neigh, uint32_t linkcost,
    uint32_t pathcost, uint8_t distance, bool single_hop) {
  struct olsrv2_dijkstra_node *node;
#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str buf;
#endif
  if (linkcost >= RFC7181_METRIC_INFINITE) {
    return;
  }

  node = &target->_dijkstra;

  /*
   * do not add ourselves to working queue,
   * do not add nodes already processed to the working queue
   */
  if (node->local || node->done) {
    return;
  }

  /* calculate new total pathcost */
  pathcost += linkcost;

  if (avl_is_node_added(&node->_node)) {
    /* node already in dijkstra working queue */

    if (node->path_cost <= pathcost) {
      /* current path is shorter than new one */
      return;
    }

    /* we found a better path, remove node from working queue */
    avl_remove(&_dijkstra_working_tree, &node->_node);
  }
  
  OONF_DEBUG(LOG_OLSRV2_ROUTING, "Add dst %s with pathcost %u to dijstra tree (0x%zx)",
          netaddr_to_string(&buf, &target->addr), pathcost,
          (size_t)target);

  node->path_cost = pathcost;
  node->first_hop = neigh;
  node->distance = distance;
  node->single_hop = single_hop;

  avl_insert(&_dijkstra_working_tree, &node->_node);
}

/**
 * Initialize a routing entry with the result of the dijkstra calculation
 * @param domain nhdp domain
 * @param destination destination of routing entry
 * @param first_hop nhdp neighbor for first hop to target
 * @param distance hopcount distance that should be used for route
 * @param pathcost pathcost to target
 * @param single_hop true if route is single hop
 */
static void
_update_routing_entry(struct nhdp_domain *domain,
    struct netaddr *destination,
    struct nhdp_neighbor *first_hop,
    uint8_t distance, uint32_t pathcost,
    bool single_hop) {
  struct nhdp_neighbor_domaindata *neighdata;
  struct olsrv2_routing_entry *rtentry;
#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str buf;
#endif

  /* make sure routing entry is present */
  rtentry = _add_entry(domain, destination);
  if (rtentry == NULL) {
    /* out of memory... */
    return;
  }

  /*
   * routing entry might already be present because it can be set by
   * a tc node AND by attached networks with a maximum prefix length
   */
  if (rtentry->state_current && rtentry->cost < pathcost) {
    /* active routing entry is already cheaper, ignore new one */
    return;
  }

  neighdata = nhdp_domain_get_neighbordata(domain, first_hop);
  OONF_DEBUG(LOG_OLSRV2_ROUTING, "Initialize route entry dst %s with pathcost %u",
      netaddr_to_string(&buf, &rtentry->route_new.dst), pathcost);

  /* copy route parameters into data structure */
  rtentry->route_new.if_index = neighdata->best_link_ifindex;
  rtentry->cost = pathcost;
  rtentry->route_new.metric = distance;

  /* mark route as set */
  rtentry->state_new = true;

  /* copy gateway if necessary */
  if (single_hop
      && netaddr_cmp(&neighdata->best_link->if_addr,
          &rtentry->route_new.dst) == 0) {
    netaddr_invalidate(&rtentry->route_new.gw);
  }
  else {
    memcpy(&rtentry->route_new.gw, &neighdata->best_link->if_addr,
        sizeof(struct netaddr));
  }
}

/**
 * Initialize internal fields for dijkstra calculation
 * @param domain nhdp domain
 */
static void
_prepare_routes(struct nhdp_domain *domain) {
  struct olsrv2_routing_entry *rtentry;
  struct olsrv2_tc_endpoint *end;
  struct olsrv2_tc_node *node;
  struct nhdp_neighbor *neigh;
  struct nhdp_neighbor_domaindata *neigh_metric;

#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str nbuf;
#endif

  /* prepare all existing routing entries and put them into the working queue */
  avl_for_each_element(&_routing_tree[domain->index], rtentry, _node) {
    rtentry->state_new = false;
  }

  /* initialize private dijkstra data on nodes */
  avl_for_each_element(olsrv2_tc_get_tree(), node, _originator_node) {
    node->target._dijkstra.first_hop = NULL;
    node->target._dijkstra.path_cost = RFC7181_METRIC_INFINITE_PATH;
    node->target._dijkstra.local =
        olsrv2_originator_is_local(&node->target.addr);
    node->target._dijkstra.done = false;

    OONF_DEBUG(LOG_OLSRV2_ROUTING, "Prepare node %s%s (0x%zx)",
    		netaddr_to_string(&nbuf, &node->target.addr),
    		node->target._dijkstra.local ? " (local)" : "",
    		(size_t)&node->target);
  }

  /* initialize private dijkstra data on endpoints */
  avl_for_each_element(olsrv2_tc_get_endpoint_tree(), end, _node) {
    end->target._dijkstra.first_hop = NULL;
    end->target._dijkstra.path_cost = RFC7181_METRIC_INFINITE_PATH;
    end->target._dijkstra.done = false;
  }

  /* initialize Dijkstra working queue with one-hop neighbors */
  list_for_each_element(nhdp_db_get_neigh_list(), neigh, _global_node) {
    if (neigh->symmetric > 0
        && netaddr_get_address_family(&neigh->originator) != AF_UNSPEC
        && (node = olsrv2_tc_node_get(&neigh->originator)) != NULL) {
      neigh_metric = nhdp_domain_get_neighbordata(domain, neigh);

      if (neigh_metric->metric.in > RFC7181_METRIC_MAX
          || neigh_metric->metric.out > RFC7181_METRIC_MAX) {
        /* ignore link with infinite metric */
        continue;
      }

      /* found node for neighbor, add to worker list */
      _insert_into_working_tree(&node->target, neigh,
          neigh_metric->metric.out, 0, 0, true);
    }
  }
}

/**
 * Remove item from dijkstra working queue and process it
 * @param domain nhdp domain
 */
static void
_handle_working_queue(struct nhdp_domain *domain) {
  struct olsrv2_tc_target *target;
  struct nhdp_neighbor *first_hop;

  struct olsrv2_tc_node *tc_node;
  struct olsrv2_tc_edge *tc_edge;
  struct olsrv2_tc_attachment *tc_attached;

#ifdef OONF_LOG_DEBUG_INFO
  struct netaddr_str buf;
#endif

  /* get tc target */
  target = avl_first_element(&_dijkstra_working_tree, target, _dijkstra._node);

  /* remove current node from working tree */
  OONF_DEBUG(LOG_OLSRV2_ROUTING, "Remove node %s from dijkstra tree",
      netaddr_to_string(&buf, &target->addr));
  avl_remove(&_dijkstra_working_tree, &target->_dijkstra._node);

  /* mark current node as done */
  target->_dijkstra.done = true;

  /* fill routing entry with dijkstra result */
  _update_routing_entry(domain, &target->addr,
      target->_dijkstra.first_hop,
      target->_dijkstra.distance,
      target->_dijkstra.path_cost,
      target->_dijkstra.single_hop);

  if (target->type == OLSRV2_NODE_TARGET) {
    /* get neighbor and its domain specific data */
    first_hop = target->_dijkstra.first_hop;

    /* calculate pointer of olsrv2_tc_node */
    tc_node = container_of(target, struct olsrv2_tc_node, target);

    /* iterate over edges */
    avl_for_each_element(&tc_node->_edges, tc_edge, _node) {
      if (!tc_edge->virtual && tc_edge->cost[domain->index] <= RFC7181_METRIC_MAX) {
        /* add new tc_node to working tree */
        _insert_into_working_tree(&tc_edge->dst->target, first_hop,
            tc_edge->cost[domain->index], target->_dijkstra.path_cost,
            0, false);
      }
    }

    /* iterate over attached networks and addresses */
    avl_for_each_element(&tc_node->_endpoints, tc_attached, _src_node) {
      if (tc_attached->cost[domain->index] <= RFC7181_METRIC_MAX) {
        /* add attached network or address to working tree */
        _insert_into_working_tree(&tc_attached->dst->target, first_hop,
            tc_attached->cost[domain->index], target->_dijkstra.path_cost,
            tc_attached->distance[domain->index], false);
      }
    }
  }
}

/**
 * Add routes learned from nhdp to dijkstra results
 * @param domain nhdp domain
 */
static void
_handle_nhdp_routes(struct nhdp_domain *domain) {
  struct nhdp_neighbor_domaindata *neigh_data;
  struct nhdp_neighbor *neigh;
  struct nhdp_naddr *naddr;
  struct nhdp_l2hop *l2hop;
  struct nhdp_link *lnk;
  uint32_t neighcost;
  uint32_t l2hop_pathcost;

  list_for_each_element(nhdp_db_get_neigh_list(), neigh, _global_node) {
    /* get linkcost to neighbor */
    neigh_data = nhdp_domain_get_neighbordata(domain, neigh);
    neighcost = neigh_data->metric.out;

    if (neigh->symmetric == 0 || neighcost >= RFC7181_METRIC_INFINITE) {
      continue;
    }

    /* make sure all addresses of the neighbor are better than our direct link */
    avl_for_each_element(&neigh->_neigh_addresses, naddr, _neigh_node) {
      if (!netaddr_acl_check_accept(olsrv2_get_routable(), &naddr->neigh_addr)) {
        /* not a routable address, check the next one */
        continue;
      }

      /* update routing entry */
      _update_routing_entry(domain, &naddr->neigh_addr, neigh, 0, neighcost, true);
    }

    list_for_each_element(&neigh->_links, lnk, _neigh_node) {
      avl_for_each_element(&lnk->_2hop, l2hop, _link_node) {
        /* check if 2hop neighbor is lost */
        if (nhdp_db_2hop_is_lost(l2hop)) {
          continue;
        }

        /* get new pathcost to 2hop neighbor */
        l2hop_pathcost = nhdp_domain_get_l2hopdata(domain, l2hop)->metric.out;
        if (l2hop_pathcost >= RFC7181_METRIC_INFINITE) {
          continue;
        }

        if (!netaddr_acl_check_accept(olsrv2_get_routable(), &l2hop->twohop_addr)) {
          /* not a routable address, check the next one */
          continue;
        }

        l2hop_pathcost += neighcost;

        /* the 2-hop route is better than the dijkstra calculation */
        _update_routing_entry(domain, &l2hop->twohop_addr, neigh, 0, l2hop_pathcost, false);
      }
    }
  }
}

/**
 * Add a route to the kernel processing queue
 * @param rtentry pointer to routing entry
 */
static void
_add_route_to_kernel_queue(struct olsrv2_routing_entry *rtentry) {
#ifdef OONF_LOG_INFO
  struct os_route_str rbuf1, rbuf2;
#endif

  if (rtentry->state_new) {
    OONF_INFO(LOG_OLSRV2_ROUTING,
        "Set route %s (%s)",
        os_routing_to_string(&rbuf1, &rtentry->route_new),
        os_routing_to_string(&rbuf2, &rtentry->route_current));

    if (_domain_parameter[rtentry->domain->index].use_srcip_in_routes
        && netaddr_get_address_family(&rtentry->route_new.dst) == AF_INET) {
      memcpy(&rtentry->route_new.src_ip, olsrv2_originator_get(AF_INET),
          sizeof(rtentry->route_new.src_ip));
    }

    if (netaddr_get_address_family(&rtentry->route_new.gw) == AF_UNSPEC) {
      /* insert/update single-hop routes early */
      list_add_head(&_kernel_queue, &rtentry->_working_node);
    }
    else {
      /* insert/update multi-hop routes late */
      list_add_tail(&_kernel_queue, &rtentry->_working_node);
    }
  }
  else {
    OONF_INFO(LOG_OLSRV2_ROUTING,
        "Dijkstra result: remove route %s",
        os_routing_to_string(&rbuf1, &rtentry->route_current));


    if (netaddr_get_address_family(&rtentry->route_current.gw) == AF_UNSPEC) {
      /* remove single-hop routes late */
      list_add_tail(&_kernel_queue, &rtentry->_working_node);
    }
    else {
      /* remove multi-hop routes early */
      list_add_head(&_kernel_queue, &rtentry->_working_node);
    }
  }
}

/**
 * process the results of a dijkstra run and add them to the kernel
 * processing queue
 * @param domain nhdp domain
 */
static void
_process_dijkstra_result(struct nhdp_domain *domain) {
  struct olsrv2_routing_entry *rtentry;
  struct olsrv2_routing_filter *filter;

  avl_for_each_element(&_routing_tree[domain->index], rtentry, _node) {
    /* initialize rest of route parameters */
    rtentry->route_new.table = _domain_parameter[rtentry->domain->index].table;
    rtentry->route_new.protocol = _domain_parameter[rtentry->domain->index].protocol;
    rtentry->route_new.metric = _domain_parameter[rtentry->domain->index].distance;

    list_for_each_element(&_routing_filter_list, filter, _node) {
      if (!filter->filter(domain, &rtentry->route_new)) {
        /* route was dropped by filter */
        continue;
      }
    }

    if (rtentry->state_new && rtentry->state_current
        && memcpy(&rtentry->route_new, &rtentry->route_current, sizeof(rtentry->route_new)) == 0) {
      /* no change, ignore this entry */
      continue;
    }
    _add_route_to_kernel_queue(rtentry);
  }
}

/**
 * Process all entries in kernel processing queue and send them to the kernel
 */
static void
_process_kernel_queue(void) {
  struct olsrv2_routing_entry *rtentry, *rt_it;
  struct os_route_str rbuf;

  list_for_each_element_safe(&_kernel_queue, rtentry, _working_node, rt_it) {
    /* remove from routing queue */
    list_remove(&rtentry->_working_node);

    /* mark route as in kernel processing */
    rtentry->in_processing = true;

    if (rtentry->state_current) {
      /* if the route exists, we must remove it anyways */
      if (os_routing_set(&rtentry->route_current, false, false)) {
        OONF_WARN(LOG_OLSRV2_ROUTING, "Could not remove route %s",
            os_routing_to_string(&rbuf, &rtentry->route_current));
      }
    }
    if (rtentry->state_new) {
      /* add new to kernel */
      if (os_routing_set(&rtentry->route_new, true, false)) {
        OONF_WARN(LOG_OLSRV2_ROUTING, "Could not set route %s",
            os_routing_to_string(&rbuf, &rtentry->route_new));
      }
    }
  }
}

/**
 * Callback for checking if dijkstra was triggered during
 * rate limitation time
 * @param unused
 */
static void
_cb_trigger_dijkstra(void *unused __attribute__((unused))) {
  if (_trigger_dijkstra) {
    _trigger_dijkstra = false;
    olsrv2_routing_force_update(false);
  }
}

/**
 * Callback triggered when neighbor metrics are updates
 * @param neigh
 */
static void
_cb_nhdp_update(struct nhdp_neighbor *neigh __attribute__((unused))) {
  olsrv2_routing_trigger_update();
}

/**
 * Callback for kernel route processing results
 * @param route pointer to kernel route
 * @param error 0 if no error happened
 */
static void
_cb_route_current_finished(struct os_route *route, int error) {
  struct olsrv2_routing_entry *rtentry;
  struct os_route_str rbuf;

  rtentry = container_of(route, struct olsrv2_routing_entry, route_current);

  /* kernel is not processing this route anymore */
  rtentry->in_processing = false;

  if (error == ESRCH) {
    OONF_DEBUG(LOG_OLSRV2_ROUTING, "Route %s was already gone",
        os_routing_to_string(&rbuf, &rtentry->route_current));
  }
  else if (error) {
    /* an error happened, try again later */
    if (error != -1) {
      /* do not display a os_routing_interrupt() caused error */
      OONF_WARN(LOG_OLSRV2_ROUTING, "Error while removing route %s: %s (%d)",
          os_routing_to_string(&rbuf, &rtentry->route_current),
          strerror(error), error);
    }

    /* prevent a followup route add */
    os_routing_interrupt(&rtentry->route_new);
    return;
  }
  OONF_INFO(LOG_OLSRV2_ROUTING, "Successfully removed route %s",
      os_routing_to_string(&rbuf, &rtentry->route_current));


  if (rtentry->state_new) {
    /* apply intermediate state */
    rtentry->state_current = false;
  }
  else {
    /* remove routing entry */
    _remove_entry(rtentry);
  }
}

/**
 * Callback for kernel route processing results
 * @param route pointer to kernel route
 * @param error 0 if no error happened
 */
static void
_cb_route_new_finished(struct os_route *route, int error) {
  struct olsrv2_routing_entry *rtentry;
  struct os_route_str rbuf;

  rtentry = container_of(route, struct olsrv2_routing_entry, route_new);

  /* kernel is not processing this route anymore */
  rtentry->in_processing = false;

  if (error) {
    /* an error happened, try again later */
    if (error != -1) {
      /* do not display a os_routing_interrupt() caused error */
      OONF_WARN(LOG_OLSRV2_ROUTING, "Error while adding route %s: %s (%d)",
          os_routing_to_string(&rbuf, &rtentry->route_new),
          strerror(error), error);
    }

    /* revert attempted change */
    if (!rtentry->state_current) {
      _remove_entry(rtentry);
    }
    return;
  }
  /* route was set/updated successfully */
  OONF_INFO(LOG_OLSRV2_ROUTING, "Successfully set route %s",
      os_routing_to_string(&rbuf, &rtentry->route_new));

  /* update current state */
  rtentry->state_current = true;
  memcpy(&rtentry->route_current, &rtentry->route_new, sizeof(rtentry->route_new));

  /* fix callback we overwrote with memcpy */
  rtentry->route_current.cb_finished = _cb_route_current_finished;
}
