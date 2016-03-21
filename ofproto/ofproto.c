/*
 * Copyright (c) 2009, 2010, 2011, 2012 Nicira Networks.
 * Copyright (c) 2010 Jean Tourrilhes - HP-Labs.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "command-line.h"
#include "compiler.h"
#include "dirs.h"
#include "dynamic-string.h"
#include "json.h"
#include "ovsdb-data.h"
#include "ovsdb-idl.h"
#include "poll-loop.h"
#include "process.h"
#include "stream.h"
#include "stream-ssl.h"
#include "sset.h"
#include "svec.h"
#include "vswitchd/vswitch-idl.h"
#include "table.h"
#include "timeval.h"
#include "util.h"
#include "vconn.h"
#include "vlog.h"

#include <config.h>
#include "ofproto.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include "bitmap.h"
#include "byte-order.h"
#include "classifier.h"
#include "connmgr.h"
#include "coverage.h"
#include "dynamic-string.h"
#include "hash.h"
#include "hmap.h"
#include "netdev.h"
#include "nx-match.h"
#include "ofp-print.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "ofproto-provider.h"
#include "openflow/nicira-ext.h"
#include "openflow/openflow.h"
#include "packets.h"
#include "pinsched.h"
#include "pktbuf.h"
#include "poll-loop.h"
#include "shash.h"
#include "sset.h"
#include "timeval.h"
#include "unaligned.h"
#include "unixctl.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(ofproto);

COVERAGE_DEFINE(ofproto_error);
COVERAGE_DEFINE(ofproto_flush);
COVERAGE_DEFINE(ofproto_no_packet_in);
COVERAGE_DEFINE(ofproto_packet_out);
COVERAGE_DEFINE(ofproto_queue_req);
COVERAGE_DEFINE(ofproto_recv_openflow);
COVERAGE_DEFINE(ofproto_reinit_ports);
COVERAGE_DEFINE(ofproto_uninstallable);
COVERAGE_DEFINE(ofproto_update_port);

enum ofproto_state {
    S_OPENFLOW,                 /* Processing OpenFlow commands. */
    S_FLUSH,                    /* Deleting all flow table rules. */
};

enum ofoperation_type {
    OFOPERATION_ADD,
    OFOPERATION_DELETE,
    OFOPERATION_MODIFY
};

////////////////////////////////////////////////////////////////////////////
/* vsctl_fatal() also logs the error, so it is preferred in this file. */
#define ovs_fatal please_use_vsctl_fatal_instead_of_ovs_fatal

struct vsctl_context;
static int i=0;
////////////////////////////////////////////////////
static char s0[]="ovs-vsctl";
static char s1[]="--";
static char s2[]="set";
static char s3[]="Port";
static char s4[]="s1-eth4";
static char s5[]="qos=@newqos";
static char s6[]="--";
static char s7[]="--id=@newqos";
static char s8[]="create";
static char s9[]="QoS";
static char s10[]="type=linux-htb";
static char s11[]="other-config:max-rate=100000000";
static char s12[]="welcome";

/////////////////////////////////////////////////////

static char a0[]="ovs-vsctl";
static char a1[]="--";
static char a2[]="add";
static char a3[]="Qos";
static char a4[]="3dba4eae-4566-4af6-9ea7-294c00cb2bc6";
static char a5[]="queues";
static char a6[]="111=@q111";
static char a7[]="--";
static char a8[]="--id=@q111";
static char a9[]="create";
static char a10[]="Queue";
static char a11[]="other-config:min-rate=100000000";
static char a12[]="other-config:max-rate=100000000";
static char a13[]="welcome";



/////////////////////////////////////////////////////

static char r0[]="ovs-vsctl";
static char r1[]="--";
static char r2[]="remove";
static char r3[]="Qos";
static char r4[]="3dba4eae-4566-4af6-9ea7-294c00cb2bc6";
static char r5[]="queues";
static char r6[]="111";
static char r7[]="welcome";

/////////////////////////////////////////////////////
static char *bridge_name;  

static bool verifyqos[49] = {false};
static char sqos[49][36];
static char array[12];
static char ap6[3]="=@q";
static int sp;


/* A command supported by ovs-vsctl. */
struct vsctl_command_syntax {
    const char *name;           /* e.g. "add-br" */
    int min_args;               /* Min number of arguments following name. */
    int max_args;               /* Max number of arguments following name. */

    /* If nonnull, calls ovsdb_idl_add_column() or ovsdb_idl_add_table() for
     * each column or table in ctx->idl that it uses. */
    void (*prerequisites)(struct vsctl_context *ctx);

    /* Does the actual work of the command and puts the command's output, if
     * any, in ctx->output or ctx->table.
     *
     * Alternatively, if some prerequisite of the command is not met and the
     * caller should wait for something to change and then retry, it may set
     * ctx->try_again to true.  (Only the "wait-until" command currently does
     * this.) */
    void (*run)(struct vsctl_context *ctx);

    /* If nonnull, called after the transaction has been successfully
     * committed.  ctx->output is the output from the "run" function, which
     * this function may modify and otherwise postprocess as needed.  (Only the
     * "create" command currently does any postprocessing.) */
    void (*postprocess)(struct vsctl_context *ctx);

    /* A comma-separated list of supported options, e.g. "--a,--b", or the
     * empty string if the command does not support any options. */
    const char *options;
    enum { RO, RW } mode;       /* Does this command modify the database? */
};

struct vsctl_command {
    /* Data that remains constant after initialization. */
    const struct vsctl_command_syntax *syntax;
    int argc;
    char **argv;
    struct shash options;

    /* Data modified by commands. */
    struct ds output;
    struct table *table;
};

/* --db: The database server to contact. */
static const char *db;

/* --oneline: Write each command's output as a single line? */
static bool oneline;

/* --dry-run: Do not commit any changes. */
static bool dry_run;

/* --no-wait: Wait for ovs-vswitchd to reload its configuration? */
static bool wait_for_reload = true;

/* --timeout: Time to wait for a connection to 'db'. */
static int timeout;

/* Format for table output. */
static struct table_style table_style = TABLE_STYLE_DEFAULT;

/* All supported commands. */
static const struct vsctl_command_syntax all_commands[];

/* The IDL we're using and the current transaction, if any.
 * This is for use by vsctl_exit() only, to allow it to clean up.
 * Other code should use its context arguments. */
static struct ovsdb_idl *the_idl;
static struct ovsdb_idl_txn *the_idl_txn;

static void vsctl_exit(int status) NO_RETURN;
static void vsctl_fatal(const char *, ...) PRINTF_FORMAT(1, 2) NO_RETURN;
static char *default_db(void);
static void usage(void) NO_RETURN;
static void parse_options(int argc, char *argv[]);
static bool might_write_to_db(char **argv);

static struct vsctl_command *parse_commands(int argc, char *argv[],
                                            size_t *n_commandsp);
static void parse_command(int argc, char *argv[], struct vsctl_command *);
static const struct vsctl_command_syntax *find_command(const char *name);
static void run_prerequisites(struct vsctl_command[], size_t n_commands,
                              struct ovsdb_idl *);
static enum ovsdb_idl_txn_status do_vsctl(const char *args,
                                          struct vsctl_command *, size_t n,
                                          struct ovsdb_idl *);

static const struct vsctl_table_class *get_table(const char *table_name);
static void set_column(const struct vsctl_table_class *,
                       const struct ovsdb_idl_row *, const char *arg,
                       struct ovsdb_symbol_table *);

static bool is_condition_satisfied(const struct vsctl_table_class *,
                                   const struct ovsdb_idl_row *,
                                   const char *arg,
                                   struct ovsdb_symbol_table *);
////////////////////////////////////////////////////////////////

/* A single OpenFlow request can execute any number of operations.  The
 * ofopgroup maintain OpenFlow state common to all of the operations, e.g. the
 * ofconn to which an error reply should be sent if necessary.
 *
 * ofproto initiates some operations internally.  These operations are still
 * assigned to groups but will not have an associated ofconn. */
struct ofopgroup {
    struct ofproto *ofproto;    /* Owning ofproto. */
    struct list ofproto_node;   /* In ofproto's "pending" list. */
    struct list ops;            /* List of "struct ofoperation"s. */

    /* Data needed to send OpenFlow reply on failure or to send a buffered
     * packet on success.
     *
     * If list_is_empty(ofconn_node) then this ofopgroup never had an
     * associated ofconn or its ofconn's connection dropped after it initiated
     * the operation.  In the latter case 'ofconn' is a wild pointer that
     * refers to freed memory, so the 'ofconn' member must be used only if
     * !list_is_empty(ofconn_node).
     */
    struct list ofconn_node;    /* In ofconn's list of pending opgroups. */
    struct ofconn *ofconn;      /* ofconn for reply (but see note above). */
    struct ofp_header *request; /* Original request (truncated at 64 bytes). */
    uint32_t buffer_id;         /* Buffer id from original request. */
    int error;                  /* 0 if no error yet, otherwise error code. */
};

static struct ofopgroup *ofopgroup_create_unattached(struct ofproto *);
static struct ofopgroup *ofopgroup_create(struct ofproto *, struct ofconn *,
                                          const struct ofp_header *,
                                          uint32_t buffer_id);
static void ofopgroup_submit(struct ofopgroup *);
static void ofopgroup_destroy(struct ofopgroup *);

/* A single flow table operation. */
struct ofoperation {
    struct ofopgroup *group;    /* Owning group. */
    struct list group_node;     /* In ofopgroup's "ops" list. */
    struct hmap_node hmap_node; /* In ofproto's "deletions" hmap. */
    struct rule *rule;          /* Rule being operated upon. */
    enum ofoperation_type type; /* Type of operation. */
    int status;                 /* -1 if pending, otherwise 0 or error code. */
    struct rule *victim;        /* OFOPERATION_ADDING: Replaced rule. */
    union ofp_action *actions;  /* OFOPERATION_MODIFYING: Replaced actions. */
    int n_actions;              /* OFOPERATION_MODIFYING: # of old actions. */
    ovs_be64 flow_cookie;       /* Rule's old flow cookie. */
};

static void ofoperation_create(struct ofopgroup *, struct rule *,
                               enum ofoperation_type);
static void ofoperation_destroy(struct ofoperation *);

static void ofport_destroy__(struct ofport *);
static void ofport_destroy(struct ofport *);

static uint64_t pick_datapath_id(const struct ofproto *);
static uint64_t pick_fallback_dpid(void);

static void ofproto_destroy__(struct ofproto *);

static void ofproto_rule_destroy__(struct rule *);
static void ofproto_rule_send_removed(struct rule *, uint8_t reason);

static void ofopgroup_destroy(struct ofopgroup *);

static int add_flow(struct ofproto *, struct ofconn *,
                    const struct ofputil_flow_mod *,
                    const struct ofp_header *);

static bool handle_openflow(struct ofconn *, struct ofpbuf *);
static int handle_flow_mod__(struct ofproto *, struct ofconn *,
                             const struct ofputil_flow_mod *,
                             const struct ofp_header *);

static void update_port(struct ofproto *, const char *devname);
static int init_ports(struct ofproto *);
static void reinit_ports(struct ofproto *);
static void set_internal_devs_mtu(struct ofproto *);

static void ofproto_unixctl_init(void);

/* All registered ofproto classes, in probe order. */
static const struct ofproto_class **ofproto_classes;
static size_t n_ofproto_classes;
static size_t allocated_ofproto_classes;

/* Map from datapath name to struct ofproto, for use by unixctl commands. */
static struct hmap all_ofprotos = HMAP_INITIALIZER(&all_ofprotos);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

static void
ofproto_initialize(void)
{
    static bool inited;

    if (!inited) {
        inited = true;
        ofproto_class_register(&ofproto_dpif_class);
    }
}

/* 'type' should be a normalized datapath type, as returned by
 * ofproto_normalize_type().  Returns the corresponding ofproto_class
 * structure, or a null pointer if there is none registered for 'type'. */
static const struct ofproto_class *
ofproto_class_find__(const char *type)
{
    size_t i;

    ofproto_initialize();
    for (i = 0; i < n_ofproto_classes; i++) {
        const struct ofproto_class *class = ofproto_classes[i];
        struct sset types;
        bool found;

        sset_init(&types);
        class->enumerate_types(&types);
        found = sset_contains(&types, type);
        sset_destroy(&types);

        if (found) {
            return class;
        }
    }
    VLOG_WARN("unknown datapath type %s", type);
    return NULL;
}

/* Registers a new ofproto class.  After successful registration, new ofprotos
 * of that type can be created using ofproto_create(). */
int
ofproto_class_register(const struct ofproto_class *new_class)
{
    size_t i;

    for (i = 0; i < n_ofproto_classes; i++) {
        if (ofproto_classes[i] == new_class) {
            return EEXIST;
        }
    }

    if (n_ofproto_classes >= allocated_ofproto_classes) {
        ofproto_classes = x2nrealloc(ofproto_classes,
                                     &allocated_ofproto_classes,
                                     sizeof *ofproto_classes);
    }
    ofproto_classes[n_ofproto_classes++] = new_class;
    return 0;
}

/* Unregisters a datapath provider.  'type' must have been previously
 * registered and not currently be in use by any ofprotos.  After
 * unregistration new datapaths of that type cannot be opened using
 * ofproto_create(). */
int
ofproto_class_unregister(const struct ofproto_class *class)
{
    size_t i;

    for (i = 0; i < n_ofproto_classes; i++) {
        if (ofproto_classes[i] == class) {
            for (i++; i < n_ofproto_classes; i++) {
                ofproto_classes[i - 1] = ofproto_classes[i];
            }
            n_ofproto_classes--;
            return 0;
        }
    }
    VLOG_WARN("attempted to unregister an ofproto class that is not "
              "registered");
    return EAFNOSUPPORT;
}

/* Clears 'types' and enumerates all registered ofproto types into it.  The
 * caller must first initialize the sset. */
void
ofproto_enumerate_types(struct sset *types)
{
    size_t i;

    ofproto_initialize();
    for (i = 0; i < n_ofproto_classes; i++) {
        ofproto_classes[i]->enumerate_types(types);
    }
}

/* Returns the fully spelled out name for the given ofproto 'type'.
 *
 * Normalized type string can be compared with strcmp().  Unnormalized type
 * string might be the same even if they have different spellings. */
const char *
ofproto_normalize_type(const char *type)
{
    return type && type[0] ? type : "system";
}

/* Clears 'names' and enumerates the names of all known created ofprotos with
 * the given 'type'.  The caller must first initialize the sset.  Returns 0 if
 * successful, otherwise a positive errno value.
 *
 * Some kinds of datapaths might not be practically enumerable.  This is not
 * considered an error. */
int
ofproto_enumerate_names(const char *type, struct sset *names)
{
    const struct ofproto_class *class = ofproto_class_find__(type);
    return class ? class->enumerate_names(type, names) : EAFNOSUPPORT;
 }

int
ofproto_create(const char *datapath_name, const char *datapath_type,
               struct ofproto **ofprotop)
{
    const struct ofproto_class *class;
    struct classifier *table;
    struct ofproto *ofproto;
    int n_tables;
    int error;

    *ofprotop = NULL;

    ofproto_initialize();
    ofproto_unixctl_init();

    datapath_type = ofproto_normalize_type(datapath_type);
    class = ofproto_class_find__(datapath_type);
    if (!class) {
        VLOG_WARN("could not create datapath %s of unknown type %s",
                  datapath_name, datapath_type);
        return EAFNOSUPPORT;
    }

    ofproto = class->alloc();
    if (!ofproto) {
        VLOG_ERR("failed to allocate datapath %s of type %s",
                 datapath_name, datapath_type);
        return ENOMEM;
    }

    /* Initialize. */
    memset(ofproto, 0, sizeof *ofproto);
    ofproto->ofproto_class = class;
    ofproto->name = xstrdup(datapath_name);
    ofproto->type = xstrdup(datapath_type);
    hmap_insert(&all_ofprotos, &ofproto->hmap_node,
                hash_string(ofproto->name, 0));
    ofproto->datapath_id = 0;
    ofproto_set_flow_eviction_threshold(ofproto,
                                        OFPROTO_FLOW_EVICTON_THRESHOLD_DEFAULT);
    ofproto->forward_bpdu = false;
    ofproto->fallback_dpid = pick_fallback_dpid();
    ofproto->mfr_desc = xstrdup(DEFAULT_MFR_DESC);
    ofproto->hw_desc = xstrdup(DEFAULT_HW_DESC);
    ofproto->sw_desc = xstrdup(DEFAULT_SW_DESC);
    ofproto->serial_desc = xstrdup(DEFAULT_SERIAL_DESC);
    ofproto->dp_desc = xstrdup(DEFAULT_DP_DESC);
    ofproto->frag_handling = OFPC_FRAG_NORMAL;
    hmap_init(&ofproto->ports);
    shash_init(&ofproto->port_by_name);
    ofproto->tables = NULL;
    ofproto->n_tables = 0;
    ofproto->connmgr = connmgr_create(ofproto, datapath_name, datapath_name);
    ofproto->state = S_OPENFLOW;
    list_init(&ofproto->pending);
    ofproto->n_pending = 0;
    hmap_init(&ofproto->deletions);
    ofproto->vlan_bitmap = NULL;
    ofproto->vlans_changed = false;

    error = ofproto->ofproto_class->construct(ofproto, &n_tables);
    if (error) {
        VLOG_ERR("failed to open datapath %s: %s",
                 datapath_name, strerror(error));
        ofproto_destroy__(ofproto);
        return error;
    }

    assert(n_tables >= 1 && n_tables <= 255);
    ofproto->n_tables = n_tables;
    ofproto->tables = xmalloc(n_tables * sizeof *ofproto->tables);
    OFPROTO_FOR_EACH_TABLE (table, ofproto) {
        classifier_init(table);
    }

    ofproto->datapath_id = pick_datapath_id(ofproto);
    VLOG_INFO("using datapath ID %016"PRIx64, ofproto->datapath_id);
    init_ports(ofproto);

    *ofprotop = ofproto;
    return 0;
}

void
ofproto_set_datapath_id(struct ofproto *p, uint64_t datapath_id)
{
    uint64_t old_dpid = p->datapath_id;
    p->datapath_id = datapath_id ? datapath_id : pick_datapath_id(p);
    if (p->datapath_id != old_dpid) {
        VLOG_INFO("datapath ID changed to %016"PRIx64, p->datapath_id);

        /* Force all active connections to reconnect, since there is no way to
         * notify a controller that the datapath ID has changed. */
        ofproto_reconnect_controllers(p);
    }
}

void
ofproto_set_controllers(struct ofproto *p,
                        const struct ofproto_controller *controllers,
                        size_t n_controllers)
{
    connmgr_set_controllers(p->connmgr, controllers, n_controllers);
}

void
ofproto_set_fail_mode(struct ofproto *p, enum ofproto_fail_mode fail_mode)
{
    connmgr_set_fail_mode(p->connmgr, fail_mode);
}

/* Drops the connections between 'ofproto' and all of its controllers, forcing
 * them to reconnect. */
void
ofproto_reconnect_controllers(struct ofproto *ofproto)
{
    connmgr_reconnect(ofproto->connmgr);
}

/* Sets the 'n' TCP port addresses in 'extras' as ones to which 'ofproto''s
 * in-band control should guarantee access, in the same way that in-band
 * control guarantees access to OpenFlow controllers. */
void
ofproto_set_extra_in_band_remotes(struct ofproto *ofproto,
                                  const struct sockaddr_in *extras, size_t n)
{
    connmgr_set_extra_in_band_remotes(ofproto->connmgr, extras, n);
}

/* Sets the OpenFlow queue used by flows set up by in-band control on
 * 'ofproto' to 'queue_id'.  If 'queue_id' is negative, then in-band control
 * flows will use the default queue. */
void
ofproto_set_in_band_queue(struct ofproto *ofproto, int queue_id)
{
    connmgr_set_in_band_queue(ofproto->connmgr, queue_id);
}

/* Sets the number of flows at which eviction from the kernel flow table
 * will occur. */
void
ofproto_set_flow_eviction_threshold(struct ofproto *ofproto, unsigned threshold)
{
    if (threshold < OFPROTO_FLOW_EVICTION_THRESHOLD_MIN) {
        ofproto->flow_eviction_threshold = OFPROTO_FLOW_EVICTION_THRESHOLD_MIN;
    } else {
        ofproto->flow_eviction_threshold = threshold;
    }
}

/* If forward_bpdu is true, the NORMAL action will forward frames with
 * reserved (e.g. STP) destination Ethernet addresses. if forward_bpdu is false,
 * the NORMAL action will drop these frames. */
void
ofproto_set_forward_bpdu(struct ofproto *ofproto, bool forward_bpdu)
{
    bool old_val = ofproto->forward_bpdu;
    ofproto->forward_bpdu = forward_bpdu;
    if (old_val != ofproto->forward_bpdu) {
        if (ofproto->ofproto_class->forward_bpdu_changed) {
            ofproto->ofproto_class->forward_bpdu_changed(ofproto);
        }
    }
}

/* Sets the MAC aging timeout for the OFPP_NORMAL action on 'ofproto' to
 * 'idle_time', in seconds. */
void
ofproto_set_mac_idle_time(struct ofproto *ofproto, unsigned idle_time)
{
    if (ofproto->ofproto_class->set_mac_idle_time) {
        ofproto->ofproto_class->set_mac_idle_time(ofproto, idle_time);
    }
}

void
ofproto_set_desc(struct ofproto *p,
                 const char *mfr_desc, const char *hw_desc,
                 const char *sw_desc, const char *serial_desc,
                 const char *dp_desc)
{
    struct ofp_desc_stats *ods;

    if (mfr_desc) {
        if (strlen(mfr_desc) >= sizeof ods->mfr_desc) {
            VLOG_WARN("truncating mfr_desc, must be less than %zu characters",
                    sizeof ods->mfr_desc);
        }
        free(p->mfr_desc);
        p->mfr_desc = xstrdup(mfr_desc);
    }
    if (hw_desc) {
        if (strlen(hw_desc) >= sizeof ods->hw_desc) {
            VLOG_WARN("truncating hw_desc, must be less than %zu characters",
                    sizeof ods->hw_desc);
        }
        free(p->hw_desc);
        p->hw_desc = xstrdup(hw_desc);
    }
    if (sw_desc) {
        if (strlen(sw_desc) >= sizeof ods->sw_desc) {
            VLOG_WARN("truncating sw_desc, must be less than %zu characters",
                    sizeof ods->sw_desc);
        }
        free(p->sw_desc);
        p->sw_desc = xstrdup(sw_desc);
    }
    if (serial_desc) {
        if (strlen(serial_desc) >= sizeof ods->serial_num) {
            VLOG_WARN("truncating serial_desc, must be less than %zu "
                    "characters",
                    sizeof ods->serial_num);
        }
        free(p->serial_desc);
        p->serial_desc = xstrdup(serial_desc);
    }
    if (dp_desc) {
        if (strlen(dp_desc) >= sizeof ods->dp_desc) {
            VLOG_WARN("truncating dp_desc, must be less than %zu characters",
                    sizeof ods->dp_desc);
        }
        free(p->dp_desc);
        p->dp_desc = xstrdup(dp_desc);
    }
}

int
ofproto_set_snoops(struct ofproto *ofproto, const struct sset *snoops)
{
    return connmgr_set_snoops(ofproto->connmgr, snoops);
}

int
ofproto_set_netflow(struct ofproto *ofproto,
                    const struct netflow_options *nf_options)
{
    if (nf_options && sset_is_empty(&nf_options->collectors)) {
        nf_options = NULL;
    }

    if (ofproto->ofproto_class->set_netflow) {
        return ofproto->ofproto_class->set_netflow(ofproto, nf_options);
    } else {
        return nf_options ? EOPNOTSUPP : 0;
    }
}

int
ofproto_set_sflow(struct ofproto *ofproto,
                  const struct ofproto_sflow_options *oso)
{
    if (oso && sset_is_empty(&oso->targets)) {
        oso = NULL;
    }

    if (ofproto->ofproto_class->set_sflow) {
        return ofproto->ofproto_class->set_sflow(ofproto, oso);
    } else {
        return oso ? EOPNOTSUPP : 0;
    }
}

/* Spanning Tree Protocol (STP) configuration. */

/* Configures STP on 'ofproto' using the settings defined in 's'.  If
 * 's' is NULL, disables STP.
 *
 * Returns 0 if successful, otherwise a positive errno value. */
int
ofproto_set_stp(struct ofproto *ofproto,
                const struct ofproto_stp_settings *s)
{
    return (ofproto->ofproto_class->set_stp
            ? ofproto->ofproto_class->set_stp(ofproto, s)
            : EOPNOTSUPP);
}

/* Retrieves STP status of 'ofproto' and stores it in 's'.  If the
 * 'enabled' member of 's' is false, then the other members are not
 * meaningful.
 *
 * Returns 0 if successful, otherwise a positive errno value. */
int
ofproto_get_stp_status(struct ofproto *ofproto,
                       struct ofproto_stp_status *s)
{
    return (ofproto->ofproto_class->get_stp_status
            ? ofproto->ofproto_class->get_stp_status(ofproto, s)
            : EOPNOTSUPP);
}

/* Configures STP on 'ofp_port' of 'ofproto' using the settings defined
 * in 's'.  The caller is responsible for assigning STP port numbers
 * (using the 'port_num' member in the range of 1 through 255, inclusive)
 * and ensuring there are no duplicates.  If the 's' is NULL, then STP
 * is disabled on the port.
 *
 * Returns 0 if successful, otherwise a positive errno value.*/
int
ofproto_port_set_stp(struct ofproto *ofproto, uint16_t ofp_port,
                     const struct ofproto_port_stp_settings *s)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);
    if (!ofport) {
        VLOG_WARN("%s: cannot configure STP on nonexistent port %"PRIu16,
                  ofproto->name, ofp_port);
        return ENODEV;
    }

    return (ofproto->ofproto_class->set_stp_port
            ? ofproto->ofproto_class->set_stp_port(ofport, s)
            : EOPNOTSUPP);
}

/* Retrieves STP port status of 'ofp_port' on 'ofproto' and stores it in
 * 's'.  If the 'enabled' member in 's' is false, then the other members
 * are not meaningful.
 *
 * Returns 0 if successful, otherwise a positive errno value.*/
int
ofproto_port_get_stp_status(struct ofproto *ofproto, uint16_t ofp_port,
                            struct ofproto_port_stp_status *s)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);
    if (!ofport) {
        VLOG_WARN_RL(&rl, "%s: cannot get STP status on nonexistent "
                     "port %"PRIu16, ofproto->name, ofp_port);
        return ENODEV;
    }

    return (ofproto->ofproto_class->get_stp_port_status
            ? ofproto->ofproto_class->get_stp_port_status(ofport, s)
            : EOPNOTSUPP);
}

/* Queue DSCP configuration. */

/* Registers meta-data associated with the 'n_qdscp' Qualities of Service
 * 'queues' attached to 'ofport'.  This data is not intended to be sufficient
 * to implement QoS.  Instead, it is used to implement features which require
 * knowledge of what queues exist on a port, and some basic information about
 * them.
 *
 * Returns 0 if successful, otherwise a positive errno value. */
int
ofproto_port_set_queues(struct ofproto *ofproto, uint16_t ofp_port,
                        const struct ofproto_port_queue *queues,
                        size_t n_queues)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);

    if (!ofport) {
        VLOG_WARN("%s: cannot set queues on nonexistent port %"PRIu16,
                  ofproto->name, ofp_port);
        return ENODEV;
    }

    return (ofproto->ofproto_class->set_queues
            ? ofproto->ofproto_class->set_queues(ofport, queues, n_queues)
            : EOPNOTSUPP);
}

/* Connectivity Fault Management configuration. */

/* Clears the CFM configuration from 'ofp_port' on 'ofproto'. */
void
ofproto_port_clear_cfm(struct ofproto *ofproto, uint16_t ofp_port)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);
    if (ofport && ofproto->ofproto_class->set_cfm) {
        ofproto->ofproto_class->set_cfm(ofport, NULL);
    }
}

/* Configures connectivity fault management on 'ofp_port' in 'ofproto'.  Takes
 * basic configuration from the configuration members in 'cfm', and the remote
 * maintenance point ID from  remote_mpid.  Ignores the statistics members of
 * 'cfm'.
 *
 * This function has no effect if 'ofproto' does not have a port 'ofp_port'. */
void
ofproto_port_set_cfm(struct ofproto *ofproto, uint16_t ofp_port,
                     const struct cfm_settings *s)
{
    struct ofport *ofport;
    int error;

    ofport = ofproto_get_port(ofproto, ofp_port);
    if (!ofport) {
        VLOG_WARN("%s: cannot configure CFM on nonexistent port %"PRIu16,
                  ofproto->name, ofp_port);
        return;
    }

    /* XXX: For configuration simplicity, we only support one remote_mpid
     * outside of the CFM module.  It's not clear if this is the correct long
     * term solution or not. */
    error = (ofproto->ofproto_class->set_cfm
             ? ofproto->ofproto_class->set_cfm(ofport, s)
             : EOPNOTSUPP);
    if (error) {
        VLOG_WARN("%s: CFM configuration on port %"PRIu16" (%s) failed (%s)",
                  ofproto->name, ofp_port, netdev_get_name(ofport->netdev),
                  strerror(error));
    }
}

/* Checks the status of LACP negotiation for 'ofp_port' within ofproto.
 * Returns 1 if LACP partner information for 'ofp_port' is up-to-date,
 * 0 if LACP partner information is not current (generally indicating a
 * connectivity problem), or -1 if LACP is not enabled on 'ofp_port'. */
int
ofproto_port_is_lacp_current(struct ofproto *ofproto, uint16_t ofp_port)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);
    return (ofport && ofproto->ofproto_class->port_is_lacp_current
            ? ofproto->ofproto_class->port_is_lacp_current(ofport)
            : -1);
}

/* Bundles. */

/* Registers a "bundle" associated with client data pointer 'aux' in 'ofproto'.
 * A bundle is the same concept as a Port in OVSDB, that is, it consists of one
 * or more "slave" devices (Interfaces, in OVSDB) along with a VLAN
 * configuration plus, if there is more than one slave, a bonding
 * configuration.
 *
 * If 'aux' is already registered then this function updates its configuration
 * to 's'.  Otherwise, this function registers a new bundle.
 *
 * Bundles only affect the NXAST_AUTOPATH action and output to the OFPP_NORMAL
 * port. */
int
ofproto_bundle_register(struct ofproto *ofproto, void *aux,
                        const struct ofproto_bundle_settings *s)
{
    return (ofproto->ofproto_class->bundle_set
            ? ofproto->ofproto_class->bundle_set(ofproto, aux, s)
            : EOPNOTSUPP);
}

/* Unregisters the bundle registered on 'ofproto' with auxiliary data 'aux'.
 * If no such bundle has been registered, this has no effect. */
int
ofproto_bundle_unregister(struct ofproto *ofproto, void *aux)
{
    return ofproto_bundle_register(ofproto, aux, NULL);
}


/* Registers a mirror associated with client data pointer 'aux' in 'ofproto'.
 * If 'aux' is already registered then this function updates its configuration
 * to 's'.  Otherwise, this function registers a new mirror. */
int
ofproto_mirror_register(struct ofproto *ofproto, void *aux,
                        const struct ofproto_mirror_settings *s)
{
    return (ofproto->ofproto_class->mirror_set
            ? ofproto->ofproto_class->mirror_set(ofproto, aux, s)
            : EOPNOTSUPP);
}

/* Unregisters the mirror registered on 'ofproto' with auxiliary data 'aux'.
 * If no mirror has been registered, this has no effect. */
int
ofproto_mirror_unregister(struct ofproto *ofproto, void *aux)
{
    return ofproto_mirror_register(ofproto, aux, NULL);
}

/* Retrieves statistics from mirror associated with client data pointer
 * 'aux' in 'ofproto'.  Stores packet and byte counts in 'packets' and
 * 'bytes', respectively.  If a particular counters is not supported,
 * the appropriate argument is set to UINT64_MAX. */
int
ofproto_mirror_get_stats(struct ofproto *ofproto, void *aux,
                         uint64_t *packets, uint64_t *bytes)
{
    if (!ofproto->ofproto_class->mirror_get_stats) {
        *packets = *bytes = UINT64_MAX;
        return EOPNOTSUPP;
    }

    return ofproto->ofproto_class->mirror_get_stats(ofproto, aux,
                                                    packets, bytes);
}

/* Configures the VLANs whose bits are set to 1 in 'flood_vlans' as VLANs on
 * which all packets are flooded, instead of using MAC learning.  If
 * 'flood_vlans' is NULL, then MAC learning applies to all VLANs.
 *
 * Flood VLANs affect only the treatment of packets output to the OFPP_NORMAL
 * port. */
int
ofproto_set_flood_vlans(struct ofproto *ofproto, unsigned long *flood_vlans)
{
    return (ofproto->ofproto_class->set_flood_vlans
            ? ofproto->ofproto_class->set_flood_vlans(ofproto, flood_vlans)
            : EOPNOTSUPP);
}

/* Returns true if 'aux' is a registered bundle that is currently in use as the
 * output for a mirror. */
bool
ofproto_is_mirror_output_bundle(const struct ofproto *ofproto, void *aux)
{
    return (ofproto->ofproto_class->is_mirror_output_bundle
            ? ofproto->ofproto_class->is_mirror_output_bundle(ofproto, aux)
            : false);
}

bool
ofproto_has_snoops(const struct ofproto *ofproto)
{
    return connmgr_has_snoops(ofproto->connmgr);
}

void
ofproto_get_snoops(const struct ofproto *ofproto, struct sset *snoops)
{
    connmgr_get_snoops(ofproto->connmgr, snoops);
}

static void
ofproto_flush__(struct ofproto *ofproto)
{
    struct classifier *table;
    struct ofopgroup *group;

    if (ofproto->ofproto_class->flush) {
        ofproto->ofproto_class->flush(ofproto);
    }

    group = ofopgroup_create_unattached(ofproto);
    OFPROTO_FOR_EACH_TABLE (table, ofproto) {
        struct rule *rule, *next_rule;
        struct cls_cursor cursor;

        cls_cursor_init(&cursor, table, NULL);
        CLS_CURSOR_FOR_EACH_SAFE (rule, next_rule, cr, &cursor) {
            if (!rule->pending) {
                ofoperation_create(group, rule, OFOPERATION_DELETE);
                classifier_remove(table, &rule->cr);
                ofproto->ofproto_class->rule_destruct(rule);
            }
        }
    }
    ofopgroup_submit(group);
}

static void
ofproto_destroy__(struct ofproto *ofproto)
{
    struct classifier *table;

    assert(list_is_empty(&ofproto->pending));
    assert(!ofproto->n_pending);

    connmgr_destroy(ofproto->connmgr);

    hmap_remove(&all_ofprotos, &ofproto->hmap_node);
    free(ofproto->name);
    free(ofproto->type);
    free(ofproto->mfr_desc);
    free(ofproto->hw_desc);
    free(ofproto->sw_desc);
    free(ofproto->serial_desc);
    free(ofproto->dp_desc);
    hmap_destroy(&ofproto->ports);
    shash_destroy(&ofproto->port_by_name);

    OFPROTO_FOR_EACH_TABLE (table, ofproto) {
        assert(classifier_is_empty(table));
        classifier_destroy(table);
    }
    free(ofproto->tables);

    hmap_destroy(&ofproto->deletions);

    free(ofproto->vlan_bitmap);

    ofproto->ofproto_class->dealloc(ofproto);
}

void
ofproto_destroy(struct ofproto *p)
{
    struct ofport *ofport, *next_ofport;

    if (!p) {
        return;
    }

    ofproto_flush__(p);
    HMAP_FOR_EACH_SAFE (ofport, next_ofport, hmap_node, &p->ports) {
        ofport_destroy(ofport);
    }

    p->ofproto_class->destruct(p);
    ofproto_destroy__(p);
}

/* Destroys the datapath with the respective 'name' and 'type'.  With the Linux
 * kernel datapath, for example, this destroys the datapath in the kernel, and
 * with the netdev-based datapath, it tears down the data structures that
 * represent the datapath.
 *
 * The datapath should not be currently open as an ofproto. */
int
ofproto_delete(const char *name, const char *type)
{
    const struct ofproto_class *class = ofproto_class_find__(type);
    return (!class ? EAFNOSUPPORT
            : !class->del ? EACCES
            : class->del(type, name));
}

static void
process_port_change(struct ofproto *ofproto, int error, char *devname)
{
    if (error == ENOBUFS) {
        reinit_ports(ofproto);
    } else if (!error) {
        update_port(ofproto, devname);
        free(devname);
    }
}

void 
assign(char *n)
{
    bridge_name = n;
    
    
}

int
ofproto_run(struct ofproto *p)
{
    struct sset changed_netdevs;
    const char *changed_netdev;
    struct ofport *ofport;
    int error;

    
    printf("HNH ofproto_run: \r\n");
    
    error = p->ofproto_class->run(p);
    if (error && error != EAGAIN) {
        VLOG_ERR_RL(&rl, "%s: run failed (%s)", p->name, strerror(error));
    }

    if (p->ofproto_class->port_poll) {
        char *devname;

        while ((error = p->ofproto_class->port_poll(p, &devname)) != EAGAIN) {
            process_port_change(p, error, devname);
        }
    }

    /* Update OpenFlow port status for any port whose netdev has changed.
     *
     * Refreshing a given 'ofport' can cause an arbitrary ofport to be
     * destroyed, so it's not safe to update ports directly from the
     * HMAP_FOR_EACH loop, or even to use HMAP_FOR_EACH_SAFE.  Instead, we
     * need this two-phase approach. */
    sset_init(&changed_netdevs);
    HMAP_FOR_EACH (ofport, hmap_node, &p->ports) {
        unsigned int change_seq = netdev_change_seq(ofport->netdev);
        if (ofport->change_seq != change_seq) {
            ofport->change_seq = change_seq;
            sset_add(&changed_netdevs, netdev_get_name(ofport->netdev));
        }
    }
    SSET_FOR_EACH (changed_netdev, &changed_netdevs) {
        update_port(p, changed_netdev);
    }
    sset_destroy(&changed_netdevs);


    switch (p->state) {
    case S_OPENFLOW:
        connmgr_run(p->connmgr, handle_openflow);
        break;

    case S_FLUSH:
        connmgr_run(p->connmgr, NULL);
        ofproto_flush__(p);
        if (list_is_empty(&p->pending) && hmap_is_empty(&p->deletions)) {
            connmgr_flushed(p->connmgr);
            p->state = S_OPENFLOW;
        }
        break;

    default:
        NOT_REACHED();
    }

    return error;
}

/* Performs periodic activity required by 'ofproto' that needs to be done
 * with the least possible latency.
 *
 * It makes sense to call this function a couple of times per poll loop, to
 * provide a significant performance boost on some benchmarks with the
 * ofproto-dpif implementation. */
int
ofproto_run_fast(struct ofproto *p)
{
    int error;
    //printf("HNH ofproto_run_fast\r\n");
    error = p->ofproto_class->run_fast ? p->ofproto_class->run_fast(p) : 0;
    if (error && error != EAGAIN) {
        VLOG_ERR_RL(&rl, "%s: fastpath run failed (%s)",
                    p->name, strerror(error));
    }
    return error;
}

void
ofproto_wait(struct ofproto *p)
{
    struct ofport *ofport;

    p->ofproto_class->wait(p);
    if (p->ofproto_class->port_poll_wait) {
        p->ofproto_class->port_poll_wait(p);
    }

    HMAP_FOR_EACH (ofport, hmap_node, &p->ports) {
        if (ofport->change_seq != netdev_change_seq(ofport->netdev)) {
            poll_immediate_wake();
        }
    }

    switch (p->state) {
    case S_OPENFLOW:
        connmgr_wait(p->connmgr, true);
        break;

    case S_FLUSH:
        connmgr_wait(p->connmgr, false);
        if (list_is_empty(&p->pending) && hmap_is_empty(&p->deletions)) {
            poll_immediate_wake();
        }
        break;
    }
}

bool
ofproto_is_alive(const struct ofproto *p)
{
    return connmgr_has_controllers(p->connmgr);
}

void
ofproto_get_ofproto_controller_info(const struct ofproto *ofproto,
                                    struct shash *info)
{
    connmgr_get_controller_info(ofproto->connmgr, info);
}

void
ofproto_free_ofproto_controller_info(struct shash *info)
{
    connmgr_free_controller_info(info);
}

/* Makes a deep copy of 'old' into 'port'. */
void
ofproto_port_clone(struct ofproto_port *port, const struct ofproto_port *old)
{
    port->name = xstrdup(old->name);
    port->type = xstrdup(old->type);
    port->ofp_port = old->ofp_port;
}

/* Frees memory allocated to members of 'ofproto_port'.
 *
 * Do not call this function on an ofproto_port obtained from
 * ofproto_port_dump_next(): that function retains ownership of the data in the
 * ofproto_port. */
void
ofproto_port_destroy(struct ofproto_port *ofproto_port)
{
    free(ofproto_port->name);
    free(ofproto_port->type);
}

/* Initializes 'dump' to begin dumping the ports in an ofproto.
 *
 * This function provides no status indication.  An error status for the entire
 * dump operation is provided when it is completed by calling
 * ofproto_port_dump_done().
 */
void
ofproto_port_dump_start(struct ofproto_port_dump *dump,
                        const struct ofproto *ofproto)
{
    dump->ofproto = ofproto;
    dump->error = ofproto->ofproto_class->port_dump_start(ofproto,
                                                          &dump->state);
}

/* Attempts to retrieve another port from 'dump', which must have been created
 * with ofproto_port_dump_start().  On success, stores a new ofproto_port into
 * 'port' and returns true.  On failure, returns false.
 *
 * Failure might indicate an actual error or merely that the last port has been
 * dumped.  An error status for the entire dump operation is provided when it
 * is completed by calling ofproto_port_dump_done().
 *
 * The ofproto owns the data stored in 'port'.  It will remain valid until at
 * least the next time 'dump' is passed to ofproto_port_dump_next() or
 * ofproto_port_dump_done(). */
bool
ofproto_port_dump_next(struct ofproto_port_dump *dump,
                       struct ofproto_port *port)
{
    const struct ofproto *ofproto = dump->ofproto;

    if (dump->error) {
        return false;
    }

    dump->error = ofproto->ofproto_class->port_dump_next(ofproto, dump->state,
                                                         port);
    if (dump->error) {
        ofproto->ofproto_class->port_dump_done(ofproto, dump->state);
        return false;
    }
    return true;
}

/* Completes port table dump operation 'dump', which must have been created
 * with ofproto_port_dump_start().  Returns 0 if the dump operation was
 * error-free, otherwise a positive errno value describing the problem. */
int
ofproto_port_dump_done(struct ofproto_port_dump *dump)
{
    const struct ofproto *ofproto = dump->ofproto;
    if (!dump->error) {
        dump->error = ofproto->ofproto_class->port_dump_done(ofproto,
                                                             dump->state);
    }
    return dump->error == EOF ? 0 : dump->error;
}

/* Attempts to add 'netdev' as a port on 'ofproto'.  If successful, returns 0
 * and sets '*ofp_portp' to the new port's OpenFlow port number (if 'ofp_portp'
 * is non-null).  On failure, returns a positive errno value and sets
 * '*ofp_portp' to OFPP_NONE (if 'ofp_portp' is non-null). */
int
ofproto_port_add(struct ofproto *ofproto, struct netdev *netdev,
                 uint16_t *ofp_portp)
{
    uint16_t ofp_port;
    int error;

    error = ofproto->ofproto_class->port_add(ofproto, netdev, &ofp_port);
    if (!error) {
        update_port(ofproto, netdev_get_name(netdev));
    }
    if (ofp_portp) {
        *ofp_portp = error ? OFPP_NONE : ofp_port;
    }
    return error;
}

/* Looks up a port named 'devname' in 'ofproto'.  On success, returns 0 and
 * initializes '*port' appropriately; on failure, returns a positive errno
 * value.
 *
 * The caller owns the data in 'ofproto_port' and must free it with
 * ofproto_port_destroy() when it is no longer needed. */
int
ofproto_port_query_by_name(const struct ofproto *ofproto, const char *devname,
                           struct ofproto_port *port)
{
    int error;

    error = ofproto->ofproto_class->port_query_by_name(ofproto, devname, port);
    if (error) {
        memset(port, 0, sizeof *port);
    }
    return error;
}

/* Deletes port number 'ofp_port' from the datapath for 'ofproto'.
 * Returns 0 if successful, otherwise a positive errno. */
int
ofproto_port_del(struct ofproto *ofproto, uint16_t ofp_port)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);
    const char *name = ofport ? netdev_get_name(ofport->netdev) : "<unknown>";
    int error;

    error = ofproto->ofproto_class->port_del(ofproto, ofp_port);
    if (!error && ofport) {
        /* 'name' is the netdev's name and update_port() is going to close the
         * netdev.  Just in case update_port() refers to 'name' after it
         * destroys 'ofport', make a copy of it around the update_port()
         * call. */
        char *devname = xstrdup(name);
        update_port(ofproto, devname);
        free(devname);
    }
    return error;
}

/* Adds a flow to OpenFlow flow table 0 in 'p' that matches 'cls_rule' and
 * performs the 'n_actions' actions in 'actions'.  The new flow will not
 * timeout.
 *
 * If cls_rule->priority is in the range of priorities supported by OpenFlow
 * (0...65535, inclusive) then the flow will be visible to OpenFlow
 * controllers; otherwise, it will be hidden.
 *
 * The caller retains ownership of 'cls_rule' and 'actions'.
 *
 * This is a helper function for in-band control and fail-open. */
void
ofproto_add_flow(struct ofproto *ofproto, const struct cls_rule *cls_rule,
                 const union ofp_action *actions, size_t n_actions)
{
    const struct rule *rule;

    rule = rule_from_cls_rule(classifier_find_rule_exactly(
                                    &ofproto->tables[0], cls_rule));
    if (!rule || !ofputil_actions_equal(rule->actions, rule->n_actions,
                                        actions, n_actions)) {
        struct ofputil_flow_mod fm;

        memset(&fm, 0, sizeof fm);
        fm.cr = *cls_rule;
        fm.buffer_id = UINT32_MAX;
        fm.actions = (union ofp_action *) actions;
        fm.n_actions = n_actions;
        add_flow(ofproto, NULL, &fm, NULL);
    }
}

/* Executes the flow modification specified in 'fm'.  Returns 0 on success, an
 * OpenFlow error code as encoded by ofp_mkerr() on failure, or
 * OFPROTO_POSTPONE if the operation cannot be initiated now but may be retried
 * later.
 *
 * This is a helper function for in-band control and fail-open. */
int
ofproto_flow_mod(struct ofproto *ofproto, const struct ofputil_flow_mod *fm)
{
    return handle_flow_mod__(ofproto, NULL, fm, NULL);
}

/* Searches for a rule with matching criteria exactly equal to 'target' in
 * ofproto's table 0 and, if it finds one, deletes it.
 *
 * This is a helper function for in-band control and fail-open. */
bool
ofproto_delete_flow(struct ofproto *ofproto, const struct cls_rule *target)
{
    struct rule *rule;

    rule = rule_from_cls_rule(classifier_find_rule_exactly(
                                  &ofproto->tables[0], target));
    if (!rule) {
        /* No such rule -> success. */
        return true;
    } else if (rule->pending) {
        /* An operation on the rule is already pending -> failure.
         * Caller must retry later if it's important. */
        return false;
    } else {
        /* Initiate deletion -> success. */
        struct ofopgroup *group = ofopgroup_create_unattached(ofproto);
        ofoperation_create(group, rule, OFOPERATION_DELETE);
        classifier_remove(&ofproto->tables[rule->table_id], &rule->cr);
        rule->ofproto->ofproto_class->rule_destruct(rule);
        ofopgroup_submit(group);
        return true;
    }

}

/* Starts the process of deleting all of the flows from all of ofproto's flow
 * tables and then reintroducing the flows required by in-band control and
 * fail-open.  The process will complete in a later call to ofproto_run(). */
void
ofproto_flush_flows(struct ofproto *ofproto)
{
    COVERAGE_INC(ofproto_flush);
    ofproto->state = S_FLUSH;
}

static void
reinit_ports(struct ofproto *p)
{
    struct ofproto_port_dump dump;
    struct sset devnames;
    struct ofport *ofport;
    struct ofproto_port ofproto_port;
    const char *devname;

    COVERAGE_INC(ofproto_reinit_ports);

    sset_init(&devnames);
    HMAP_FOR_EACH (ofport, hmap_node, &p->ports) {
        sset_add(&devnames, netdev_get_name(ofport->netdev));
    }
    OFPROTO_PORT_FOR_EACH (&ofproto_port, &dump, p) {
        sset_add(&devnames, ofproto_port.name);
    }

    SSET_FOR_EACH (devname, &devnames) {
        update_port(p, devname);
    }
    sset_destroy(&devnames);
}

/* Opens and returns a netdev for 'ofproto_port', or a null pointer if the
 * netdev cannot be opened.  On success, also fills in 'opp'.  */
static struct netdev *
ofport_open(const struct ofproto_port *ofproto_port, struct ofp_phy_port *opp)
{
    uint32_t curr, advertised, supported, peer;
    enum netdev_flags flags;
    struct netdev *netdev;
    int error;

    error = netdev_open(ofproto_port->name, ofproto_port->type, &netdev);
    if (error) {
        VLOG_WARN_RL(&rl, "ignoring port %s (%"PRIu16") because netdev %s "
                     "cannot be opened (%s)",
                     ofproto_port->name, ofproto_port->ofp_port,
                     ofproto_port->name, strerror(error));
        return NULL;
    }

    netdev_get_flags(netdev, &flags);
    netdev_get_features(netdev, &curr, &advertised, &supported, &peer);

    opp->port_no = htons(ofproto_port->ofp_port);
    netdev_get_etheraddr(netdev, opp->hw_addr);
    ovs_strzcpy(opp->name, ofproto_port->name, sizeof opp->name);
    opp->config = flags & NETDEV_UP ? 0 : htonl(OFPPC_PORT_DOWN);
    opp->state = netdev_get_carrier(netdev) ? 0 : htonl(OFPPS_LINK_DOWN);
    opp->curr = htonl(curr);
    opp->advertised = htonl(advertised);
    opp->supported = htonl(supported);
    opp->peer = htonl(peer);

    return netdev;
}

/* Returns true if most fields of 'a' and 'b' are equal.  Differences in name,
 * port number, and 'config' bits other than OFPPC_PORT_DOWN are
 * disregarded. */
static bool
ofport_equal(const struct ofp_phy_port *a, const struct ofp_phy_port *b)
{
    BUILD_ASSERT_DECL(sizeof *a == 48); /* Detect ofp_phy_port changes. */
    return (!memcmp(a->hw_addr, b->hw_addr, sizeof a->hw_addr)
            && a->state == b->state
            && !((a->config ^ b->config) & htonl(OFPPC_PORT_DOWN))
            && a->curr == b->curr
            && a->advertised == b->advertised
            && a->supported == b->supported
            && a->peer == b->peer);
}

/* Adds an ofport to 'p' initialized based on the given 'netdev' and 'opp'.
 * The caller must ensure that 'p' does not have a conflicting ofport (that is,
 * one with the same name or port number). */
static void
ofport_install(struct ofproto *p,
               struct netdev *netdev, const struct ofp_phy_port *opp)
{
    const char *netdev_name = netdev_get_name(netdev);
    struct ofport *ofport;
    int dev_mtu;
    int error;

    /* Create ofport. */
    ofport = p->ofproto_class->port_alloc();
    if (!ofport) {
        error = ENOMEM;
        goto error;
    }
    ofport->ofproto = p;
    ofport->netdev = netdev;
    ofport->change_seq = netdev_change_seq(netdev);
    ofport->opp = *opp;
    ofport->ofp_port = ntohs(opp->port_no);

    /* Add port to 'p'. */
    hmap_insert(&p->ports, &ofport->hmap_node, hash_int(ofport->ofp_port, 0));
    shash_add(&p->port_by_name, netdev_name, ofport);

    if (!netdev_get_mtu(netdev, &dev_mtu)) {
        ofport->mtu = dev_mtu;
        set_internal_devs_mtu(p);
    } else {
        ofport->mtu = 0;
    }

    /* Let the ofproto_class initialize its private data. */
    error = p->ofproto_class->port_construct(ofport);
    if (error) {
        goto error;
    }
    connmgr_send_port_status(p->connmgr, opp, OFPPR_ADD);
    return;

error:
    VLOG_WARN_RL(&rl, "%s: could not add port %s (%s)",
                 p->name, netdev_name, strerror(error));
    if (ofport) {
        ofport_destroy__(ofport);
    } else {
        netdev_close(netdev);
    }
}

/* Removes 'ofport' from 'p' and destroys it. */
static void
ofport_remove(struct ofport *ofport)
{
    connmgr_send_port_status(ofport->ofproto->connmgr, &ofport->opp,
                             OFPPR_DELETE);
    ofport_destroy(ofport);
}

/* If 'ofproto' contains an ofport named 'name', removes it from 'ofproto' and
 * destroys it. */
static void
ofport_remove_with_name(struct ofproto *ofproto, const char *name)
{
    struct ofport *port = shash_find_data(&ofproto->port_by_name, name);
    if (port) {
        ofport_remove(port);
    }
}

/* Updates 'port' with new 'opp' description.
 *
 * Does not handle a name or port number change.  The caller must implement
 * such a change as a delete followed by an add.  */
static void
ofport_modified(struct ofport *port, struct ofp_phy_port *opp)
{
    memcpy(port->opp.hw_addr, opp->hw_addr, ETH_ADDR_LEN);
    port->opp.config = ((port->opp.config & ~htonl(OFPPC_PORT_DOWN))
                        | (opp->config & htonl(OFPPC_PORT_DOWN)));
    port->opp.state = opp->state;
    port->opp.curr = opp->curr;
    port->opp.advertised = opp->advertised;
    port->opp.supported = opp->supported;
    port->opp.peer = opp->peer;

    connmgr_send_port_status(port->ofproto->connmgr, &port->opp, OFPPR_MODIFY);
}

/* Update OpenFlow 'state' in 'port' and notify controller. */
void
ofproto_port_set_state(struct ofport *port, ovs_be32 state)
{
    if (port->opp.state != state) {
        port->opp.state = state;
        connmgr_send_port_status(port->ofproto->connmgr, &port->opp,
                                 OFPPR_MODIFY);
    }
}

void
ofproto_port_unregister(struct ofproto *ofproto, uint16_t ofp_port)
{
    struct ofport *port = ofproto_get_port(ofproto, ofp_port);
    if (port) {
        if (port->ofproto->ofproto_class->set_realdev) {
            port->ofproto->ofproto_class->set_realdev(port, 0, 0);
        }
        if (port->ofproto->ofproto_class->set_stp_port) {
            port->ofproto->ofproto_class->set_stp_port(port, NULL);
        }
        if (port->ofproto->ofproto_class->set_cfm) {
            port->ofproto->ofproto_class->set_cfm(port, NULL);
        }
        if (port->ofproto->ofproto_class->bundle_remove) {
            port->ofproto->ofproto_class->bundle_remove(port);
        }
    }
}

static void
ofport_destroy__(struct ofport *port)
{
    struct ofproto *ofproto = port->ofproto;
    const char *name = netdev_get_name(port->netdev);

    hmap_remove(&ofproto->ports, &port->hmap_node);
    shash_delete(&ofproto->port_by_name,
                 shash_find(&ofproto->port_by_name, name));

    netdev_close(port->netdev);
    ofproto->ofproto_class->port_dealloc(port);
}

static void
ofport_destroy(struct ofport *port)
{
    if (port) {
        port->ofproto->ofproto_class->port_destruct(port);
        ofport_destroy__(port);
     }
}

struct ofport *
ofproto_get_port(const struct ofproto *ofproto, uint16_t ofp_port)
{
    struct ofport *port;

    HMAP_FOR_EACH_IN_BUCKET (port, hmap_node,
                             hash_int(ofp_port, 0), &ofproto->ports) {
        if (port->ofp_port == ofp_port) {
            return port;
        }
    }
    return NULL;
}

static void
update_port(struct ofproto *ofproto, const char *name)
{
    struct ofproto_port ofproto_port;
    struct ofp_phy_port opp;
    struct netdev *netdev;
    struct ofport *port;

    COVERAGE_INC(ofproto_update_port);

    /* Fetch 'name''s location and properties from the datapath. */
    netdev = (!ofproto_port_query_by_name(ofproto, name, &ofproto_port)
              ? ofport_open(&ofproto_port, &opp)
              : NULL);
    if (netdev) {
        port = ofproto_get_port(ofproto, ofproto_port.ofp_port);
        if (port && !strcmp(netdev_get_name(port->netdev), name)) {
            struct netdev *old_netdev = port->netdev;
            int dev_mtu;

            /* 'name' hasn't changed location.  Any properties changed? */
            if (!ofport_equal(&port->opp, &opp)) {
                ofport_modified(port, &opp);
            }

            /* If this is a non-internal port and the MTU changed, check
             * if the datapath's MTU needs to be updated. */
            if (strcmp(netdev_get_type(netdev), "internal")
                    && !netdev_get_mtu(netdev, &dev_mtu)
                    && port->mtu != dev_mtu) {
                set_internal_devs_mtu(ofproto);
                port->mtu = dev_mtu;
            }

            /* Install the newly opened netdev in case it has changed.
             * Don't close the old netdev yet in case port_modified has to
             * remove a retained reference to it.*/
            port->netdev = netdev;
            port->change_seq = netdev_change_seq(netdev);

            if (port->ofproto->ofproto_class->port_modified) {
                port->ofproto->ofproto_class->port_modified(port);
            }

            netdev_close(old_netdev);
        } else {
            /* If 'port' is nonnull then its name differs from 'name' and thus
             * we should delete it.  If we think there's a port named 'name'
             * then its port number must be wrong now so delete it too. */
            if (port) {
                ofport_remove(port);
            }
            ofport_remove_with_name(ofproto, name);
            ofport_install(ofproto, netdev, &opp);
        }
    } else {
        /* Any port named 'name' is gone now. */
        ofport_remove_with_name(ofproto, name);
    }
    ofproto_port_destroy(&ofproto_port);
}

static int
init_ports(struct ofproto *p)
{
    struct ofproto_port_dump dump;
    struct ofproto_port ofproto_port;

    OFPROTO_PORT_FOR_EACH (&ofproto_port, &dump, p) {
        uint16_t ofp_port = ofproto_port.ofp_port;
        if (ofproto_get_port(p, ofp_port)) {
            VLOG_WARN_RL(&rl, "ignoring duplicate port %"PRIu16" in datapath",
                         ofp_port);
        } else if (shash_find(&p->port_by_name, ofproto_port.name)) {
            VLOG_WARN_RL(&rl, "ignoring duplicate device %s in datapath",
                         ofproto_port.name);
        } else {
            struct ofp_phy_port opp;
            struct netdev *netdev;

            netdev = ofport_open(&ofproto_port, &opp);
            if (netdev) {
                ofport_install(p, netdev, &opp);
            }
        }
    }

    return 0;
}

/* Find the minimum MTU of all non-datapath devices attached to 'p'.
 * Returns ETH_PAYLOAD_MAX or the minimum of the ports. */
static int
find_min_mtu(struct ofproto *p)
{
    struct ofport *ofport;
    int mtu = 0;

    HMAP_FOR_EACH (ofport, hmap_node, &p->ports) {
        struct netdev *netdev = ofport->netdev;
        int dev_mtu;

        /* Skip any internal ports, since that's what we're trying to
         * set. */
        if (!strcmp(netdev_get_type(netdev), "internal")) {
            continue;
        }

        if (netdev_get_mtu(netdev, &dev_mtu)) {
            continue;
        }
        if (!mtu || dev_mtu < mtu) {
            mtu = dev_mtu;
        }
    }

    return mtu ? mtu: ETH_PAYLOAD_MAX;
}

/* Set the MTU of all datapath devices on 'p' to the minimum of the
 * non-datapath ports. */
static void
set_internal_devs_mtu(struct ofproto *p)
{
    struct ofport *ofport;
    int mtu = find_min_mtu(p);

    HMAP_FOR_EACH (ofport, hmap_node, &p->ports) {
        struct netdev *netdev = ofport->netdev;

        if (!strcmp(netdev_get_type(netdev), "internal")) {
            netdev_set_mtu(netdev, mtu);
        }
    }
}

static void
ofproto_rule_destroy__(struct rule *rule)
{
    free(rule->actions);
    rule->ofproto->ofproto_class->rule_dealloc(rule);
}

/* This function allows an ofproto implementation to destroy any rules that
 * remain when its ->destruct() function is called.  The caller must have
 * already uninitialized any derived members of 'rule' (step 5 described in the
 * large comment in ofproto/ofproto-provider.h titled "Life Cycle").
 * This function implements steps 6 and 7.
 *
 * This function should only be called from an ofproto implementation's
 * ->destruct() function.  It is not suitable elsewhere. */
void
ofproto_rule_destroy(struct rule *rule)
{
    assert(!rule->pending);
    classifier_remove(&rule->ofproto->tables[rule->table_id], &rule->cr);
    ofproto_rule_destroy__(rule);
}

/* Returns true if 'rule' has an OpenFlow OFPAT_OUTPUT or OFPAT_ENQUEUE action
 * that outputs to 'out_port' (output to OFPP_FLOOD and OFPP_ALL doesn't
 * count). */
static bool
rule_has_out_port(const struct rule *rule, uint16_t out_port)
{
    const union ofp_action *oa;
    size_t left;

    if (out_port == OFPP_NONE) {
        return true;
    }
    OFPUTIL_ACTION_FOR_EACH_UNSAFE (oa, left, rule->actions, rule->n_actions) {
        if (action_outputs_to_port(oa, htons(out_port))) {
            return true;
        }
    }
    return false;
}

/* Executes the actions indicated by 'rule' on 'packet' and credits 'rule''s
 * statistics appropriately.  'packet' must have at least sizeof(struct
 * ofp_packet_in) bytes of headroom.
 *
 * 'packet' doesn't necessarily have to match 'rule'.  'rule' will be credited
 * with statistics for 'packet' either way.
 *
 * Takes ownership of 'packet'. */
static int
rule_execute(struct rule *rule, uint16_t in_port, struct ofpbuf *packet)
{
    struct flow flow;

    assert(ofpbuf_headroom(packet) >= sizeof(struct ofp_packet_in));

    flow_extract(packet, 0, 0, in_port, &flow);
    return rule->ofproto->ofproto_class->rule_execute(rule, &flow, packet);
}

/* Returns true if 'rule' should be hidden from the controller.
 *
 * Rules with priority higher than UINT16_MAX are set up by ofproto itself
 * (e.g. by in-band control) and are intentionally hidden from the
 * controller. */
static bool
rule_is_hidden(const struct rule *rule)
{
    return rule->cr.priority > UINT16_MAX;
}

static int
handle_echo_request(struct ofconn *ofconn, const struct ofp_header *oh)
{
    ofconn_send_reply(ofconn, make_echo_reply(oh));
    return 0;
}

static int
handle_features_request(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    struct ofp_switch_features *osf;
    struct ofpbuf *buf;
    struct ofport *port;
    bool arp_match_ip;
    uint32_t actions;

    ofproto->ofproto_class->get_features(ofproto, &arp_match_ip, &actions);
    assert(actions & (1 << OFPAT_OUTPUT)); /* sanity check */

    osf = make_openflow_xid(sizeof *osf, OFPT_FEATURES_REPLY, oh->xid, &buf);
    osf->datapath_id = htonll(ofproto->datapath_id);
    osf->n_buffers = htonl(pktbuf_capacity());
    osf->n_tables = ofproto->n_tables;
    osf->capabilities = htonl(OFPC_FLOW_STATS | OFPC_TABLE_STATS |
                              OFPC_PORT_STATS | OFPC_QUEUE_STATS);
    if (arp_match_ip) {
        osf->capabilities |= htonl(OFPC_ARP_MATCH_IP);
    }
    osf->actions = htonl(actions);

    HMAP_FOR_EACH (port, hmap_node, &ofproto->ports) {
        ofpbuf_put(buf, &port->opp, sizeof port->opp);
    }

    ofconn_send_reply(ofconn, buf);
    return 0;
}

static int
handle_get_config_request(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    struct ofp_switch_config *osc;
    struct ofpbuf *buf;

    /* Send reply. */
    osc = make_openflow_xid(sizeof *osc, OFPT_GET_CONFIG_REPLY, oh->xid, &buf);
    osc->flags = htons(ofproto->frag_handling);
    osc->miss_send_len = htons(ofconn_get_miss_send_len(ofconn));
    ofconn_send_reply(ofconn, buf);

    return 0;
}

static int
handle_set_config(struct ofconn *ofconn, const struct ofp_switch_config *osc)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    uint16_t flags = ntohs(osc->flags);

    if (ofconn_get_type(ofconn) != OFCONN_PRIMARY
        || ofconn_get_role(ofconn) != NX_ROLE_SLAVE) {
        enum ofp_config_flags cur = ofproto->frag_handling;
        enum ofp_config_flags next = flags & OFPC_FRAG_MASK;

        assert((cur & OFPC_FRAG_MASK) == cur);
        if (cur != next) {
            if (ofproto->ofproto_class->set_frag_handling(ofproto, next)) {
                ofproto->frag_handling = next;
            } else {
                VLOG_WARN_RL(&rl, "%s: unsupported fragment handling mode %s",
                             ofproto->name,
                             ofputil_frag_handling_to_string(next));
            }
        }
    }

    ofconn_set_miss_send_len(ofconn, ntohs(osc->miss_send_len));

    return 0;
}

/* Checks whether 'ofconn' is a slave controller.  If so, returns an OpenFlow
 * error message code (composed with ofp_mkerr()) for the caller to propagate
 * upward.  Otherwise, returns 0. */
static int
reject_slave_controller(const struct ofconn *ofconn)
{
    if (ofconn_get_type(ofconn) == OFCONN_PRIMARY
        && ofconn_get_role(ofconn) == NX_ROLE_SLAVE) {
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_EPERM);
    } else {
        return 0;
    }
}

static int
handle_packet_out(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct ofproto *p = ofconn_get_ofproto(ofconn);
    struct ofp_packet_out *opo;
    struct ofpbuf payload, *buffer;
    union ofp_action *ofp_actions;
    struct ofpbuf request;
    struct flow flow;
    size_t n_ofp_actions;
    uint16_t in_port;
    int error;

    COVERAGE_INC(ofproto_packet_out);

    error = reject_slave_controller(ofconn);
    if (error) {
        return error;
    }

    /* Get ofp_packet_out. */
    ofpbuf_use_const(&request, oh, ntohs(oh->length));
    opo = ofpbuf_pull(&request, offsetof(struct ofp_packet_out, actions));

    /* Get actions. */
    error = ofputil_pull_actions(&request, ntohs(opo->actions_len),
                                 &ofp_actions, &n_ofp_actions);
    if (error) {
        return error;
    }

    /* Get payload. */
    if (opo->buffer_id != htonl(UINT32_MAX)) {
        error = ofconn_pktbuf_retrieve(ofconn, ntohl(opo->buffer_id),
                                       &buffer, NULL);
        if (error || !buffer) {
            return error;
        }
        payload = *buffer;
    } else {
        payload = request;
        buffer = NULL;
    }

    /* Get in_port and partially validate it.
     *
     * We don't know what range of ports the ofproto actually implements, but
     * we do know that only certain reserved ports (numbered OFPP_MAX and
     * above) are valid. */
    in_port = ntohs(opo->in_port);
    if (in_port >= OFPP_MAX && in_port != OFPP_LOCAL && in_port != OFPP_NONE
        && in_port != OFPP_CONTROLLER) {
        return ofp_mkerr_nicira(OFPET_BAD_REQUEST, NXBRC_BAD_IN_PORT);
    }

    /* Send out packet. */
    flow_extract(&payload, 0, 0, in_port, &flow);
    error = p->ofproto_class->packet_out(p, &payload, &flow,
                                         ofp_actions, n_ofp_actions);
    ofpbuf_delete(buffer);

    return error;
}

static void
update_port_config(struct ofport *port, ovs_be32 config, ovs_be32 mask)
{
    ovs_be32 old_config = port->opp.config;

    mask &= config ^ port->opp.config;
    if (mask & htonl(OFPPC_PORT_DOWN)) {
        if (config & htonl(OFPPC_PORT_DOWN)) {
            netdev_turn_flags_off(port->netdev, NETDEV_UP, true);
        } else {
            netdev_turn_flags_on(port->netdev, NETDEV_UP, true);
        }
    }

    port->opp.config ^= mask & (htonl(OFPPC_NO_RECV | OFPPC_NO_RECV_STP |
                                      OFPPC_NO_FLOOD | OFPPC_NO_FWD |
                                      OFPPC_NO_PACKET_IN));
    if (port->opp.config != old_config) {
        port->ofproto->ofproto_class->port_reconfigured(port, old_config);
    }
}

static int
handle_port_mod(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct ofproto *p = ofconn_get_ofproto(ofconn);
    const struct ofp_port_mod *opm = (const struct ofp_port_mod *) oh;
    struct ofport *port;
    int error;

    error = reject_slave_controller(ofconn);
    if (error) {
        return error;
    }

    port = ofproto_get_port(p, ntohs(opm->port_no));
    if (!port) {
        return ofp_mkerr(OFPET_PORT_MOD_FAILED, OFPPMFC_BAD_PORT);
    } else if (memcmp(port->opp.hw_addr, opm->hw_addr, OFP_ETH_ALEN)) {
        return ofp_mkerr(OFPET_PORT_MOD_FAILED, OFPPMFC_BAD_HW_ADDR);
    } else {
        update_port_config(port, opm->config, opm->mask);
        if (opm->advertise) {
            netdev_set_advertisements(port->netdev, ntohl(opm->advertise));
        }
    }
    return 0;
}

static int
handle_desc_stats_request(struct ofconn *ofconn,
                          const struct ofp_stats_msg *request)
{
    struct ofproto *p = ofconn_get_ofproto(ofconn);
    struct ofp_desc_stats *ods;
    struct ofpbuf *msg;

    ods = ofputil_make_stats_reply(sizeof *ods, request, &msg);
    ovs_strlcpy(ods->mfr_desc, p->mfr_desc, sizeof ods->mfr_desc);
    ovs_strlcpy(ods->hw_desc, p->hw_desc, sizeof ods->hw_desc);
    ovs_strlcpy(ods->sw_desc, p->sw_desc, sizeof ods->sw_desc);
    ovs_strlcpy(ods->serial_num, p->serial_desc, sizeof ods->serial_num);
    ovs_strlcpy(ods->dp_desc, p->dp_desc, sizeof ods->dp_desc);
    ofconn_send_reply(ofconn, msg);

    return 0;
}

static int
handle_table_stats_request(struct ofconn *ofconn,
                           const struct ofp_stats_msg *request)
{
    struct ofproto *p = ofconn_get_ofproto(ofconn);
    struct ofp_table_stats *ots;
    struct ofpbuf *msg;
    size_t i;

    ofputil_make_stats_reply(sizeof(struct ofp_stats_msg), request, &msg);

    ots = ofpbuf_put_zeros(msg, sizeof *ots * p->n_tables);
    for (i = 0; i < p->n_tables; i++) {
        ots[i].table_id = i;
        sprintf(ots[i].name, "table%zu", i);
        ots[i].wildcards = htonl(OFPFW_ALL);
        ots[i].max_entries = htonl(1000000); /* An arbitrary big number. */
        ots[i].active_count = htonl(classifier_count(&p->tables[i]));
    }

    p->ofproto_class->get_tables(p, ots);

    ofconn_send_reply(ofconn, msg);
    return 0;
}

static void
append_port_stat(struct ofport *port, struct list *replies)
{
    struct netdev_stats stats;
    struct ofp_port_stats *ops;

    /* Intentionally ignore return value, since errors will set
     * 'stats' to all-1s, which is correct for OpenFlow, and
     * netdev_get_stats() will log errors. */
    netdev_get_stats(port->netdev, &stats);

    ops = ofputil_append_stats_reply(sizeof *ops, replies);
    ops->port_no = port->opp.port_no;
    memset(ops->pad, 0, sizeof ops->pad);
    put_32aligned_be64(&ops->rx_packets, htonll(stats.rx_packets));
    put_32aligned_be64(&ops->tx_packets, htonll(stats.tx_packets));
    put_32aligned_be64(&ops->rx_bytes, htonll(stats.rx_bytes));
    put_32aligned_be64(&ops->tx_bytes, htonll(stats.tx_bytes));
    put_32aligned_be64(&ops->rx_dropped, htonll(stats.rx_dropped));
    put_32aligned_be64(&ops->tx_dropped, htonll(stats.tx_dropped));
    put_32aligned_be64(&ops->rx_errors, htonll(stats.rx_errors));
    put_32aligned_be64(&ops->tx_errors, htonll(stats.tx_errors));
    put_32aligned_be64(&ops->rx_frame_err, htonll(stats.rx_frame_errors));
    put_32aligned_be64(&ops->rx_over_err, htonll(stats.rx_over_errors));
    put_32aligned_be64(&ops->rx_crc_err, htonll(stats.rx_crc_errors));
    put_32aligned_be64(&ops->collisions, htonll(stats.collisions));
}

static int
handle_port_stats_request(struct ofconn *ofconn,
                          const struct ofp_port_stats_request *psr)
{
    struct ofproto *p = ofconn_get_ofproto(ofconn);
    struct ofport *port;
    struct list replies;

    ofputil_start_stats_reply(&psr->osm, &replies);
    if (psr->port_no != htons(OFPP_NONE)) {
        port = ofproto_get_port(p, ntohs(psr->port_no));
        if (port) {
            append_port_stat(port, &replies);
        }
    } else {
        HMAP_FOR_EACH (port, hmap_node, &p->ports) {
            append_port_stat(port, &replies);
        }
    }

    ofconn_send_replies(ofconn, &replies);
    return 0;
}

static void
calc_flow_duration__(long long int start, uint32_t *sec, uint32_t *nsec)
{
    long long int msecs = time_msec() - start;
    *sec = msecs / 1000;
    *nsec = (msecs % 1000) * (1000 * 1000);
}

/* Checks whether 'table_id' is 0xff or a valid table ID in 'ofproto'.  Returns
 * 0 if 'table_id' is OK, otherwise an OpenFlow error code.  */
static int
check_table_id(const struct ofproto *ofproto, uint8_t table_id)
{
    return (table_id == 0xff || table_id < ofproto->n_tables
            ? 0
            : ofp_mkerr_nicira(OFPET_BAD_REQUEST, NXBRC_BAD_TABLE_ID));

}

static struct classifier *
first_matching_table(struct ofproto *ofproto, uint8_t table_id)
{
    if (table_id == 0xff) {
        return &ofproto->tables[0];
    } else if (table_id < ofproto->n_tables) {
        return &ofproto->tables[table_id];
    } else {
        return NULL;
    }
}

static struct classifier *
next_matching_table(struct ofproto *ofproto,
                    struct classifier *cls, uint8_t table_id)
{
    return (table_id == 0xff && cls != &ofproto->tables[ofproto->n_tables - 1]
            ? cls + 1
            : NULL);
}

/* Assigns CLS to each classifier table, in turn, that matches TABLE_ID in
 * OFPROTO:
 *
 *   - If TABLE_ID is 0xff, this iterates over every classifier table in
 *     OFPROTO.
 *
 *   - If TABLE_ID is the number of a table in OFPROTO, then the loop iterates
 *     only once, for that table.
 *
 *   - Otherwise, TABLE_ID isn't valid for OFPROTO, so the loop won't be
 *     entered at all.  (Perhaps you should have validated TABLE_ID with
 *     check_table_id().)
 *
 * All parameters are evaluated multiple times.
 */
#define FOR_EACH_MATCHING_TABLE(CLS, TABLE_ID, OFPROTO)         \
    for ((CLS) = first_matching_table(OFPROTO, TABLE_ID);       \
         (CLS) != NULL;                                         \
         (CLS) = next_matching_table(OFPROTO, CLS, TABLE_ID))

/* Searches 'ofproto' for rules in table 'table_id' (or in all tables, if
 * 'table_id' is 0xff) that match 'match' in the "loose" way required for
 * OpenFlow OFPFC_MODIFY and OFPFC_DELETE requests and puts them on list
 * 'rules'.
 *
 * If 'out_port' is anything other than OFPP_NONE, then only rules that output
 * to 'out_port' are included.
 *
 * Hidden rules are always omitted.
 *
 * Returns 0 on success, otherwise an OpenFlow error code. */
static int
collect_rules_loose(struct ofproto *ofproto, uint8_t table_id,
                    const struct cls_rule *match, uint16_t out_port,
                    struct list *rules)
{
    struct classifier *cls;
    int error;

    error = check_table_id(ofproto, table_id);
    if (error) {
        return error;
    }

    list_init(rules);
    FOR_EACH_MATCHING_TABLE (cls, table_id, ofproto) {
        struct cls_cursor cursor;
        struct rule *rule;

        cls_cursor_init(&cursor, cls, match);
        CLS_CURSOR_FOR_EACH (rule, cr, &cursor) {
            if (rule->pending) {
                return OFPROTO_POSTPONE;
            }
            if (!rule_is_hidden(rule) && rule_has_out_port(rule, out_port)) {
                list_push_back(rules, &rule->ofproto_node);
            }
        }
    }
    return 0;
}

/* Searches 'ofproto' for rules in table 'table_id' (or in all tables, if
 * 'table_id' is 0xff) that match 'match' in the "strict" way required for
 * OpenFlow OFPFC_MODIFY_STRICT and OFPFC_DELETE_STRICT requests and puts them
 * on list 'rules'.
 *
 * If 'out_port' is anything other than OFPP_NONE, then only rules that output
 * to 'out_port' are included.
 *
 * Hidden rules are always omitted.
 *
 * Returns 0 on success, otherwise an OpenFlow error code. */
static int
collect_rules_strict(struct ofproto *ofproto, uint8_t table_id,
                     const struct cls_rule *match, uint16_t out_port,
                     struct list *rules)
{
    struct classifier *cls;
    int error;

    error = check_table_id(ofproto, table_id);
    if (error) {
        return error;
    }

    list_init(rules);
    FOR_EACH_MATCHING_TABLE (cls, table_id, ofproto) {
        struct rule *rule;

        rule = rule_from_cls_rule(classifier_find_rule_exactly(cls, match));
        if (rule) {
            if (rule->pending) {
                return OFPROTO_POSTPONE;
            }
            if (!rule_is_hidden(rule) && rule_has_out_port(rule, out_port)) {
                list_push_back(rules, &rule->ofproto_node);
            }
        }
    }
    return 0;
}

static int
handle_flow_stats_request(struct ofconn *ofconn,
                          const struct ofp_stats_msg *osm)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    struct ofputil_flow_stats_request fsr;
    struct list replies;
    struct list rules;
    struct rule *rule;
    int error;

    error = ofputil_decode_flow_stats_request(&fsr, &osm->header);
    if (error) {
        return error;
    }

    error = collect_rules_loose(ofproto, fsr.table_id, &fsr.match,
                                fsr.out_port, &rules);
    if (error) {
        return error;
    }

    ofputil_start_stats_reply(osm, &replies);
    LIST_FOR_EACH (rule, ofproto_node, &rules) {
        struct ofputil_flow_stats fs;

        fs.rule = rule->cr;
        fs.cookie = rule->flow_cookie;
        fs.table_id = rule->table_id;
        calc_flow_duration__(rule->created, &fs.duration_sec,
                             &fs.duration_nsec);
        fs.idle_timeout = rule->idle_timeout;
        fs.hard_timeout = rule->hard_timeout;
        ofproto->ofproto_class->rule_get_stats(rule, &fs.packet_count,
                                               &fs.byte_count);
        fs.actions = rule->actions;
        fs.n_actions = rule->n_actions;
        ofputil_append_flow_stats_reply(&fs, &replies);
    }
    ofconn_send_replies(ofconn, &replies);

    return 0;
}

static void
flow_stats_ds(struct rule *rule, struct ds *results)
{
    uint64_t packet_count, byte_count;

    rule->ofproto->ofproto_class->rule_get_stats(rule,
                                                 &packet_count, &byte_count);

    if (rule->table_id != 0) {
        ds_put_format(results, "table_id=%"PRIu8", ", rule->table_id);
    }
    ds_put_format(results, "duration=%llds, ",
                  (time_msec() - rule->created) / 1000);
    ds_put_format(results, "priority=%u, ", rule->cr.priority);
    ds_put_format(results, "n_packets=%"PRIu64", ", packet_count);
    ds_put_format(results, "n_bytes=%"PRIu64", ", byte_count);
    cls_rule_format(&rule->cr, results);
    ds_put_char(results, ',');
    if (rule->n_actions > 0) {
        ofp_print_actions(results, rule->actions, rule->n_actions);
    } else {
        ds_put_cstr(results, "drop");
    }
    ds_put_cstr(results, "\n");
}

/* Adds a pretty-printed description of all flows to 'results', including
 * hidden flows (e.g., set up by in-band control). */
void
ofproto_get_all_flows(struct ofproto *p, struct ds *results)
{
    struct classifier *cls;

    OFPROTO_FOR_EACH_TABLE (cls, p) {
        struct cls_cursor cursor;
        struct rule *rule;

        cls_cursor_init(&cursor, cls, NULL);
        CLS_CURSOR_FOR_EACH (rule, cr, &cursor) {
            flow_stats_ds(rule, results);
        }
    }
}

/* Obtains the NetFlow engine type and engine ID for 'ofproto' into
 * '*engine_type' and '*engine_id', respectively. */
void
ofproto_get_netflow_ids(const struct ofproto *ofproto,
                        uint8_t *engine_type, uint8_t *engine_id)
{
    ofproto->ofproto_class->get_netflow_ids(ofproto, engine_type, engine_id);
}

/* Checks the fault status of CFM for 'ofp_port' within 'ofproto'.  Returns 1
 * if CFM is faulted (generally indiciating a connectivity problem), 0 if CFM
 * is not faulted, and -1 if CFM is not enabled on 'ofp_port'. */
int
ofproto_port_get_cfm_fault(const struct ofproto *ofproto, uint16_t ofp_port)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);
    return (ofport && ofproto->ofproto_class->get_cfm_fault
            ? ofproto->ofproto_class->get_cfm_fault(ofport)
            : -1);
}

/* Gets the MPIDs of the remote maintenance points broadcasting to 'ofp_port'
 * within 'ofproto'.  Populates 'rmps' with an array of MPIDs owned by
 * 'ofproto', and 'n_rmps' with the number of MPIDs in 'rmps'.  Returns a
 * number less than 0 if CFM is not enabled on 'ofp_port'. */
int
ofproto_port_get_cfm_remote_mpids(const struct ofproto *ofproto,
                                  uint16_t ofp_port, const uint64_t **rmps,
                                  size_t *n_rmps)
{
    struct ofport *ofport = ofproto_get_port(ofproto, ofp_port);

    *rmps = NULL;
    *n_rmps = 0;
    return (ofport && ofproto->ofproto_class->get_cfm_remote_mpids
            ? ofproto->ofproto_class->get_cfm_remote_mpids(ofport, rmps,
                                                           n_rmps)
            : -1);
}

static int
handle_aggregate_stats_request(struct ofconn *ofconn,
                               const struct ofp_stats_msg *osm)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    struct ofputil_flow_stats_request request;
    struct ofputil_aggregate_stats stats;
    bool unknown_packets, unknown_bytes;
    struct ofpbuf *reply;
    struct list rules;
    struct rule *rule;
    int error;

    error = ofputil_decode_flow_stats_request(&request, &osm->header);
    if (error) {
        return error;
    }

    error = collect_rules_loose(ofproto, request.table_id, &request.match,
                                request.out_port, &rules);
    if (error) {
        return error;
    }

    memset(&stats, 0, sizeof stats);
    unknown_packets = unknown_bytes = false;
    LIST_FOR_EACH (rule, ofproto_node, &rules) {
        uint64_t packet_count;
        uint64_t byte_count;

        ofproto->ofproto_class->rule_get_stats(rule, &packet_count,
                                               &byte_count);

        if (packet_count == UINT64_MAX) {
            unknown_packets = true;
        } else {
            stats.packet_count += packet_count;
        }

        if (byte_count == UINT64_MAX) {
            unknown_bytes = true;
        } else {
            stats.byte_count += byte_count;
        }

        stats.flow_count++;
    }
    if (unknown_packets) {
        stats.packet_count = UINT64_MAX;
    }
    if (unknown_bytes) {
        stats.byte_count = UINT64_MAX;
    }

    reply = ofputil_encode_aggregate_stats_reply(&stats, osm);
    ofconn_send_reply(ofconn, reply);

    return 0;
}

struct queue_stats_cbdata {
    struct ofport *ofport;
    struct list replies;
};

static void
put_queue_stats(struct queue_stats_cbdata *cbdata, uint32_t queue_id,
                const struct netdev_queue_stats *stats)
{
    struct ofp_queue_stats *reply;

    reply = ofputil_append_stats_reply(sizeof *reply, &cbdata->replies);
    reply->port_no = cbdata->ofport->opp.port_no;
    memset(reply->pad, 0, sizeof reply->pad);
    reply->queue_id = htonl(queue_id);
    put_32aligned_be64(&reply->tx_bytes, htonll(stats->tx_bytes));
    put_32aligned_be64(&reply->tx_packets, htonll(stats->tx_packets));
    put_32aligned_be64(&reply->tx_errors, htonll(stats->tx_errors));
}

static void
handle_queue_stats_dump_cb(uint32_t queue_id,
                           struct netdev_queue_stats *stats,
                           void *cbdata_)
{
    struct queue_stats_cbdata *cbdata = cbdata_;

    put_queue_stats(cbdata, queue_id, stats);
}

static void
handle_queue_stats_for_port(struct ofport *port, uint32_t queue_id,
                            struct queue_stats_cbdata *cbdata)
{
    cbdata->ofport = port;
    if (queue_id == OFPQ_ALL) {
        netdev_dump_queue_stats(port->netdev,
                                handle_queue_stats_dump_cb, cbdata);
    } else {
        struct netdev_queue_stats stats;

        if (!netdev_get_queue_stats(port->netdev, queue_id, &stats)) {
            put_queue_stats(cbdata, queue_id, &stats);
        }
    }
}

static int
handle_queue_stats_request(struct ofconn *ofconn,
                           const struct ofp_queue_stats_request *qsr)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    struct queue_stats_cbdata cbdata;
    struct ofport *port;
    unsigned int port_no;
    uint32_t queue_id;

    COVERAGE_INC(ofproto_queue_req);

    ofputil_start_stats_reply(&qsr->osm, &cbdata.replies);

    port_no = ntohs(qsr->port_no);
    queue_id = ntohl(qsr->queue_id);
    if (port_no == OFPP_ALL) {
        HMAP_FOR_EACH (port, hmap_node, &ofproto->ports) {
            handle_queue_stats_for_port(port, queue_id, &cbdata);
        }
    } else if (port_no < OFPP_MAX) {
        port = ofproto_get_port(ofproto, port_no);
        if (port) {
            handle_queue_stats_for_port(port, queue_id, &cbdata);
        }
    } else {
        ofpbuf_list_delete(&cbdata.replies);
        return ofp_mkerr(OFPET_QUEUE_OP_FAILED, OFPQOFC_BAD_PORT);
    }
    ofconn_send_replies(ofconn, &cbdata.replies);

    return 0;
}

static bool
is_flow_deletion_pending(const struct ofproto *ofproto,
                         const struct cls_rule *cls_rule,
                         uint8_t table_id)
{
    if (!hmap_is_empty(&ofproto->deletions)) {
        struct ofoperation *op;

        HMAP_FOR_EACH_WITH_HASH (op, hmap_node,
                                 cls_rule_hash(cls_rule, table_id),
                                 &ofproto->deletions) {
            if (cls_rule_equal(cls_rule, &op->rule->cr)) {
                return true;
            }
        }
    }

    return false;
}

/* Implements OFPFC_ADD and the cases for OFPFC_MODIFY and OFPFC_MODIFY_STRICT
 * in which no matching flow already exists in the flow table.
 *
 * Adds the flow specified by 'ofm', which is followed by 'n_actions'
 * ofp_actions, to the ofproto's flow table.  Returns 0 on success, an OpenFlow
 * error code as encoded by ofp_mkerr() on failure, or OFPROTO_POSTPONE if the
 * operation cannot be initiated now but may be retried later.
 *
 * 'ofconn' is used to retrieve the packet buffer specified in ofm->buffer_id,
 * if any. */
static int
add_flow(struct ofproto *ofproto, struct ofconn *ofconn,
         const struct ofputil_flow_mod *fm, const struct ofp_header *request)
{
    struct classifier *table;
    struct ofopgroup *group;
    struct rule *victim;
    struct rule *rule;
    int error;

    error = check_table_id(ofproto, fm->table_id);
    if (error) {
        return error;
    }

    /* Pick table. */
    if (fm->table_id == 0xff) {
        uint8_t table_id;
        if (ofproto->ofproto_class->rule_choose_table) {
            error = ofproto->ofproto_class->rule_choose_table(ofproto, &fm->cr,
                                                              &table_id);
            if (error) {
                return error;
            }
            assert(table_id < ofproto->n_tables);
            table = &ofproto->tables[table_id];
        } else {
            table = &ofproto->tables[0];
        }
    } else if (fm->table_id < ofproto->n_tables) {
        table = &ofproto->tables[fm->table_id];
    } else {
        return ofp_mkerr_nicira(OFPET_FLOW_MOD_FAILED, NXFMFC_BAD_TABLE_ID);
    }

    /* Check for overlap, if requested. */
    if (fm->flags & OFPFF_CHECK_OVERLAP
        && classifier_rule_overlaps(table, &fm->cr)) {
        return ofp_mkerr(OFPET_FLOW_MOD_FAILED, OFPFMFC_OVERLAP);
    }

    /* Serialize against pending deletion. */
    if (is_flow_deletion_pending(ofproto, &fm->cr, table - ofproto->tables)) {
        return OFPROTO_POSTPONE;
    }

    /* Allocate new rule. */
    rule = ofproto->ofproto_class->rule_alloc();
    if (!rule) {
        VLOG_WARN_RL(&rl, "%s: failed to create rule (%s)",
                     ofproto->name, strerror(error));
        return ENOMEM;
    }
    rule->ofproto = ofproto;
    rule->cr = fm->cr;
    rule->pending = NULL;
    rule->flow_cookie = fm->cookie;
    rule->created = rule->modified = time_msec();
    rule->idle_timeout = fm->idle_timeout;
    rule->hard_timeout = fm->hard_timeout;
    rule->table_id = table - ofproto->tables;
    rule->send_flow_removed = (fm->flags & OFPFF_SEND_FLOW_REM) != 0;
    rule->actions = ofputil_actions_clone(fm->actions, fm->n_actions);
    rule->n_actions = fm->n_actions;

    /* Insert new rule. */
    victim = rule_from_cls_rule(classifier_replace(table, &rule->cr));
    if (victim && victim->pending) {
        error = OFPROTO_POSTPONE;
    } else {
        group = ofopgroup_create(ofproto, ofconn, request, fm->buffer_id);
        ofoperation_create(group, rule, OFOPERATION_ADD);
        rule->pending->victim = victim;

        error = ofproto->ofproto_class->rule_construct(rule);
        if (error) {
            ofoperation_destroy(rule->pending);
        }
        ofopgroup_submit(group);
    }

    /* Back out if an error occurred. */
    if (error) {
        if (victim) {
            classifier_replace(table, &victim->cr);
        } else {
            classifier_remove(table, &rule->cr);
        }
        ofproto_rule_destroy__(rule);
    }
    return error;
}

/* OFPFC_MODIFY and OFPFC_MODIFY_STRICT. */

/* Modifies the rules listed in 'rules', changing their actions to match those
 * in 'fm'.
 *
 * 'ofconn' is used to retrieve the packet buffer specified in fm->buffer_id,
 * if any.
 *
 * Returns 0 on success, otherwise an OpenFlow error code. */
static int
modify_flows__(struct ofproto *ofproto, struct ofconn *ofconn,
               const struct ofputil_flow_mod *fm,
               const struct ofp_header *request, struct list *rules)
{
    struct ofopgroup *group;
    struct rule *rule;

    group = ofopgroup_create(ofproto, ofconn, request, fm->buffer_id);
    LIST_FOR_EACH (rule, ofproto_node, rules) {
        if (!ofputil_actions_equal(fm->actions, fm->n_actions,
                                   rule->actions, rule->n_actions)) {
            ofoperation_create(group, rule, OFOPERATION_MODIFY);
            rule->pending->actions = rule->actions;
            rule->pending->n_actions = rule->n_actions;
            rule->actions = ofputil_actions_clone(fm->actions, fm->n_actions);
            rule->n_actions = fm->n_actions;
            rule->ofproto->ofproto_class->rule_modify_actions(rule);
        } else {
            rule->modified = time_msec();
        }
        rule->flow_cookie = fm->cookie;
    }
    ofopgroup_submit(group);

    return 0;
}

/* Implements OFPFC_MODIFY.  Returns 0 on success or an OpenFlow error code as
 * encoded by ofp_mkerr() on failure.
 *
 * 'ofconn' is used to retrieve the packet buffer specified in fm->buffer_id,
 * if any. */
static int
modify_flows_loose(struct ofproto *ofproto, struct ofconn *ofconn,
                   const struct ofputil_flow_mod *fm,
                   const struct ofp_header *request)
{
    struct list rules;
    int error;

    error = collect_rules_loose(ofproto, fm->table_id, &fm->cr, OFPP_NONE,
                                &rules);
    return (error ? error
            : list_is_empty(&rules) ? add_flow(ofproto, ofconn, fm, request)
            : modify_flows__(ofproto, ofconn, fm, request, &rules));
}

/* Implements OFPFC_MODIFY_STRICT.  Returns 0 on success or an OpenFlow error
 * code as encoded by ofp_mkerr() on failure.
 *
 * 'ofconn' is used to retrieve the packet buffer specified in fm->buffer_id,
 * if any. */
static int
modify_flow_strict(struct ofproto *ofproto, struct ofconn *ofconn,
                   const struct ofputil_flow_mod *fm,
                   const struct ofp_header *request)
{
    struct list rules;
    int error;

    error = collect_rules_strict(ofproto, fm->table_id, &fm->cr, OFPP_NONE,
                                 &rules);
    return (error ? error
            : list_is_empty(&rules) ? add_flow(ofproto, ofconn, fm, request)
            : list_is_singleton(&rules) ? modify_flows__(ofproto, ofconn,
                                                         fm, request, &rules)
            : 0);
}

/* OFPFC_DELETE implementation. */

/* Deletes the rules listed in 'rules'.
 *
 * Returns 0 on success, otherwise an OpenFlow error code. */
static int
delete_flows__(struct ofproto *ofproto, struct ofconn *ofconn,
               const struct ofp_header *request, struct list *rules)
{
    struct rule *rule, *next;
    struct ofopgroup *group;

    group = ofopgroup_create(ofproto, ofconn, request, UINT32_MAX);
    LIST_FOR_EACH_SAFE (rule, next, ofproto_node, rules) {
        ofproto_rule_send_removed(rule, OFPRR_DELETE);

        ofoperation_create(group, rule, OFOPERATION_DELETE);
        classifier_remove(&ofproto->tables[rule->table_id], &rule->cr);
        rule->ofproto->ofproto_class->rule_destruct(rule);
    }
    ofopgroup_submit(group);

    return 0;
}

/* Implements OFPFC_DELETE. */
static int
delete_flows_loose(struct ofproto *ofproto, struct ofconn *ofconn,
                   const struct ofputil_flow_mod *fm,
                   const struct ofp_header *request)
{
    struct list rules;
    int error;

    error = collect_rules_loose(ofproto, fm->table_id, &fm->cr, fm->out_port,
                                &rules);
    return (error ? error
            : !list_is_empty(&rules) ? delete_flows__(ofproto, ofconn, request,
                                                      &rules)
            : 0);
}

/* Implements OFPFC_DELETE_STRICT. */
static int
delete_flow_strict(struct ofproto *ofproto, struct ofconn *ofconn,
                   const struct ofputil_flow_mod *fm,
                   const struct ofp_header *request)
{
    struct list rules;
    int error;

    error = collect_rules_strict(ofproto, fm->table_id, &fm->cr, fm->out_port,
                                 &rules);
    return (error ? error
            : list_is_singleton(&rules) ? delete_flows__(ofproto, ofconn,
                                                         request, &rules)
            : 0);
}

static void
ofproto_rule_send_removed(struct rule *rule, uint8_t reason)
{
    struct ofputil_flow_removed fr;

    if (rule_is_hidden(rule) || !rule->send_flow_removed) {
        return;
    }

    fr.rule = rule->cr;
    fr.cookie = rule->flow_cookie;
    fr.reason = reason;
    calc_flow_duration__(rule->created, &fr.duration_sec, &fr.duration_nsec);
    fr.idle_timeout = rule->idle_timeout;
    rule->ofproto->ofproto_class->rule_get_stats(rule, &fr.packet_count,
                                                 &fr.byte_count);

    connmgr_send_flow_removed(rule->ofproto->connmgr, &fr);
}

/* Sends an OpenFlow "flow removed" message with the given 'reason' (either
 * OFPRR_HARD_TIMEOUT or OFPRR_IDLE_TIMEOUT), and then removes 'rule' from its
 * ofproto.
 *
 * ofproto implementation ->run() functions should use this function to expire
 * OpenFlow flows. */
void
ofproto_rule_expire(struct rule *rule, uint8_t reason)
{
    struct ofproto *ofproto = rule->ofproto;
    struct ofopgroup *group;

    assert(reason == OFPRR_HARD_TIMEOUT || reason == OFPRR_IDLE_TIMEOUT);

    ofproto_rule_send_removed(rule, reason);

    group = ofopgroup_create_unattached(ofproto);
    ofoperation_create(group, rule, OFOPERATION_DELETE);
    classifier_remove(&ofproto->tables[rule->table_id], &rule->cr);
    rule->ofproto->ofproto_class->rule_destruct(rule);
    ofopgroup_submit(group);
}

static int
handle_flow_mod(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct ofputil_flow_mod fm;
    int error;

    

    //VLOG_INFO("bridge %s",ofconn->rconn->name);
    error = reject_slave_controller(ofconn);
    if (error) {
        return error;
    }

    error = ofputil_decode_flow_mod(&fm, oh,
                                    ofconn_get_flow_mod_table_id(ofconn));
    if (error) {
        return error;
    }

    /* We do not support the emergency flow cache.  It will hopefully get
     * dropped from OpenFlow in the near future. */
    if (fm.flags & OFPFF_EMERG) {
        /* There isn't a good fit for an error code, so just state that the
         * flow table is full. */
        return ofp_mkerr(OFPET_FLOW_MOD_FAILED, OFPFMFC_ALL_TABLES_FULL);
    }

    return handle_flow_mod__(ofconn_get_ofproto(ofconn), ofconn, &fm, oh);
}

static int
handle_flow_mod__(struct ofproto *ofproto, struct ofconn *ofconn,
                  const struct ofputil_flow_mod *fm,
                  const struct ofp_header *oh)
{
    
    if (ofproto->n_pending >= 50) {
        assert(!list_is_empty(&ofproto->pending));
        return OFPROTO_POSTPONE;
    }

    switch (fm->command) {
    case OFPFC_ADD:
        return add_flow(ofproto, ofconn, fm, oh);

    case OFPFC_MODIFY:
        return modify_flows_loose(ofproto, ofconn, fm, oh);

    case OFPFC_MODIFY_STRICT:
        return modify_flow_strict(ofproto, ofconn, fm, oh);

    case OFPFC_DELETE:
        return delete_flows_loose(ofproto, ofconn, fm, oh);

    case OFPFC_DELETE_STRICT:
        return delete_flow_strict(ofproto, ofconn, fm, oh);

    default:
        if (fm->command > 0xff) {
            VLOG_WARN_RL(&rl, "flow_mod has explicit table_id but "
                         "flow_mod_table_id extension is not enabled");
        }
        return ofp_mkerr(OFPET_FLOW_MOD_FAILED, OFPFMFC_BAD_COMMAND);
    }
}

static int
handle_role_request(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct nx_role_request *nrr = (struct nx_role_request *) oh;
    struct nx_role_request *reply;
    struct ofpbuf *buf;
    uint32_t role;

    if (ofconn_get_type(ofconn) != OFCONN_PRIMARY) {
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_EPERM);
    }

    role = ntohl(nrr->role);
    if (role != NX_ROLE_OTHER && role != NX_ROLE_MASTER
        && role != NX_ROLE_SLAVE) {
        return ofp_mkerr_nicira(OFPET_BAD_REQUEST, NXBRC_BAD_ROLE);
    }

    if (ofconn_get_role(ofconn) != role
        && ofconn_has_pending_opgroups(ofconn)) {
        return OFPROTO_POSTPONE;
    }

    ofconn_set_role(ofconn, role);

    reply = make_nxmsg_xid(sizeof *reply, NXT_ROLE_REPLY, oh->xid, &buf);
    reply->role = htonl(role);
    ofconn_send_reply(ofconn, buf);

    return 0;
}

static int
handle_nxt_flow_mod_table_id(struct ofconn *ofconn,
                             const struct ofp_header *oh)
{
    const struct nxt_flow_mod_table_id *msg
        = (const struct nxt_flow_mod_table_id *) oh;

    ofconn_set_flow_mod_table_id(ofconn, msg->set != 0);
    return 0;
}

static int
handle_nxt_set_flow_format(struct ofconn *ofconn, const struct ofp_header *oh)
{
    const struct nxt_set_flow_format *msg
        = (const struct nxt_set_flow_format *) oh;
    uint32_t format;

    format = ntohl(msg->format);
    if (format != NXFF_OPENFLOW10 && format != NXFF_NXM) {
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_EPERM);
    }

    if (format != ofconn_get_flow_format(ofconn)
        && ofconn_has_pending_opgroups(ofconn)) {
        /* Avoid sending async messages in surprising flow format. */
        return OFPROTO_POSTPONE;
    }

    ofconn_set_flow_format(ofconn, format);
    return 0;
}

static int
handle_barrier_request(struct ofconn *ofconn, const struct ofp_header *oh)
{
    struct ofp_header *ob;
    struct ofpbuf *buf;

    if (ofconn_has_pending_opgroups(ofconn)) {
        return OFPROTO_POSTPONE;
    }

    ob = make_openflow_xid(sizeof *ob, OFPT_BARRIER_REPLY, oh->xid, &buf);
    ofconn_send_reply(ofconn, buf);
    return 0;
}

static int
char2int(char c)
{
     switch(c){
	case '0': return 0;
	case '1': return 1;
	case '2': return 2;
	case '3': return 3;
	case '4': return 4;
	case '5': return 5;
	case '6': return 6;
	case '7': return 7;
	case '8': return 8;
	case '9': return 9;
    }
}

static char
int2char(int num)
{
    switch(num){
	case 0: return '0';
	case 1: return '1';
	case 2: return '2';
	case 3: return '3';
	case 4: return '4';
	case 5: return '5';
	case 6: return '6';
	case 7: return '7';
	case 8: return '8';
	case 9: return '9';
    }
}

static int sum(uint32_t num)
{
    uint8_t octet;
    int voctet;
    int value = 0;

    octet = num >> 24;
    voctet = octet;
    value = value + voctet;

    octet = num >> 16;
    voctet = octet;
    value = value + voctet*16*16;

    octet = num >> 8;
    voctet = octet;
    value = value + voctet*16*16*16*16;

    octet = num;
    voctet = octet;
    value = value + voctet*16*16*16*16*16*16;

    return value;
}

static int
int2string(int num)
{
    int c = 0;
    while(num > 9){
        array[c] = int2char(num%10);
        num = num/10;
        c++;
    }
    array[c]=int2char(num);
    c++;

    return c;
}



void
handle_vendor_message(struct ofconn *ofconn, const struct ofp_header *oh)
{
    const struct ofp_vendor_header *opvh = (const struct ofp_vendor_header *) oh;
    
    int p;
    ovs_be32 vendor = opvh->vendor;


    if (vendor == 0x1000080){
	    int argc=12;
	    char *argv[12];

	    int argcr=7;
	    char *argvr[7];

	    int argca=13;
	    char *argva[13];

	    int port_id = opvh->port_id;
	    int queue_id = opvh->queue_id;
	    
	    uint32_t min_rate = opvh->min_rate;
	    uint32_t max_rate = opvh->max_rate;
	    

	    int per_maxrate = sum(max_rate);
	    int per_minrate = sum(min_rate);

	    VLOG_INFO("port_id %d",port_id);
	    /*VLOG_INFO("queue_id %d",queue_id);
	    VLOG_INFO("per_minrate %d",per_minrate);
	    VLOG_INFO("per_maxrate %d",per_maxrate);*/

	    int minrate = 100000000*per_minrate/100;
	    int maxrate = 100000000*per_maxrate/100;

	    //VLOG_INFO("minrate %d",minrate);
	    //VLOG_INFO("maxrate %d",maxrate);

	    int bridge_id = char2int(bridge_name[1]);

	    VLOG_INFO("bridge_id %d",bridge_id);
	    
	    if(port_id == 252){
		
		/*if((bridge_id == 1) || (bridge_id == 3))
	    	    p = 5;
		else 
		    p = 3;
		*/
		for(port_id = 1;port_id < 4;port_id++){
	    
	    		sp = bridge_id*10 + port_id;

			VLOG_INFO("sp %d",sp);
	    		if (verifyqos[sp] == false){
	
				s4[6] = int2char(port_id);
				s4[1] = int2char(bridge_id);
			

				argv[0]=s0;
				argv[1]=s1;
				argv[2]=s2;
				argv[3]=s3;
				argv[4]=s4;
				argv[5]=s5;
				argv[6]=s6;
				argv[7]=s7;
				argv[8]=s8;
				argv[9]=s9;
				argv[10]=s10;
				argv[11]=s11;


			
			    	vsctl(argc,argv);
			    
				verifyqos[sp] = true;     
	       
	    		}

			    int k;
			    
			    for(k = 0;k < 36;k++){
				r4[k] = sqos[sp][k];
			    }
			     
			    
			    
			    int c = int2string(queue_id);


			    for (k=0;k < c;k++){
				r6[k] = array[c - k - 1];
			    }

			    argvr[6]=r6;
			    argvr[6][c]=NULL;
			    VLOG_INFO("argvr[6] %s",argvr[6]);
	    

			    argvr[0]=r0;
			    argvr[1]=r1;
			    argvr[2]=r2;
			    argvr[3]=r3;
			    argvr[4]=r4;
			    argvr[5]=r5;
			    
			   
			    vsctl(argcr,argvr);

			    for (k=0;k < c;k++){
				a6[k] = array[c - k - 1];
			    }

			    for (k=0;k < 3;k++){
				a6[k+c] = ap6[k];
			    }

			    for (k=0;k < c;k++){
				a6[k + c + 3] = array[c - k - 1];
			    }

			    argva[6]=a6;
			    
			    argva[6][c+3+c] = NULL;
			    VLOG_INFO("argva[6] %s",argva[6]);

			    for (k=0;k < c;k++){
				a8[k + 7] = array[c - k - 1];
			    }
			    argva[8]=a8;
			    argva[8][7 + c] = NULL;
			    VLOG_INFO("argva[8] %s",argva[8]);

			    c = int2string(minrate);

			    for (k=0;k < c;k++){
				a11[k + 22] = array[c - k - 1];
			    }
		
			    argva[11]=a11;
			    
			    argva[11][22+c] = NULL;
			    VLOG_INFO("argva[11] %s",argva[11]);

			    c = int2string(maxrate);
			    

			    for (k=0;k < c;k++){
				a12[k + 22] = array[c - k - 1];
			    }

			    
			    
			    argva[0]=a0;
			    argva[1]=a1;
			    argva[2]=a2;
			    argva[3]=a3;
			    argva[4]=r4;
			    argva[5]=a5;
			    
			    argva[7]=a7;
			    
			    argva[9]=a9;
			    argva[10]=a10;

			    
			    
			    argva[12]=a12;
			    
                            argva[12][22+c] = NULL;

			    VLOG_INFO("argva[12] %s",argva[12]);
			    vsctl(argca,argva);
			}  
	    }

	    else{
			
			sp = bridge_id*10 + port_id;
			
	    		if (verifyqos[sp] == false){
			VLOG_INFO("sp %d",sp);
				s4[6] = int2char(port_id);
				s4[1] = int2char(bridge_id);
			
				argv[0]=s0;
				argv[1]=s1;
				argv[2]=s2;
				argv[3]=s3;
				argv[4]=s4;
				argv[5]=s5;
				argv[6]=s6;
				argv[7]=s7;
				argv[8]=s8;
				argv[9]=s9;
				argv[10]=s10;
				argv[11]=s11;

			    	vsctl(argc,argv);
			        
				verifyqos[sp] = true;     
	       
	    		}

			    int k;
			    
			    for(k = 0;k < 36;k++){
				r4[k] = sqos[sp][k];
			    }
			     
			    
			   

			    int c = int2string(queue_id);

			    for (k=0;k < c;k++){
				r6[k] = array[c - k - 1];
			    }

			    argvr[6]=r6;
			    argvr[6][c]=NULL;
			    VLOG_INFO("argvr[6] %s",argvr[6]);

			    argvr[0]=r0;
			    argvr[1]=r1;
			    argvr[2]=r2;
			    argvr[3]=r3;
			    argvr[4]=r4;
			    argvr[5]=r5;
			  
			   
			    vsctl(argcr,argvr);

			     for (k=0;k < c;k++){
				a6[k] = array[c - k - 1];
			    }

			    for (k=0;k < 3;k++){
				a6[k+c] = ap6[k];
			    }

			    for (k=0;k < c;k++){
				a6[k + c + 3] = array[c - k - 1];
			    }

			    argva[6]=a6;
			    
			    argva[6][c+3+c] = NULL;
			    VLOG_INFO("argva[6] %s",argva[6]);

			    for (k=0;k < c;k++){
				a8[k + 7] = array[c - k - 1];
			    }
			    argva[8]=a8;
			    argva[8][7 + c] = NULL;
			    VLOG_INFO("argva[8] %s",argva[8]);

			    c = int2string(minrate);

			    for (k=0;k < c;k++){
				a11[k + 22] = array[c - k - 1];
			    }

			    argva[11]=a11;
			    
			    argva[11][22+c] = NULL;
			    VLOG_INFO("argva[11] %s",argva[11]);

			    c = int2string(maxrate);

			    for (k=0;k < c;k++){
				a12[k + 22] = array[c - k - 1];
			    }

			   
			    
			    argva[0]=a0;
			    argva[1]=a1;
			    argva[2]=a2;
			    argva[3]=a3;
			    argva[4]=r4;
			    argva[5]=a5;
			   
			    argva[7]=a7;
			    
			    argva[9]=a9;
			    argva[10]=a10;
			
			    argva[12]=a12;

		    
                            argva[12][22+c] = NULL;

			    VLOG_INFO("argva[12] %s",argva[12]);

			    vsctl(argca,argva);

				
		    
	    }
    }
    /*
    ////////////////////////////////////////////////////
	static char s0[]="ovs-vsctl";
	static char s1[]="--";
	static char s2[]="set";
	static char s3[]="Port";
	static char s4[]="s1-eth4";
	static char s5[]="qos=@newqos";
	static char s6[]="--";
	static char s7[]="--id=@newqos";
	static char s8[]="create";
	static char s9[]="QoS";
	static char s10[]="type=linux-htb";
	static char s11[]="other-config:max-rate=100000000";
	static char s12[]="welcome";

/////////////////////////////////////////////////////

	static char a0[]="ovs-vsctl";
	static char a1[]="--";
	static char a2[]="add";
	static char a3[]="Qos";
	static char a4[]="3dba4eae-4566-4af6-9ea7-294c00cb2bc6";
	static char a5[]="queues";
	static char a6[]="21=@q21";
	static char a7[]="--";
	static char a8[]="--id=@q21";
	static char a9[]="create";
	static char a10[]="Queue";
	static char a11[]="other-config:min-rate=4000000";
	static char a12[]="other-config:max-rate=6000000";
	static char a13[]="welcome";

/////////////////////////////////////////////////////

	static char r0[]="ovs-vsctl";
	static char r1[]="--";
	static char r2[]="remove";
	static char r3[]="Qos";
	static char r4[]="3dba4eae-4566-4af6-9ea7-294c00cb2bc6";
	static char r5[]="queues";
	static char r6[]="22";
	static char r7[]="welcome";

/////////////////////////////////////////////////////*/
    
    /*const struct ofp_vendor_header *opvh = (const struct ofp_vendor_header *) oh;

    int argc=19;
    char *argv[20];
    //int switch_id = opvh->switch_id;
    int port_id = opvh->port_id;
    int queue_id = opvh->queue_id;
    ovs_be32 vendor = opvh->vendor;
    //int flag = opvh->flag;

    char queue[100];
    VLOG_INFO("queue_id %d",queue_id);
    int c_queue = 0;
    while(queue_id > 9){
        queue[c_queue] = int2char(queue_id%10);
        queue_id = queue_id/10;
        c_queue++;
    }
    queue[c_queue]=int2char(queue_id);
    c_queue++;

    int z;

    for (z=0;z < c_queue;z++){
	a12[z+7] = queue[c_queue - z - 1];
    }
    
    for (z=0;z < c_queue;z++){
	a12[z+12] = queue[c_queue - z - 1];
    }

    for (z=0;z < c_queue;z++){
	a14[z+7] = queue[c_queue - z - 1];
    }
    
    
    if (vendor == 0x1000080){
    
    
    uint32_t min_rate = opvh->min_rate;
    uint32_t max_rate = opvh->max_rate;

    char min[100];
    char max[100];

    int vmin,vmax;

    
     
    int cmin = 0;
    while(vmin > 9){
	min[cmin] = int2char(vmin%10);
        vmin = vmin/10;
       	cmin++;
    }
    min[cmin]=int2char(vmin);
    cmin++;
  
    int m;
    for(m=0;m < cmin;m++){
	a18[m+22] = min[cmin - m - 1];
    }

    int cmax = 0;
    while(vmax > 9){
	max[cmax] = int2char(vmax%10);
        vmax = vmax/10;
       	cmax++;
    }
    max[cmax]=int2char(vmax);
    cmax++;

   
  
    int n;
    for(n=0;n < cmax;n++){
	a17[n+22] = max[cmax - n - 1];
    }

    /*static char a0[]="ovs-vsctl";
    static char a1[]="--";
    static char a2[]="set";
    static char a3[]="Port";
    static char a4[]="s1-eth2";
    static char a5[]="qos=@newqos";
    static char a6[]="--";
    static char a7[]="--id=@newqos";
    static char a8[]="create";
    static char a9[]="QoS";
    static char a10[]="type=linux-htb";
    static char a11[]="other-config:max-rate=10000000";
    static char a12[]="queues=45=@q45";
    static char a13[]="--";
    static char a14[]="--id=@q45";
    static char a15[]="create";
    static char a16[]="Queue";
    static char a17[]="other-config:min-rate=500";
    static char a18[]="other-config:max-rate=1000"; */

   /* int c;
    for (c = 1;c < 5;c++){
	    a4[1]=bridge_name[1];
	    a4[6]=int2char(c);
	    //a12[7]=a12[11]=a14[7]=int2char(queue_id);

	    

	    argv[0]=a0;
	    argv[1]=a1;
	    argv[2]=a2;
	    argv[3]=a3;
	    argv[4]=a4;
	    argv[5]=a5;
	    argv[6]=a6;
	    argv[7]=a7;
	    argv[8]=a8;
	    argv[9]=a9;
	    argv[10]=a10;
	    argv[11]=a11;
	    
	    
	    
	    
	    vsctl(argc,argv);
   // }
    }*/
    
}

static int
handle_openflow__(struct ofconn *ofconn, const struct ofpbuf *msg)
{
    const struct ofp_header *oh = msg->data;
    const struct ofputil_msg_type *type;
    int error;

    error = ofputil_decode_msg_type(oh, &type);

    if (error == 100) {
	error = 0;
	handle_vendor_message(ofconn,oh);
	return error;
    }
    if (error) {
        return error;
    }

    switch (ofputil_msg_type_code(type)) {
        /* OpenFlow requests. */
    case OFPUTIL_OFPT_ECHO_REQUEST:
        return handle_echo_request(ofconn, oh);

    case OFPUTIL_OFPT_FEATURES_REQUEST:
        return handle_features_request(ofconn, oh);

    case OFPUTIL_OFPT_GET_CONFIG_REQUEST:
        return handle_get_config_request(ofconn, oh);

    case OFPUTIL_OFPT_SET_CONFIG:
        return handle_set_config(ofconn, msg->data);

    case OFPUTIL_OFPT_PACKET_OUT:
        return handle_packet_out(ofconn, oh);

    case OFPUTIL_OFPT_PORT_MOD:
        return handle_port_mod(ofconn, oh);

    case OFPUTIL_OFPT_FLOW_MOD:
        return handle_flow_mod(ofconn, oh);

    case OFPUTIL_OFPT_BARRIER_REQUEST:
        return handle_barrier_request(ofconn, oh);

        /* OpenFlow replies. */
    case OFPUTIL_OFPT_ECHO_REPLY:
        return 0;

        /* Nicira extension requests. */
    case OFPUTIL_NXT_ROLE_REQUEST:
        return handle_role_request(ofconn, oh);

    case OFPUTIL_NXT_FLOW_MOD_TABLE_ID:
        return handle_nxt_flow_mod_table_id(ofconn, oh);

    case OFPUTIL_NXT_SET_FLOW_FORMAT:
        return handle_nxt_set_flow_format(ofconn, oh);

    case OFPUTIL_NXT_FLOW_MOD:
        return handle_flow_mod(ofconn, oh);

        /* Statistics requests. */
    case OFPUTIL_OFPST_DESC_REQUEST:
        return handle_desc_stats_request(ofconn, msg->data);

    case OFPUTIL_OFPST_FLOW_REQUEST:
    case OFPUTIL_NXST_FLOW_REQUEST:
        return handle_flow_stats_request(ofconn, msg->data);

    case OFPUTIL_OFPST_AGGREGATE_REQUEST:
    case OFPUTIL_NXST_AGGREGATE_REQUEST:
        return handle_aggregate_stats_request(ofconn, msg->data);

    case OFPUTIL_OFPST_TABLE_REQUEST:
        return handle_table_stats_request(ofconn, msg->data);

    case OFPUTIL_OFPST_PORT_REQUEST:
        return handle_port_stats_request(ofconn, msg->data);

    case OFPUTIL_OFPST_QUEUE_REQUEST:
        return handle_queue_stats_request(ofconn, msg->data);

    case OFPUTIL_MSG_INVALID:
    case OFPUTIL_OFPT_HELLO:
    case OFPUTIL_OFPT_ERROR:
    case OFPUTIL_OFPT_FEATURES_REPLY:
    case OFPUTIL_OFPT_GET_CONFIG_REPLY:
    case OFPUTIL_OFPT_PACKET_IN:
    case OFPUTIL_OFPT_FLOW_REMOVED:
    case OFPUTIL_OFPT_PORT_STATUS:
    case OFPUTIL_OFPT_BARRIER_REPLY:
    case OFPUTIL_OFPT_QUEUE_GET_CONFIG_REQUEST:
    case OFPUTIL_OFPT_QUEUE_GET_CONFIG_REPLY:
    case OFPUTIL_OFPST_DESC_REPLY:
    case OFPUTIL_OFPST_FLOW_REPLY:
    case OFPUTIL_OFPST_QUEUE_REPLY:
    case OFPUTIL_OFPST_PORT_REPLY:
    case OFPUTIL_OFPST_TABLE_REPLY:
    case OFPUTIL_OFPST_AGGREGATE_REPLY:
    case OFPUTIL_NXT_ROLE_REPLY:
    case OFPUTIL_NXT_FLOW_REMOVED:
    case OFPUTIL_NXST_FLOW_REPLY:
    case OFPUTIL_NXST_AGGREGATE_REPLY:
    default:
        if (oh->type == OFPT_STATS_REQUEST || oh->type == OFPT_STATS_REPLY) {
            return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_STAT);
        } else {
            return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_TYPE);
        }
    }
}

static bool
handle_openflow(struct ofconn *ofconn, struct ofpbuf *ofp_msg)
{
    int error = handle_openflow__(ofconn, ofp_msg);
    if (error && error != OFPROTO_POSTPONE) {
        ofconn_send_error(ofconn, ofp_msg->data, error);
    }
    COVERAGE_INC(ofproto_recv_openflow);
    return error != OFPROTO_POSTPONE;
}

/* Asynchronous operations. */

/* Creates and returns a new ofopgroup that is not associated with any
 * OpenFlow connection.
 *
 * The caller should add operations to the returned group with
 * ofoperation_create() and then submit it with ofopgroup_submit(). */
static struct ofopgroup *
ofopgroup_create_unattached(struct ofproto *ofproto)
{
    struct ofopgroup *group = xzalloc(sizeof *group);
    group->ofproto = ofproto;
    list_init(&group->ofproto_node);
    list_init(&group->ops);
    list_init(&group->ofconn_node);
    return group;
}

/* Creates and returns a new ofopgroup for 'ofproto'.
 *
 * If 'ofconn' is NULL, the new ofopgroup is not associated with any OpenFlow
 * connection.  The 'request' and 'buffer_id' arguments are ignored.
 *
 * If 'ofconn' is nonnull, then the new ofopgroup is associated with 'ofconn'.
 * If the ofopgroup eventually fails, then the error reply will include
 * 'request'.  If the ofopgroup eventually succeeds, then the packet with
 * buffer id 'buffer_id' on 'ofconn' will be sent by 'ofconn''s ofproto.
 *
 * The caller should add operations to the returned group with
 * ofoperation_create() and then submit it with ofopgroup_submit(). */
static struct ofopgroup *
ofopgroup_create(struct ofproto *ofproto, struct ofconn *ofconn,
                 const struct ofp_header *request, uint32_t buffer_id)
{
    struct ofopgroup *group = ofopgroup_create_unattached(ofproto);
    if (ofconn) {
        size_t request_len = ntohs(request->length);

        assert(ofconn_get_ofproto(ofconn) == ofproto);

        ofconn_add_opgroup(ofconn, &group->ofconn_node);
        group->ofconn = ofconn;
        group->request = xmemdup(request, MIN(request_len, 64));
        group->buffer_id = buffer_id;
    }
    return group;
}

/* Submits 'group' for processing.
 *
 * If 'group' contains no operations (e.g. none were ever added, or all of the
 * ones that were added completed synchronously), then it is destroyed
 * immediately.  Otherwise it is added to the ofproto's list of pending
 * groups. */
static void
ofopgroup_submit(struct ofopgroup *group)
{
    if (list_is_empty(&group->ops)) {
        ofopgroup_destroy(group);
    } else {
        list_push_back(&group->ofproto->pending, &group->ofproto_node);
        group->ofproto->n_pending++;
    }
}

static void
ofopgroup_destroy(struct ofopgroup *group)
{
    assert(list_is_empty(&group->ops));
    if (!list_is_empty(&group->ofproto_node)) {
        assert(group->ofproto->n_pending > 0);
        group->ofproto->n_pending--;
        list_remove(&group->ofproto_node);
    }
    if (!list_is_empty(&group->ofconn_node)) {
        list_remove(&group->ofconn_node);
        if (group->error) {
            ofconn_send_error(group->ofconn, group->request, group->error);
        }
        connmgr_retry(group->ofproto->connmgr);
    }
    free(group->request);
    free(group);
}

/* Initiates a new operation on 'rule', of the specified 'type', within
 * 'group'.  Prior to calling, 'rule' must not have any pending operation. */
static void
ofoperation_create(struct ofopgroup *group, struct rule *rule,
                   enum ofoperation_type type)
{
    struct ofoperation *op;

    assert(!rule->pending);

    op = rule->pending = xzalloc(sizeof *op);
    op->group = group;
    list_push_back(&group->ops, &op->group_node);
    op->rule = rule;
    op->type = type;
    op->status = -1;
    op->flow_cookie = rule->flow_cookie;

    if (type == OFOPERATION_DELETE) {
        hmap_insert(&op->group->ofproto->deletions, &op->hmap_node,
                    cls_rule_hash(&rule->cr, rule->table_id));
    }
}

static void
ofoperation_destroy(struct ofoperation *op)
{
    struct ofopgroup *group = op->group;

    if (op->rule) {
        op->rule->pending = NULL;
    }
    if (op->type == OFOPERATION_DELETE) {
        hmap_remove(&group->ofproto->deletions, &op->hmap_node);
    }
    list_remove(&op->group_node);
    free(op->actions);
    free(op);

    if (list_is_empty(&group->ops) && !list_is_empty(&group->ofproto_node)) {
        ofopgroup_destroy(group);
    }
}

/* Indicates that 'op' completed with status 'error', which is either 0 to
 * indicate success or an OpenFlow error code (constructed with
 * e.g. ofp_mkerr()).
 *
 * If 'error' is 0, indicating success, the operation will be committed
 * permanently to the flow table.  There is one interesting subcase:
 *
 *   - If 'op' is an "add flow" operation that is replacing an existing rule in
 *     the flow table (the "victim" rule) by a new one, then the caller must
 *     have uninitialized any derived state in the victim rule, as in step 5 in
 *     the "Life Cycle" in ofproto/ofproto-provider.h.  ofoperation_complete()
 *     performs steps 6 and 7 for the victim rule, most notably by calling its
 *     ->rule_dealloc() function.
 *
 * If 'error' is nonzero, then generally the operation will be rolled back:
 *
 *   - If 'op' is an "add flow" operation, ofproto removes the new rule or
 *     restores the original rule.  The caller must have uninitialized any
 *     derived state in the new rule, as in step 5 of in the "Life Cycle" in
 *     ofproto/ofproto-provider.h.  ofoperation_complete() performs steps 6 and
 *     and 7 for the new rule, calling its ->rule_dealloc() function.
 *
 *   - If 'op' is a "modify flow" operation, ofproto restores the original
 *     actions.
 *
 *   - 'op' must not be a "delete flow" operation.  Removing a rule is not
 *     allowed to fail.  It must always succeed.
 *
 * Please see the large comment in ofproto/ofproto-provider.h titled
 * "Asynchronous Operation Support" for more information. */
void
ofoperation_complete(struct ofoperation *op, int error)
{
    struct ofopgroup *group = op->group;
    struct rule *rule = op->rule;
    struct ofproto *ofproto = rule->ofproto;
    struct classifier *table = &ofproto->tables[rule->table_id];

    assert(rule->pending == op);
    assert(op->status < 0);
    assert(error >= 0);

    if (!error
        && !group->error
        && op->type != OFOPERATION_DELETE
        && group->ofconn
        && group->buffer_id != UINT32_MAX
        && list_is_singleton(&op->group_node)) {
        struct ofpbuf *packet;
        uint16_t in_port;

        error = ofconn_pktbuf_retrieve(group->ofconn, group->buffer_id,
                                       &packet, &in_port);
        if (packet) {
            assert(!error);
            error = rule_execute(rule, in_port, packet);
        }
    }
    if (!group->error) {
        group->error = error;
    }

    switch (op->type) {
    case OFOPERATION_ADD:
        if (!error) {
            if (op->victim) {
                ofproto_rule_destroy__(op->victim);
            }
            if ((rule->cr.wc.vlan_tci_mask & htons(VLAN_VID_MASK))
                == htons(VLAN_VID_MASK)) {
                if (ofproto->vlan_bitmap) {
                    uint16_t vid = vlan_tci_to_vid(rule->cr.flow.vlan_tci);

                    if (!bitmap_is_set(ofproto->vlan_bitmap, vid)) {
                        bitmap_set1(ofproto->vlan_bitmap, vid);
                        ofproto->vlans_changed = true;
                    }
                } else {
                    ofproto->vlans_changed = true;
                }
            }
        } else {
            if (op->victim) {
                classifier_replace(table, &op->victim->cr);
                op->victim = NULL;
            } else {
                classifier_remove(table, &rule->cr);
            }
            ofproto_rule_destroy__(rule);
            op->rule = NULL;
        }
        op->victim = NULL;
        break;

    case OFOPERATION_DELETE:
        assert(!error);
        ofproto_rule_destroy__(rule);
        op->rule = NULL;
        break;

    case OFOPERATION_MODIFY:
        if (!error) {
            rule->modified = time_msec();
        } else {
            free(rule->actions);
            rule->actions = op->actions;
            rule->n_actions = op->n_actions;
            op->actions = NULL;
        }
        break;

    default:
        NOT_REACHED();
    }
    ofoperation_destroy(op);
}

struct rule *
ofoperation_get_victim(struct ofoperation *op)
{
    assert(op->type == OFOPERATION_ADD);
    return op->victim;
}

static uint64_t
pick_datapath_id(const struct ofproto *ofproto)
{
    const struct ofport *port;

    port = ofproto_get_port(ofproto, OFPP_LOCAL);
    if (port) {
        uint8_t ea[ETH_ADDR_LEN];
        int error;

        error = netdev_get_etheraddr(port->netdev, ea);
        if (!error) {
            return eth_addr_to_uint64(ea);
        }
        VLOG_WARN("could not get MAC address for %s (%s)",
                  netdev_get_name(port->netdev), strerror(error));
    }
    return ofproto->fallback_dpid;
}

static uint64_t
pick_fallback_dpid(void)
{
    uint8_t ea[ETH_ADDR_LEN];
    eth_addr_nicira_random(ea);
    return eth_addr_to_uint64(ea);
}

/* unixctl commands. */

struct ofproto *
ofproto_lookup(const char *name)
{
    struct ofproto *ofproto;

    HMAP_FOR_EACH_WITH_HASH (ofproto, hmap_node, hash_string(name, 0),
                             &all_ofprotos) {
        if (!strcmp(ofproto->name, name)) {
            return ofproto;
        }
    }
    return NULL;
}

static void
ofproto_unixctl_list(struct unixctl_conn *conn, const char *arg OVS_UNUSED,
                     void *aux OVS_UNUSED)
{
    struct ofproto *ofproto;
    struct ds results;

    ds_init(&results);
    HMAP_FOR_EACH (ofproto, hmap_node, &all_ofprotos) {
        ds_put_format(&results, "%s\n", ofproto->name);
    }
    unixctl_command_reply(conn, 200, ds_cstr(&results));
    ds_destroy(&results);
}

static void
ofproto_unixctl_init(void)
{
    static bool registered;
    if (registered) {
        return;
    }
    registered = true;

    unixctl_command_register("ofproto/list", "", ofproto_unixctl_list, NULL);
}

/* Linux VLAN device support (e.g. "eth0.10" for VLAN 10.)
 *
 * This is deprecated.  It is only for compatibility with broken device drivers
 * in old versions of Linux that do not properly support VLANs when VLAN
 * devices are not used.  When broken device drivers are no longer in
 * widespread use, we will delete these interfaces. */

/* Sets a 1-bit in the 4096-bit 'vlan_bitmap' for each VLAN ID that is matched
 * (exactly) by an OpenFlow rule in 'ofproto'. */
void
ofproto_get_vlan_usage(struct ofproto *ofproto, unsigned long int *vlan_bitmap)
{
    const struct classifier *cls;

    free(ofproto->vlan_bitmap);
    ofproto->vlan_bitmap = bitmap_allocate(4096);
    ofproto->vlans_changed = false;

    OFPROTO_FOR_EACH_TABLE (cls, ofproto) {
        const struct cls_table *table;

        HMAP_FOR_EACH (table, hmap_node, &cls->tables) {
            if ((table->wc.vlan_tci_mask & htons(VLAN_VID_MASK))
                == htons(VLAN_VID_MASK)) {
                const struct cls_rule *rule;

                HMAP_FOR_EACH (rule, hmap_node, &table->rules) {
                    uint16_t vid = vlan_tci_to_vid(rule->flow.vlan_tci);
                    bitmap_set1(vlan_bitmap, vid);
                    bitmap_set1(ofproto->vlan_bitmap, vid);
                }
            }
        }
    }
}

/* Returns true if new VLANs have come into use by the flow table since the
 * last call to ofproto_get_vlan_usage().
 *
 * We don't track when old VLANs stop being used. */
bool
ofproto_has_vlan_usage_changed(const struct ofproto *ofproto)
{
    return ofproto->vlans_changed;
}

/* Configures a VLAN splinter binding between the ports identified by OpenFlow
 * port numbers 'vlandev_ofp_port' and 'realdev_ofp_port'.  If
 * 'realdev_ofp_port' is nonzero, then the VLAN device is enslaved to the real
 * device as a VLAN splinter for VLAN ID 'vid'.  If 'realdev_ofp_port' is zero,
 * then the VLAN device is un-enslaved. */
int
ofproto_port_set_realdev(struct ofproto *ofproto, uint16_t vlandev_ofp_port,
                         uint16_t realdev_ofp_port, int vid)
{
    struct ofport *ofport;
    int error;

    assert(vlandev_ofp_port != realdev_ofp_port);

    ofport = ofproto_get_port(ofproto, vlandev_ofp_port);
    if (!ofport) {
        VLOG_WARN("%s: cannot set realdev on nonexistent port %"PRIu16,
                  ofproto->name, vlandev_ofp_port);
        return EINVAL;
    }

    if (!ofproto->ofproto_class->set_realdev) {
        if (!vlandev_ofp_port) {
            return 0;
        }
        VLOG_WARN("%s: vlan splinters not supported", ofproto->name);
        return EOPNOTSUPP;
    }

    error = ofproto->ofproto_class->set_realdev(ofport, realdev_ofp_port, vid);
    if (error) {
        VLOG_WARN("%s: setting realdev on port %"PRIu16" (%s) failed (%s)",
                  ofproto->name, vlandev_ofp_port,
                  netdev_get_name(ofport->netdev), strerror(error));
    }
    return error;
}

int
vsctl(int argc, char *argv[])
{
    extern struct vlog_module VLM_reconnect;
    enum ovsdb_idl_txn_status status;
    struct ovsdb_idl *idl;
    struct vsctl_command *commands;
    size_t n_commands;
    char *args;

    set_program_name(argv[0]);
    signal(SIGPIPE, SIG_IGN);
    vlog_set_levels(NULL, VLF_CONSOLE, VLL_WARN);
    vlog_set_levels(&VLM_reconnect, VLF_ANY_FACILITY, VLL_WARN);
    ovsrec_init();

    /* Log our arguments.  This is often valuable for debugging systems. */
    //args = process_escape_args(argv);
    //VLOG(might_write_to_db(argv) ? VLL_INFO : VLL_DBG, "Called as %s", args);
    
    

    /* Parse command line. */
    //parse_options(argc, argv);

   
  
    commands = parse_commands(argc - 2, argv + 2, &n_commands);

    if (timeout) {
        time_alarm(timeout);
    }
    db = default_db();

    /* Initialize IDL. */
    idl = the_idl = ovsdb_idl_create(db, &ovsrec_idl_class, false);
    run_prerequisites(commands, n_commands, idl);

    /* Now execute the commands. */
    status = TXN_AGAIN_WAIT;
    for (;;) {
        if (ovsdb_idl_run(idl) || status == TXN_AGAIN_NOW) {
	    
            status = do_vsctl(args, commands, n_commands, idl);
	    goto exit;
        }

        if (status != TXN_AGAIN_NOW) {
            ovsdb_idl_wait(idl);
            poll_block();
        }
    }
    exit: ;
}

static void
parse_options(int argc, char *argv[])
{
    enum {
        OPT_DB = UCHAR_MAX + 1,
        OPT_ONELINE,
        OPT_NO_SYSLOG,
        OPT_NO_WAIT,
        OPT_DRY_RUN,
        OPT_PEER_CA_CERT,
        VLOG_OPTION_ENUMS,
        TABLE_OPTION_ENUMS
    };
    static struct option long_options[] = {
        {"db", required_argument, NULL, OPT_DB},
        {"no-syslog", no_argument, NULL, OPT_NO_SYSLOG},
        {"no-wait", no_argument, NULL, OPT_NO_WAIT},
        {"dry-run", no_argument, NULL, OPT_DRY_RUN},
        {"oneline", no_argument, NULL, OPT_ONELINE},
        {"timeout", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        VLOG_LONG_OPTIONS,
        TABLE_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {NULL, 0, NULL, 0},
    };
    char *tmp, *short_options;

    tmp = long_options_to_short_options(long_options);
    short_options = xasprintf("+%s", tmp);
    free(tmp);

    table_style.format = TF_LIST;

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case OPT_DB:
            db = optarg;
            break;

        case OPT_ONELINE:
            oneline = true;
            break;

        case OPT_NO_SYSLOG:
           // vlog_set_levels(&VLM_vsctl, VLF_SYSLOG, VLL_WARN);
            break;

        case OPT_NO_WAIT:
            wait_for_reload = false;
            break;

        case OPT_DRY_RUN:
            dry_run = true;
            break;

        case 'h':
            usage();

        case 'V':
            ovs_print_version(0, 0);
            exit(EXIT_SUCCESS);

        case 't':
            timeout = strtoul(optarg, NULL, 10);
            if (timeout < 0) {
                vsctl_fatal("value %s on -t or --timeout is invalid",
                            optarg);
            }
            break;

        VLOG_OPTION_HANDLERS
        TABLE_OPTION_HANDLERS(&table_style)

        STREAM_SSL_OPTION_HANDLERS

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    if (!db) {
        db = default_db();
    }
}

static struct vsctl_command *
parse_commands(int argc, char *argv[], size_t *n_commandsp)
{
    struct vsctl_command *commands;
    size_t n_commands, allocated_commands;
    int i, start;

    commands = NULL;
    n_commands = allocated_commands = 0;

    for (start = i = 0; i <= argc; i++) {
	
        if (i == argc || !strcmp(argv[i], "--")) {
	  
            if (i > start) {
                if (n_commands >= allocated_commands) {
                    struct vsctl_command *c;

                    commands = x2nrealloc(commands, &allocated_commands,
                                          sizeof *commands);
                    for (c = commands; c < &commands[n_commands]; c++) {
                        shash_moved(&c->options);
                    }
                }
		
                parse_command(i - start, &argv[start],
                              &commands[n_commands++]);
            }
            start = i + 1;
        }
    }
    if (!n_commands) {
        vsctl_fatal("missing command name (use --help for help)");
    }
    *n_commandsp = n_commands;
    return commands;
}

static void
parse_command(int argc, char *argv[], struct vsctl_command *command)
{
    const struct vsctl_command_syntax *p;
    struct shash_node *node;
    int n_arg;
    int i;

    

    shash_init(&command->options);
    for (i = 0; i < argc; i++) {
        const char *option = argv[i];
        const char *equals;
        char *key, *value;

        if (option[0] != '-') {
            break;
        }

        equals = strchr(option, '=');
        if (equals) {
            key = xmemdup0(option, equals - option);
            value = xstrdup(equals + 1);
        } else {
            key = xstrdup(option);
            value = NULL;
        }

        if (shash_find(&command->options, key)) {
            vsctl_fatal("'%s' option specified multiple times", argv[i]);
        }
        shash_add_nocopy(&command->options, key, value);
    }
    if (i == argc) {
        vsctl_fatal("missing command name");
    }

    p = find_command(argv[i]);
    if (!p) {
        vsctl_fatal("unknown command '%s'; use --help for help", argv[i]);
    }

    SHASH_FOR_EACH (node, &command->options) {
        const char *s = strstr(p->options, node->name);
        int end = s ? s[strlen(node->name)] : EOF;

        if (end != '=' && end != ',' && end != ' ' && end != '\0') {
            vsctl_fatal("'%s' command has no '%s' option",
                        argv[i], node->name);
        }
        if ((end == '=') != (node->data != NULL)) {
            if (end == '=') {
                vsctl_fatal("missing argument to '%s' option on '%s' "
                            "command", node->name, argv[i]);
            } else {
                vsctl_fatal("'%s' option on '%s' does not accept an "
                            "argument", node->name, argv[i]);
            }
        }
    }

    n_arg = argc - i - 1;
   
    if (n_arg < p->min_args) {
        vsctl_fatal("'%s' command requires at least %d arguments",
                    p->name, p->min_args);
    } else if (n_arg > p->max_args) {
        int j;

        for (j = i + 1; j < argc; j++) {
            if (argv[j][0] == '-') {
                vsctl_fatal("'%s' command takes at most %d arguments "
                            "(note that options must precede command "
                            "names and follow a \"--\" argument)",
                            p->name, p->max_args);
            }
        }

        vsctl_fatal("'%s' command takes at most %d arguments",
                    p->name, p->max_args);
    }

    command->syntax = p;
    command->argc = n_arg + 1;
    command->argv = &argv[i];
}

/* Returns the "struct vsctl_command_syntax" for a given command 'name', or a
 * null pointer if there is none. */
static const struct vsctl_command_syntax *
find_command(const char *name)
{
    static struct shash commands = SHASH_INITIALIZER(&commands);

    if (shash_is_empty(&commands)) {
        const struct vsctl_command_syntax *p;

        for (p = all_commands; p->name; p++) {
            shash_add_assert(&commands, p->name, p);
        }
    }

    return shash_find_data(&commands, name);
}

static void
vsctl_fatal(const char *format, ...)
{
    char *message;
    va_list args;

    va_start(args, format);
    message = xvasprintf(format, args);
    va_end(args);

   // vlog_set_levels(&VLM_vsctl, VLF_CONSOLE, VLL_OFF);
    VLOG_ERR("%s", message);
    ovs_error(0, "%s", message);
    vsctl_exit(EXIT_FAILURE);
}

/* Frees the current transaction and the underlying IDL and then calls
 * exit(status).
 *
 * Freeing the transaction and the IDL is not strictly necessary, but it makes
 * for a clean memory leak report from valgrind in the normal case.  That makes
 * it easier to notice real memory leaks. */
static void
vsctl_exit(int status)
{
    if (the_idl_txn) {
        ovsdb_idl_txn_abort(the_idl_txn);
        ovsdb_idl_txn_destroy(the_idl_txn);
    }
    ovsdb_idl_destroy(the_idl);
    exit(status);
}

static void
usage(void)
{
    printf("\
%s: ovs-vswitchd management utility\n\
usage: %s [OPTIONS] COMMAND [ARG...]\n\
\n\
Open vSwitch commands:\n\
  init                        initialize database, if not yet initialized\n\
  show                        print overview of database contents\n\
  emer-reset                  reset configuration to clean state\n\
\n\
Bridge commands:\n\
  add-br BRIDGE               create a new bridge named BRIDGE\n\
  add-br BRIDGE PARENT VLAN   create new fake BRIDGE in PARENT on VLAN\n\
  del-br BRIDGE               delete BRIDGE and all of its ports\n\
  list-br                     print the names of all the bridges\n\
  br-exists BRIDGE            test whether BRIDGE exists\n\
  br-to-vlan BRIDGE           print the VLAN which BRIDGE is on\n\
  br-to-parent BRIDGE         print the parent of BRIDGE\n\
  br-set-external-id BRIDGE KEY VALUE  set KEY on BRIDGE to VALUE\n\
  br-set-external-id BRIDGE KEY  unset KEY on BRIDGE\n\
  br-get-external-id BRIDGE KEY  print value of KEY on BRIDGE\n\
  br-get-external-id BRIDGE  list key-value pairs on BRIDGE\n\
\n\
Port commands (a bond is considered to be a single port):\n\
  list-ports BRIDGE           print the names of all the ports on BRIDGE\n\
  add-port BRIDGE PORT        add network device PORT to BRIDGE\n\
  add-bond BRIDGE PORT IFACE...  add bonded port PORT in BRIDGE from IFACES\n\
  del-port [BRIDGE] PORT      delete PORT (which may be bonded) from BRIDGE\n\
  port-to-br PORT             print name of bridge that contains PORT\n\
\n\
Interface commands (a bond consists of multiple interfaces):\n\
  list-ifaces BRIDGE          print the names of all interfaces on BRIDGE\n\
  iface-to-br IFACE           print name of bridge that contains IFACE\n\
\n\
Controller commands:\n\
  get-controller BRIDGE      print the controllers for BRIDGE\n\
  del-controller BRIDGE      delete the controllers for BRIDGE\n\
  set-controller BRIDGE TARGET...  set the controllers for BRIDGE\n\
  get-fail-mode BRIDGE       print the fail-mode for BRIDGE\n\
  del-fail-mode BRIDGE       delete the fail-mode for BRIDGE\n\
  set-fail-mode BRIDGE MODE  set the fail-mode for BRIDGE to MODE\n\
\n\
Manager commands:\n\
  get-manager                print the managers\n\
  del-manager                delete the managers\n\
  set-manager TARGET...      set the list of managers to TARGET...\n\
\n\
SSL commands:\n\
  get-ssl                     print the SSL configuration\n\
  del-ssl                     delete the SSL configuration\n\
  set-ssl PRIV-KEY CERT CA-CERT  set the SSL configuration\n\
\n\
Switch commands:\n\
  emer-reset                  reset switch to known good state\n\
\n\
Database commands:\n\
  list TBL [REC]              list RECord (or all records) in TBL\n\
  find TBL CONDITION...       list records satisfying CONDITION in TBL\n\
  get TBL REC COL[:KEY]       print values of COLumns in RECord in TBL\n\
  set TBL REC COL[:KEY]=VALUE set COLumn values in RECord in TBL\n\
  add TBL REC COL [KEY=]VALUE add (KEY=)VALUE to COLumn in RECord in TBL\n\
  remove TBL REC COL [KEY=]VALUE  remove (KEY=)VALUE from COLumn\n\
  clear TBL REC COL           clear values from COLumn in RECord in TBL\n\
  create TBL COL[:KEY]=VALUE  create and initialize new record\n\
  destroy TBL REC             delete RECord from TBL\n\
  wait-until TBL REC [COL[:KEY]=VALUE]  wait until condition is true\n\
Potentially unsafe database commands require --force option.\n\
\n\
Options:\n\
  --db=DATABASE               connect to DATABASE\n\
                              (default: %s)\n\
  --no-wait                   do not wait for ovs-vswitchd to reconfigure\n\
  -t, --timeout=SECS          wait at most SECS seconds for ovs-vswitchd\n\
  --dry-run                   do not commit changes to database\n\
  --oneline                   print exactly one line of output per command\n",
           program_name, program_name, default_db());
    vlog_usage();
    printf("\
  --no-syslog             equivalent to --verbose=vsctl:syslog:warn\n");
    stream_usage("database", true, true, false);
    printf("\n\
Other options:\n\
  -h, --help                  display this help message\n\
  -V, --version               display version information\n");
    exit(EXIT_SUCCESS);
}

static char *
default_db(void)
{
    static char *def;
    if (!def) {
        def = xasprintf("unix:%s/db.sock", ovs_rundir());
    }
    return def;
}

/* Returns true if it looks like this set of arguments might modify the
 * database, otherwise false.  (Not very smart, so it's prone to false
 * positives.) */
static bool
might_write_to_db(char **argv)
{
    for (; *argv; argv++) {
        const struct vsctl_command_syntax *p = find_command(*argv);
        if (p && p->mode == RW) {
            return true;
        }
    }
    return false;
}

struct vsctl_context {
    /* Read-only. */
    int argc;
    char **argv;
    struct shash options;

    /* Modifiable state. */
    struct ds output;
    struct table *table;
    struct ovsdb_idl *idl;
    struct ovsdb_idl_txn *txn;
    struct ovsdb_symbol_table *symtab;
    const struct ovsrec_open_vswitch *ovs;
    bool verified_ports;

    /* A command may set this member to true if some prerequisite is not met
     * and the caller should wait for something to change and then retry. */
    bool try_again;
};

struct vsctl_bridge {
    struct ovsrec_bridge *br_cfg;
    char *name;
    struct ovsrec_controller **ctrl;
    char *fail_mode;
    size_t n_ctrl;

    /* VLAN ("fake") bridge support.
     *
     * Use 'parent != NULL' to detect a fake bridge, because 'vlan' can be 0
     * in either case. */
    struct vsctl_bridge *parent; /* Real bridge, or NULL. */
    int vlan;                    /* VLAN VID (0...4095), or 0. */
};

struct vsctl_port {
    struct ovsrec_port *port_cfg;
    struct vsctl_bridge *bridge;
};

struct vsctl_iface {
    struct ovsrec_interface *iface_cfg;
    struct vsctl_port *port;
};

struct vsctl_info {
    struct vsctl_context *ctx;
    struct shash bridges;   /* Maps from bridge name to struct vsctl_bridge. */
    struct shash ports;     /* Maps from port name to struct vsctl_port. */
    struct shash ifaces;    /* Maps from port name to struct vsctl_iface. */
};

static char *
vsctl_context_to_string(const struct vsctl_context *ctx)
{
    const struct shash_node *node;
    struct svec words;
    char *s;
    int i;

    svec_init(&words);
    SHASH_FOR_EACH (node, &ctx->options) {
        svec_add(&words, node->name);
    }
    for (i = 0; i < ctx->argc; i++) {
        svec_add(&words, ctx->argv[i]);
    }
    svec_terminate(&words);

    s = process_escape_args(words.names);

    svec_destroy(&words);

    return s;
}

static void
verify_ports(struct vsctl_context *ctx)
{
    if (!ctx->verified_ports) {
        const struct ovsrec_bridge *bridge;
        const struct ovsrec_port *port;

        ovsrec_open_vswitch_verify_bridges(ctx->ovs);
        OVSREC_BRIDGE_FOR_EACH (bridge, ctx->idl) {
            ovsrec_bridge_verify_ports(bridge);
        }
        OVSREC_PORT_FOR_EACH (port, ctx->idl) {
            ovsrec_port_verify_interfaces(port);
        }

        ctx->verified_ports = true;
    }
}

static struct vsctl_bridge *
add_bridge(struct vsctl_info *b,
           struct ovsrec_bridge *br_cfg, const char *name,
           struct vsctl_bridge *parent, int vlan)
{
    struct vsctl_bridge *br = xmalloc(sizeof *br);
    br->br_cfg = br_cfg;
    br->name = xstrdup(name);
    br->parent = parent;
    br->vlan = vlan;
    if (parent) {
        br->ctrl = parent->br_cfg->controller;
        br->n_ctrl = parent->br_cfg->n_controller;
        br->fail_mode = parent->br_cfg->fail_mode;
    } else {
        br->ctrl = br_cfg->controller;
        br->n_ctrl = br_cfg->n_controller;
        br->fail_mode = br_cfg->fail_mode;
    }
    shash_add(&b->bridges, br->name, br);
    return br;
}

static bool
port_is_fake_bridge(const struct ovsrec_port *port_cfg)
{
    return (port_cfg->fake_bridge
            && port_cfg->tag
            && *port_cfg->tag >= 0 && *port_cfg->tag <= 4095);
}

static struct vsctl_bridge *
find_vlan_bridge(struct vsctl_info *info,
                 struct vsctl_bridge *parent, int vlan)
{
    struct shash_node *node;

    SHASH_FOR_EACH (node, &info->bridges) {
        struct vsctl_bridge *br = node->data;
        if (br->parent == parent && br->vlan == vlan) {
            return br;
        }
    }

    return NULL;
}

static void
free_info(struct vsctl_info *info)
{
    struct shash_node *node;

    SHASH_FOR_EACH (node, &info->bridges) {
        struct vsctl_bridge *bridge = node->data;
        free(bridge->name);
        free(bridge);
    }
    shash_destroy(&info->bridges);

    shash_destroy_free_data(&info->ports);
    shash_destroy_free_data(&info->ifaces);
}

static void
pre_get_info(struct vsctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_bridges);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_name);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_controller);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_fail_mode);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_ports);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_port_col_name);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_port_col_fake_bridge);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_port_col_tag);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_port_col_interfaces);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_interface_col_name);
}

static void
get_info(struct vsctl_context *ctx, struct vsctl_info *info)
{
    const struct ovsrec_open_vswitch *ovs = ctx->ovs;
    struct sset bridges, ports;
    size_t i;

    info->ctx = ctx;
    shash_init(&info->bridges);
    shash_init(&info->ports);
    shash_init(&info->ifaces);

    sset_init(&bridges);
    sset_init(&ports);
    for (i = 0; i < ovs->n_bridges; i++) {
        struct ovsrec_bridge *br_cfg = ovs->bridges[i];
        struct vsctl_bridge *br;
        size_t j;

        if (!sset_add(&bridges, br_cfg->name)) {
            VLOG_WARN("%s: database contains duplicate bridge name",
                      br_cfg->name);
            continue;
        }
        br = add_bridge(info, br_cfg, br_cfg->name, NULL, 0);
        if (!br) {
            continue;
        }

        for (j = 0; j < br_cfg->n_ports; j++) {
            struct ovsrec_port *port_cfg = br_cfg->ports[j];

            if (!sset_add(&ports, port_cfg->name)) {
                /* Duplicate port name.  (We will warn about that later.) */
                continue;
            }

            if (port_is_fake_bridge(port_cfg)
                && sset_add(&bridges, port_cfg->name)) {
                add_bridge(info, NULL, port_cfg->name, br, *port_cfg->tag);
            }
        }
    }
    sset_destroy(&bridges);
    sset_destroy(&ports);

    sset_init(&bridges);
    for (i = 0; i < ovs->n_bridges; i++) {
        struct ovsrec_bridge *br_cfg = ovs->bridges[i];
        struct vsctl_bridge *br;
        size_t j;

        if (!sset_add(&bridges, br_cfg->name)) {
            continue;
        }
        br = shash_find_data(&info->bridges, br_cfg->name);
        for (j = 0; j < br_cfg->n_ports; j++) {
            struct ovsrec_port *port_cfg = br_cfg->ports[j];
            struct vsctl_port *port;
            size_t k;

            port = shash_find_data(&info->ports, port_cfg->name);
            if (port) {
                if (port_cfg == port->port_cfg) {
                    VLOG_WARN("%s: port is in multiple bridges (%s and %s)",
                              port_cfg->name, br->name, port->bridge->name);
                } else {
                    /* Log as an error because this violates the database's
                     * uniqueness constraints, so the database server shouldn't
                     * have allowed it. */
                    VLOG_ERR("%s: database contains duplicate port name",
                             port_cfg->name);
                }
                continue;
            }

            if (port_is_fake_bridge(port_cfg)
                && !sset_add(&bridges, port_cfg->name)) {
                continue;
            }

            port = xmalloc(sizeof *port);
            port->port_cfg = port_cfg;
            if (port_cfg->tag
                && *port_cfg->tag >= 0 && *port_cfg->tag <= 4095) {
                port->bridge = find_vlan_bridge(info, br, *port_cfg->tag);
                if (!port->bridge) {
                    port->bridge = br;
                }
            } else {
                port->bridge = br;
            }
            shash_add(&info->ports, port_cfg->name, port);

            for (k = 0; k < port_cfg->n_interfaces; k++) {
                struct ovsrec_interface *iface_cfg = port_cfg->interfaces[k];
                struct vsctl_iface *iface;

                iface = shash_find_data(&info->ifaces, iface_cfg->name);
                if (iface) {
                    if (iface_cfg == iface->iface_cfg) {
                        VLOG_WARN("%s: interface is in multiple ports "
                                  "(%s and %s)",
                                  iface_cfg->name,
                                  iface->port->port_cfg->name,
                                  port->port_cfg->name);
                    } else {
                        /* Log as an error because this violates the database's
                         * uniqueness constraints, so the database server
                         * shouldn't have allowed it. */
                        VLOG_ERR("%s: database contains duplicate interface "
                                 "name", iface_cfg->name);
                    }
                    continue;
                }

                iface = xmalloc(sizeof *iface);
                iface->iface_cfg = iface_cfg;
                iface->port = port;
                shash_add(&info->ifaces, iface_cfg->name, iface);
            }
        }
    }
    sset_destroy(&bridges);
}

static void
check_conflicts(struct vsctl_info *info, const char *name,
                char *msg)
{
    struct vsctl_iface *iface;
    struct vsctl_port *port;

    verify_ports(info->ctx);

    if (shash_find(&info->bridges, name)) {
        vsctl_fatal("%s because a bridge named %s already exists",
                    msg, name);
    }

    port = shash_find_data(&info->ports, name);
    if (port) {
        vsctl_fatal("%s because a port named %s already exists on "
                    "bridge %s", msg, name, port->bridge->name);
    }

    iface = shash_find_data(&info->ifaces, name);
    if (iface) {
        vsctl_fatal("%s because an interface named %s already exists "
                    "on bridge %s", msg, name, iface->port->bridge->name);
    }

    free(msg);
}

static struct vsctl_bridge *
find_bridge(struct vsctl_info *info, const char *name, bool must_exist)
{
    struct vsctl_bridge *br = shash_find_data(&info->bridges, name);
    if (must_exist && !br) {
        vsctl_fatal("no bridge named %s", name);
    }
    ovsrec_open_vswitch_verify_bridges(info->ctx->ovs);
    return br;
}

static struct vsctl_bridge *
find_real_bridge(struct vsctl_info *info, const char *name, bool must_exist)
{
    struct vsctl_bridge *br = find_bridge(info, name, must_exist);
    if (br && br->parent) {
        vsctl_fatal("%s is a fake bridge", name);
    }
    return br;
}

static struct vsctl_port *
find_port(struct vsctl_info *info, const char *name, bool must_exist)
{
    struct vsctl_port *port = shash_find_data(&info->ports, name);
    if (port && !strcmp(name, port->bridge->name)) {
        port = NULL;
    }
    if (must_exist && !port) {
        vsctl_fatal("no port named %s", name);
    }
    verify_ports(info->ctx);
    return port;
}

static struct vsctl_iface *
find_iface(struct vsctl_info *info, const char *name, bool must_exist)
{
    struct vsctl_iface *iface = shash_find_data(&info->ifaces, name);
    if (iface && !strcmp(name, iface->port->bridge->name)) {
        iface = NULL;
    }
    if (must_exist && !iface) {
        vsctl_fatal("no interface named %s", name);
    }
    verify_ports(info->ctx);
    return iface;
}

static void
bridge_insert_port(struct ovsrec_bridge *br, struct ovsrec_port *port)
{
    struct ovsrec_port **ports;
    size_t i;

    ports = xmalloc(sizeof *br->ports * (br->n_ports + 1));
    for (i = 0; i < br->n_ports; i++) {
        ports[i] = br->ports[i];
    }
    ports[br->n_ports] = port;
    ovsrec_bridge_set_ports(br, ports, br->n_ports + 1);
    free(ports);
}

static void
bridge_delete_port(struct ovsrec_bridge *br, struct ovsrec_port *port)
{
    struct ovsrec_port **ports;
    size_t i, n;

    ports = xmalloc(sizeof *br->ports * br->n_ports);
    for (i = n = 0; i < br->n_ports; i++) {
        if (br->ports[i] != port) {
            ports[n++] = br->ports[i];
        }
    }
    ovsrec_bridge_set_ports(br, ports, n);
    free(ports);
}

static void
ovs_insert_bridge(const struct ovsrec_open_vswitch *ovs,
                  struct ovsrec_bridge *bridge)
{
    struct ovsrec_bridge **bridges;
    size_t i;

    bridges = xmalloc(sizeof *ovs->bridges * (ovs->n_bridges + 1));
    for (i = 0; i < ovs->n_bridges; i++) {
        bridges[i] = ovs->bridges[i];
    }
    bridges[ovs->n_bridges] = bridge;
    ovsrec_open_vswitch_set_bridges(ovs, bridges, ovs->n_bridges + 1);
    free(bridges);
}

static void
ovs_delete_bridge(const struct ovsrec_open_vswitch *ovs,
                  struct ovsrec_bridge *bridge)
{
    struct ovsrec_bridge **bridges;
    size_t i, n;

    bridges = xmalloc(sizeof *ovs->bridges * ovs->n_bridges);
    for (i = n = 0; i < ovs->n_bridges; i++) {
        if (ovs->bridges[i] != bridge) {
            bridges[n++] = ovs->bridges[i];
        }
    }
    ovsrec_open_vswitch_set_bridges(ovs, bridges, n);
    free(bridges);
}

static void
cmd_init(struct vsctl_context *ctx OVS_UNUSED)
{
}

struct cmd_show_table {
    const struct ovsdb_idl_table_class *table;
    const struct ovsdb_idl_column *name_column;
    const struct ovsdb_idl_column *columns[3];
    bool recurse;
};

static struct cmd_show_table cmd_show_tables[] = {
    {&ovsrec_table_open_vswitch,
     NULL,
     {&ovsrec_open_vswitch_col_manager_options,
      &ovsrec_open_vswitch_col_bridges,
      &ovsrec_open_vswitch_col_ovs_version},
     false},

    {&ovsrec_table_bridge,
     &ovsrec_bridge_col_name,
     {&ovsrec_bridge_col_controller,
      &ovsrec_bridge_col_fail_mode,
      &ovsrec_bridge_col_ports},
     false},

    {&ovsrec_table_port,
     &ovsrec_port_col_name,
     {&ovsrec_port_col_tag,
      &ovsrec_port_col_trunks,
      &ovsrec_port_col_interfaces},
     false},

    {&ovsrec_table_interface,
     &ovsrec_interface_col_name,
     {&ovsrec_interface_col_type,
      &ovsrec_interface_col_options,
      NULL},
     false},

    {&ovsrec_table_controller,
     &ovsrec_controller_col_target,
     {&ovsrec_controller_col_is_connected,
      NULL,
      NULL},
     false},

    {&ovsrec_table_manager,
     &ovsrec_manager_col_target,
     {&ovsrec_manager_col_is_connected,
      NULL,
      NULL},
     false},
};

static void
pre_cmd_show(struct vsctl_context *ctx)
{
    struct cmd_show_table *show;

    for (show = cmd_show_tables;
         show < &cmd_show_tables[ARRAY_SIZE(cmd_show_tables)];
         show++) {
        size_t i;

        ovsdb_idl_add_table(ctx->idl, show->table);
        if (show->name_column) {
            ovsdb_idl_add_column(ctx->idl, show->name_column);
        }
        for (i = 0; i < ARRAY_SIZE(show->columns); i++) {
            const struct ovsdb_idl_column *column = show->columns[i];
            if (column) {
                ovsdb_idl_add_column(ctx->idl, column);
            }
        }
    }
}

static struct cmd_show_table *
cmd_show_find_table_by_row(const struct ovsdb_idl_row *row)
{
    struct cmd_show_table *show;

    for (show = cmd_show_tables;
         show < &cmd_show_tables[ARRAY_SIZE(cmd_show_tables)];
         show++) {
        if (show->table == row->table->class) {
            return show;
        }
    }
    return NULL;
}

static struct cmd_show_table *
cmd_show_find_table_by_name(const char *name)
{
    struct cmd_show_table *show;

    for (show = cmd_show_tables;
         show < &cmd_show_tables[ARRAY_SIZE(cmd_show_tables)];
         show++) {
        if (!strcmp(show->table->name, name)) {
            return show;
        }
    }
    return NULL;
}

static void
cmd_show_row(struct vsctl_context *ctx, const struct ovsdb_idl_row *row,
             int level)
{
    struct cmd_show_table *show = cmd_show_find_table_by_row(row);
    size_t i;

    ds_put_char_multiple(&ctx->output, ' ', level * 4);
    if (show && show->name_column) {
        const struct ovsdb_datum *datum;

        ds_put_format(&ctx->output, "%s ", show->table->name);
        datum = ovsdb_idl_read(row, show->name_column);
        ovsdb_datum_to_string(datum, &show->name_column->type, &ctx->output);
    } else {
        ds_put_format(&ctx->output, UUID_FMT, UUID_ARGS(&row->uuid));
    }
    ds_put_char(&ctx->output, '\n');

    if (!show || show->recurse) {
        return;
    }

    show->recurse = true;
    for (i = 0; i < ARRAY_SIZE(show->columns); i++) {
        const struct ovsdb_idl_column *column = show->columns[i];
        const struct ovsdb_datum *datum;

        if (!column) {
            break;
        }

        datum = ovsdb_idl_read(row, column);
        if (column->type.key.type == OVSDB_TYPE_UUID &&
            column->type.key.u.uuid.refTableName) {
            struct cmd_show_table *ref_show;
            size_t j;

            ref_show = cmd_show_find_table_by_name(
                column->type.key.u.uuid.refTableName);
            if (ref_show) {
                for (j = 0; j < datum->n; j++) {
                    const struct ovsdb_idl_row *ref_row;

                    ref_row = ovsdb_idl_get_row_for_uuid(ctx->idl,
                                                         ref_show->table,
                                                         &datum->keys[j].uuid);
                    if (ref_row) {
                        cmd_show_row(ctx, ref_row, level + 1);
                    }
                }
                continue;
            }
        }

        if (!ovsdb_datum_is_default(datum, &column->type)) {
            ds_put_char_multiple(&ctx->output, ' ', (level + 1) * 4);
            ds_put_format(&ctx->output, "%s: ", column->name);
            ovsdb_datum_to_string(datum, &column->type, &ctx->output);
            ds_put_char(&ctx->output, '\n');
        }
    }
    show->recurse = false;
}

static void
cmd_show(struct vsctl_context *ctx)
{
    const struct ovsdb_idl_row *row;

    for (row = ovsdb_idl_first_row(ctx->idl, cmd_show_tables[0].table);
         row; row = ovsdb_idl_next_row(row)) {
        cmd_show_row(ctx, row, 0);
    }
}

static void
pre_cmd_emer_reset(struct vsctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_manager_options);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_ssl);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_controller);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_fail_mode);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_mirrors);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_netflow);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_sflow);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_flood_vlans);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_other_config);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_port_col_other_config);

    ovsdb_idl_add_column(ctx->idl,
                          &ovsrec_interface_col_ingress_policing_rate);
    ovsdb_idl_add_column(ctx->idl,
                          &ovsrec_interface_col_ingress_policing_burst);
}

static void
cmd_emer_reset(struct vsctl_context *ctx)
{
    const struct ovsdb_idl *idl = ctx->idl;
    const struct ovsrec_bridge *br;
    const struct ovsrec_port *port;
    const struct ovsrec_interface *iface;
    const struct ovsrec_mirror *mirror, *next_mirror;
    const struct ovsrec_controller *ctrl, *next_ctrl;
    const struct ovsrec_manager *mgr, *next_mgr;
    const struct ovsrec_netflow *nf, *next_nf;
    const struct ovsrec_ssl *ssl, *next_ssl;
    const struct ovsrec_sflow *sflow, *next_sflow;

    /* Reset the Open_vSwitch table. */
    ovsrec_open_vswitch_set_manager_options(ctx->ovs, NULL, 0);
    ovsrec_open_vswitch_set_ssl(ctx->ovs, NULL);

    OVSREC_BRIDGE_FOR_EACH (br, idl) {
        int i;
        char *hw_key = "hwaddr";
        char *hw_val = NULL;

        ovsrec_bridge_set_controller(br, NULL, 0);
        ovsrec_bridge_set_fail_mode(br, NULL);
        ovsrec_bridge_set_mirrors(br, NULL, 0);
        ovsrec_bridge_set_netflow(br, NULL);
        ovsrec_bridge_set_sflow(br, NULL);
        ovsrec_bridge_set_flood_vlans(br, NULL, 0);

        /* We only want to save the "hwaddr" key from other_config. */
        for (i=0; i < br->n_other_config; i++) {
            if (!strcmp(br->key_other_config[i], hw_key)) {
                hw_val = br->value_other_config[i];
                break;
            }
        }
        if (hw_val) {
            char *val = xstrdup(hw_val);
            ovsrec_bridge_set_other_config(br, &hw_key, &val, 1);
            free(val);
        } else {
            ovsrec_bridge_set_other_config(br, NULL, NULL, 0);
        }
    }

    OVSREC_PORT_FOR_EACH (port, idl) {
        ovsrec_port_set_other_config(port, NULL, NULL, 0);
    }

    OVSREC_INTERFACE_FOR_EACH (iface, idl) {
        /* xxx What do we do about gre/patch devices created by mgr? */

        ovsrec_interface_set_ingress_policing_rate(iface, 0);
        ovsrec_interface_set_ingress_policing_burst(iface, 0);
    }

    OVSREC_MIRROR_FOR_EACH_SAFE (mirror, next_mirror, idl) {
        ovsrec_mirror_delete(mirror);
    }

    OVSREC_CONTROLLER_FOR_EACH_SAFE (ctrl, next_ctrl, idl) {
        ovsrec_controller_delete(ctrl);
    }

    OVSREC_MANAGER_FOR_EACH_SAFE (mgr, next_mgr, idl) {
        ovsrec_manager_delete(mgr);
    }

    OVSREC_NETFLOW_FOR_EACH_SAFE (nf, next_nf, idl) {
        ovsrec_netflow_delete(nf);
    }

    OVSREC_SSL_FOR_EACH_SAFE (ssl, next_ssl, idl) {
        ovsrec_ssl_delete(ssl);
    }

    OVSREC_SFLOW_FOR_EACH_SAFE (sflow, next_sflow, idl) {
        ovsrec_sflow_delete(sflow);
    }
}

static void
cmd_add_br(struct vsctl_context *ctx)
{
    bool may_exist = shash_find(&ctx->options, "--may-exist") != NULL;
    const char *br_name, *parent_name;
    struct vsctl_info info;
    int vlan;

    br_name = ctx->argv[1];
    if (ctx->argc == 2) {
        parent_name = NULL;
        vlan = 0;
    } else if (ctx->argc == 4) {
        parent_name = ctx->argv[2];
        vlan = atoi(ctx->argv[3]);
        if (vlan < 0 || vlan > 4095) {
            vsctl_fatal("%s: vlan must be between 0 and 4095", ctx->argv[0]);
        }
    } else {
        vsctl_fatal("'%s' command takes exactly 1 or 3 arguments",
                    ctx->argv[0]);
    }

    get_info(ctx, &info);
    if (may_exist) {
        struct vsctl_bridge *br;

        br = find_bridge(&info, br_name, false);
        if (br) {
            if (!parent_name) {
                if (br->parent) {
                    vsctl_fatal("\"--may-exist add-br %s\" but %s is "
                                "a VLAN bridge for VLAN %d",
                                br_name, br_name, br->vlan);
                }
            } else {
                if (!br->parent) {
                    vsctl_fatal("\"--may-exist add-br %s %s %d\" but %s "
                                "is not a VLAN bridge",
                                br_name, parent_name, vlan, br_name);
                } else if (strcmp(br->parent->name, parent_name)) {
                    vsctl_fatal("\"--may-exist add-br %s %s %d\" but %s "
                                "has the wrong parent %s",
                                br_name, parent_name, vlan,
                                br_name, br->parent->name);
                } else if (br->vlan != vlan) {
                    vsctl_fatal("\"--may-exist add-br %s %s %d\" but %s "
                                "is a VLAN bridge for the wrong VLAN %d",
                                br_name, parent_name, vlan, br_name, br->vlan);
                }
            }
            return;
        }
    }
    check_conflicts(&info, br_name,
                    xasprintf("cannot create a bridge named %s", br_name));

    if (!parent_name) {
        struct ovsrec_port *port;
        struct ovsrec_interface *iface;
        struct ovsrec_bridge *br;

        iface = ovsrec_interface_insert(ctx->txn);
        ovsrec_interface_set_name(iface, br_name);
        ovsrec_interface_set_type(iface, "internal");

        port = ovsrec_port_insert(ctx->txn);
        ovsrec_port_set_name(port, br_name);
        ovsrec_port_set_interfaces(port, &iface, 1);

        br = ovsrec_bridge_insert(ctx->txn);
        ovsrec_bridge_set_name(br, br_name);
        ovsrec_bridge_set_ports(br, &port, 1);

        ovs_insert_bridge(ctx->ovs, br);
    } else {
        struct vsctl_bridge *parent;
        struct ovsrec_port *port;
        struct ovsrec_interface *iface;
        struct ovsrec_bridge *br;
        int64_t tag = vlan;

        parent = find_bridge(&info, parent_name, false);
        if (parent && parent->parent) {
            vsctl_fatal("cannot create bridge with fake bridge as parent");
        }
        if (!parent) {
            vsctl_fatal("parent bridge %s does not exist", parent_name);
        }
        br = parent->br_cfg;

        iface = ovsrec_interface_insert(ctx->txn);
        ovsrec_interface_set_name(iface, br_name);
        ovsrec_interface_set_type(iface, "internal");

        port = ovsrec_port_insert(ctx->txn);
        ovsrec_port_set_name(port, br_name);
        ovsrec_port_set_interfaces(port, &iface, 1);
        ovsrec_port_set_fake_bridge(port, true);
        ovsrec_port_set_tag(port, &tag, 1);

        bridge_insert_port(br, port);
    }

    free_info(&info);
}

static void
del_port(struct vsctl_info *info, struct vsctl_port *port)
{
    struct shash_node *node;

    SHASH_FOR_EACH (node, &info->ifaces) {
        struct vsctl_iface *iface = node->data;
        if (iface->port == port) {
            ovsrec_interface_delete(iface->iface_cfg);
        }
    }
    ovsrec_port_delete(port->port_cfg);

    bridge_delete_port((port->bridge->parent
                        ? port->bridge->parent->br_cfg
                        : port->bridge->br_cfg), port->port_cfg);
}

static void
cmd_del_br(struct vsctl_context *ctx)
{
    bool must_exist = !shash_find(&ctx->options, "--if-exists");
    struct vsctl_bridge *bridge;
    struct vsctl_info info;

    get_info(ctx, &info);
    bridge = find_bridge(&info, ctx->argv[1], must_exist);
    if (bridge) {
        struct shash_node *node;

        SHASH_FOR_EACH (node, &info.ports) {
            struct vsctl_port *port = node->data;
            if (port->bridge == bridge || port->bridge->parent == bridge
                || !strcmp(port->port_cfg->name, bridge->name)) {
                del_port(&info, port);
            }
        }
        if (bridge->br_cfg) {
            ovsrec_bridge_delete(bridge->br_cfg);
            ovs_delete_bridge(ctx->ovs, bridge->br_cfg);
        }
    }
    free_info(&info);
}

static void
output_sorted(struct svec *svec, struct ds *output)
{
    const char *name;
    size_t i;

    svec_sort(svec);
    SVEC_FOR_EACH (i, name, svec) {
        ds_put_format(output, "%s\n", name);
    }
}

static void
cmd_list_br(struct vsctl_context *ctx)
{
    struct shash_node *node;
    struct vsctl_info info;
    struct svec bridges;

    get_info(ctx, &info);

    svec_init(&bridges);
    SHASH_FOR_EACH (node, &info.bridges) {
        struct vsctl_bridge *br = node->data;
        svec_add(&bridges, br->name);
    }
    output_sorted(&bridges, &ctx->output);
    svec_destroy(&bridges);

    free_info(&info);
}

static void
cmd_br_exists(struct vsctl_context *ctx)
{
    struct vsctl_info info;

    get_info(ctx, &info);
    if (!find_bridge(&info, ctx->argv[1], false)) {
        vsctl_exit(2);
    }
    free_info(&info);
}

/* Returns true if 'b_prefix' (of length 'b_prefix_len') concatenated with 'b'
 * equals 'a', false otherwise. */
static bool
key_matches(const char *a,
            const char *b_prefix, size_t b_prefix_len, const char *b)
{
    return !strncmp(a, b_prefix, b_prefix_len) && !strcmp(a + b_prefix_len, b);
}

static void
set_external_id(char **old_keys, char **old_values, size_t old_n,
                char *key, char *value,
                char ***new_keysp, char ***new_valuesp, size_t *new_np)
{
    char **new_keys;
    char **new_values;
    size_t new_n;
    size_t i;

    new_keys = xmalloc(sizeof *new_keys * (old_n + 1));
    new_values = xmalloc(sizeof *new_values * (old_n + 1));
    new_n = 0;
    for (i = 0; i < old_n; i++) {
        if (strcmp(key, old_keys[i])) {
            new_keys[new_n] = old_keys[i];
            new_values[new_n] = old_values[i];
            new_n++;
        }
    }
    if (value) {
        new_keys[new_n] = key;
        new_values[new_n] = value;
        new_n++;
    }
    *new_keysp = new_keys;
    *new_valuesp = new_values;
    *new_np = new_n;
}

static void
pre_cmd_br_set_external_id(struct vsctl_context *ctx)
{
    pre_get_info(ctx);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_bridge_col_external_ids);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_port_col_external_ids);
}

static void
cmd_br_set_external_id(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *bridge;
    char **keys, **values;
    size_t n;

    get_info(ctx, &info);
    bridge = find_bridge(&info, ctx->argv[1], true);
    if (bridge->br_cfg) {
        set_external_id(bridge->br_cfg->key_external_ids,
                        bridge->br_cfg->value_external_ids,
                        bridge->br_cfg->n_external_ids,
                        ctx->argv[2], ctx->argc >= 4 ? ctx->argv[3] : NULL,
                        &keys, &values, &n);
        ovsrec_bridge_verify_external_ids(bridge->br_cfg);
        ovsrec_bridge_set_external_ids(bridge->br_cfg, keys, values, n);
    } else {
        char *key = xasprintf("fake-bridge-%s", ctx->argv[2]);
        struct vsctl_port *port = shash_find_data(&info.ports, ctx->argv[1]);
        set_external_id(port->port_cfg->key_external_ids,
                        port->port_cfg->value_external_ids,
                        port->port_cfg->n_external_ids,
                        key, ctx->argc >= 4 ? ctx->argv[3] : NULL,
                        &keys, &values, &n);
        ovsrec_port_verify_external_ids(port->port_cfg);
        ovsrec_port_set_external_ids(port->port_cfg, keys, values, n);
        free(key);
    }
    free(keys);
    free(values);

    free_info(&info);
}

static void
get_external_id(char **keys, char **values, size_t n,
                const char *prefix, const char *key,
                struct ds *output)
{
    size_t prefix_len = strlen(prefix);
    struct svec svec;
    size_t i;

    svec_init(&svec);
    for (i = 0; i < n; i++) {
        if (!key && !strncmp(keys[i], prefix, prefix_len)) {
            svec_add_nocopy(&svec, xasprintf("%s=%s",
                                             keys[i] + prefix_len, values[i]));
        } else if (key && key_matches(keys[i], prefix, prefix_len, key)) {
            svec_add(&svec, values[i]);
            break;
        }
    }
    output_sorted(&svec, output);
    svec_destroy(&svec);
}

static void
pre_cmd_br_get_external_id(struct vsctl_context *ctx)
{
    pre_cmd_br_set_external_id(ctx);
}

static void
cmd_br_get_external_id(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *bridge;

    get_info(ctx, &info);
    bridge = find_bridge(&info, ctx->argv[1], true);
    if (bridge->br_cfg) {
        ovsrec_bridge_verify_external_ids(bridge->br_cfg);
        get_external_id(bridge->br_cfg->key_external_ids,
                        bridge->br_cfg->value_external_ids,
                        bridge->br_cfg->n_external_ids,
                        "", ctx->argc >= 3 ? ctx->argv[2] : NULL,
                        &ctx->output);
    } else {
        struct vsctl_port *port = shash_find_data(&info.ports, ctx->argv[1]);
        ovsrec_port_verify_external_ids(port->port_cfg);
        get_external_id(port->port_cfg->key_external_ids,
                        port->port_cfg->value_external_ids,
                        port->port_cfg->n_external_ids,
                        "fake-bridge-", ctx->argc >= 3 ? ctx->argv[2] : NULL, &ctx->output);
    }
    free_info(&info);
}


static void
cmd_list_ports(struct vsctl_context *ctx)
{
    struct vsctl_bridge *br;
    struct shash_node *node;
    struct vsctl_info info;
    struct svec ports;

    get_info(ctx, &info);
    br = find_bridge(&info, ctx->argv[1], true);
    ovsrec_bridge_verify_ports(br->br_cfg ? br->br_cfg : br->parent->br_cfg);

    svec_init(&ports);
    SHASH_FOR_EACH (node, &info.ports) {
        struct vsctl_port *port = node->data;

        if (strcmp(port->port_cfg->name, br->name) && br == port->bridge) {
            svec_add(&ports, port->port_cfg->name);
        }
    }
    output_sorted(&ports, &ctx->output);
    svec_destroy(&ports);

    free_info(&info);
}

static void
add_port(struct vsctl_context *ctx,
         const char *br_name, const char *port_name,
         bool may_exist, bool fake_iface,
         char *iface_names[], int n_ifaces,
         char *settings[], int n_settings)
{
    struct vsctl_info info;
    struct vsctl_bridge *bridge;
    struct ovsrec_interface **ifaces;
    struct ovsrec_port *port;
    size_t i;

    get_info(ctx, &info);
    if (may_exist) {
        struct vsctl_port *vsctl_port;

        vsctl_port = find_port(&info, port_name, false);
        if (vsctl_port) {
            struct svec want_names, have_names;

            svec_init(&want_names);
            for (i = 0; i < n_ifaces; i++) {
                svec_add(&want_names, iface_names[i]);
            }
            svec_sort(&want_names);

            svec_init(&have_names);
            for (i = 0; i < vsctl_port->port_cfg->n_interfaces; i++) {
                svec_add(&have_names,
                         vsctl_port->port_cfg->interfaces[i]->name);
            }
            svec_sort(&have_names);

            if (strcmp(vsctl_port->bridge->name, br_name)) {
                char *command = vsctl_context_to_string(ctx);
                vsctl_fatal("\"%s\" but %s is actually attached to bridge %s",
                            command, port_name, vsctl_port->bridge->name);
            }

            if (!svec_equal(&want_names, &have_names)) {
                char *have_names_string = svec_join(&have_names, ", ", "");
                char *command = vsctl_context_to_string(ctx);

                vsctl_fatal("\"%s\" but %s actually has interface(s) %s",
                            command, port_name, have_names_string);
            }

            svec_destroy(&want_names);
            svec_destroy(&have_names);

            return;
        }
    }
    check_conflicts(&info, port_name,
                    xasprintf("cannot create a port named %s", port_name));
    for (i = 0; i < n_ifaces; i++) {
        check_conflicts(&info, iface_names[i],
                        xasprintf("cannot create an interface named %s",
                                  iface_names[i]));
    }
    bridge = find_bridge(&info, br_name, true);

    ifaces = xmalloc(n_ifaces * sizeof *ifaces);
    for (i = 0; i < n_ifaces; i++) {
        ifaces[i] = ovsrec_interface_insert(ctx->txn);
        ovsrec_interface_set_name(ifaces[i], iface_names[i]);
    }

    port = ovsrec_port_insert(ctx->txn);
    ovsrec_port_set_name(port, port_name);
    ovsrec_port_set_interfaces(port, ifaces, n_ifaces);
    ovsrec_port_set_bond_fake_iface(port, fake_iface);
    free(ifaces);

    if (bridge->parent) {
        int64_t tag = bridge->vlan;
        ovsrec_port_set_tag(port, &tag, 1);
    }

    for (i = 0; i < n_settings; i++) {
        set_column(get_table("Port"), &port->header_, settings[i],
                   ctx->symtab);
    }

    bridge_insert_port((bridge->parent ? bridge->parent->br_cfg
                        : bridge->br_cfg), port);

    free_info(&info);
}

static void
cmd_add_port(struct vsctl_context *ctx)
{
    bool may_exist = shash_find(&ctx->options, "--may-exist") != NULL;

    add_port(ctx, ctx->argv[1], ctx->argv[2], may_exist, false,
             &ctx->argv[2], 1, &ctx->argv[3], ctx->argc - 3);
}

static void
cmd_add_bond(struct vsctl_context *ctx)
{
    bool may_exist = shash_find(&ctx->options, "--may-exist") != NULL;
    bool fake_iface = shash_find(&ctx->options, "--fake-iface");
    int n_ifaces;
    int i;

    n_ifaces = ctx->argc - 3;
    for (i = 3; i < ctx->argc; i++) {
        if (strchr(ctx->argv[i], '=')) {
            n_ifaces = i - 3;
            break;
        }
    }
    if (n_ifaces < 2) {
        vsctl_fatal("add-bond requires at least 2 interfaces, but only "
                    "%d were specified", n_ifaces);
    }

    add_port(ctx, ctx->argv[1], ctx->argv[2], may_exist, fake_iface,
             &ctx->argv[3], n_ifaces,
             &ctx->argv[n_ifaces + 3], ctx->argc - 3 - n_ifaces);
}

static void
cmd_del_port(struct vsctl_context *ctx)
{
    bool must_exist = !shash_find(&ctx->options, "--if-exists");
    bool with_iface = shash_find(&ctx->options, "--with-iface") != NULL;
    struct vsctl_port *port;
    struct vsctl_info info;

    get_info(ctx, &info);
    if (!with_iface) {
        port = find_port(&info, ctx->argv[ctx->argc - 1], must_exist);
    } else {
        const char *target = ctx->argv[ctx->argc - 1];
        struct vsctl_iface *iface;

        port = find_port(&info, target, false);
        if (!port) {
            iface = find_iface(&info, target, false);
            if (iface) {
                port = iface->port;
            }
        }
        if (must_exist && !port) {
            vsctl_fatal("no port or interface named %s", target);
        }
    }

    if (port) {
        if (ctx->argc == 3) {
            struct vsctl_bridge *bridge;

            bridge = find_bridge(&info, ctx->argv[1], true);
            if (port->bridge != bridge) {
                if (port->bridge->parent == bridge) {
                    vsctl_fatal("bridge %s does not have a port %s (although "
                                "its parent bridge %s does)",
                                ctx->argv[1], ctx->argv[2],
                                bridge->parent->name);
                } else {
                    vsctl_fatal("bridge %s does not have a port %s",
                                ctx->argv[1], ctx->argv[2]);
                }
            }
        }

        del_port(&info, port);
    }

    free_info(&info);
}

static void
cmd_port_to_br(struct vsctl_context *ctx)
{
    struct vsctl_port *port;
    struct vsctl_info info;

    get_info(ctx, &info);
    port = find_port(&info, ctx->argv[1], true);
    ds_put_format(&ctx->output, "%s\n", port->bridge->name);
    free_info(&info);
}

static void
cmd_br_to_vlan(struct vsctl_context *ctx)
{
    struct vsctl_bridge *bridge;
    struct vsctl_info info;

    get_info(ctx, &info);
    bridge = find_bridge(&info, ctx->argv[1], true);
    ds_put_format(&ctx->output, "%d\n", bridge->vlan);
    free_info(&info);
}

static void
cmd_br_to_parent(struct vsctl_context *ctx)
{
    struct vsctl_bridge *bridge;
    struct vsctl_info info;

    get_info(ctx, &info);
    bridge = find_bridge(&info, ctx->argv[1], true);
    if (bridge->parent) {
        bridge = bridge->parent;
    }
    ds_put_format(&ctx->output, "%s\n", bridge->name);
    free_info(&info);
}

static void
cmd_list_ifaces(struct vsctl_context *ctx)
{
    struct vsctl_bridge *br;
    struct shash_node *node;
    struct vsctl_info info;
    struct svec ifaces;

    get_info(ctx, &info);
    br = find_bridge(&info, ctx->argv[1], true);
    verify_ports(ctx);

    svec_init(&ifaces);
    SHASH_FOR_EACH (node, &info.ifaces) {
        struct vsctl_iface *iface = node->data;

        if (strcmp(iface->iface_cfg->name, br->name)
            && br == iface->port->bridge) {
            svec_add(&ifaces, iface->iface_cfg->name);
        }
    }
    output_sorted(&ifaces, &ctx->output);
    svec_destroy(&ifaces);

    free_info(&info);
}

static void
cmd_iface_to_br(struct vsctl_context *ctx)
{
    struct vsctl_iface *iface;
    struct vsctl_info info;

    get_info(ctx, &info);
    iface = find_iface(&info, ctx->argv[1], true);
    ds_put_format(&ctx->output, "%s\n", iface->port->bridge->name);
    free_info(&info);
}

static void
verify_controllers(struct ovsrec_bridge *bridge)
{
    if (bridge) {
        size_t i;

        ovsrec_bridge_verify_controller(bridge);
        for (i = 0; i < bridge->n_controller; i++) {
            ovsrec_controller_verify_target(bridge->controller[i]);
        }
    }
}

static void
pre_controller(struct vsctl_context *ctx)
{
    pre_get_info(ctx);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_controller_col_target);
}

static void
cmd_get_controller(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *br;
    struct svec targets;
    size_t i;

    get_info(ctx, &info);
    br = find_bridge(&info, ctx->argv[1], true);
    verify_controllers(br->br_cfg);

    /* Print the targets in sorted order for reproducibility. */
    svec_init(&targets);
    for (i = 0; i < br->n_ctrl; i++) {
        svec_add(&targets, br->ctrl[i]->target);
    }

    svec_sort(&targets);
    for (i = 0; i < targets.n; i++) {
        ds_put_format(&ctx->output, "%s\n", targets.names[i]);
    }
    svec_destroy(&targets);

    free_info(&info);
}

static void
delete_controllers(struct ovsrec_controller **controllers,
                   size_t n_controllers)
{
    size_t i;

    for (i = 0; i < n_controllers; i++) {
        ovsrec_controller_delete(controllers[i]);
    }
}

static void
cmd_del_controller(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *br;

    get_info(ctx, &info);

    br = find_real_bridge(&info, ctx->argv[1], true);
    verify_controllers(br->br_cfg);

    if (br->ctrl) {
        delete_controllers(br->ctrl, br->n_ctrl);
        ovsrec_bridge_set_controller(br->br_cfg, NULL, 0);
    }

    free_info(&info);
}

static struct ovsrec_controller **
insert_controllers(struct ovsdb_idl_txn *txn, char *targets[], size_t n)
{
    struct ovsrec_controller **controllers;
    size_t i;

    controllers = xmalloc(n * sizeof *controllers);
    for (i = 0; i < n; i++) {
        if (vconn_verify_name(targets[i]) && pvconn_verify_name(targets[i])) {
            VLOG_WARN("target type \"%s\" is possibly erroneous", targets[i]);
        }
        controllers[i] = ovsrec_controller_insert(txn);
        ovsrec_controller_set_target(controllers[i], targets[i]);
    }

    return controllers;
}

static void
cmd_set_controller(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *br;
    struct ovsrec_controller **controllers;
    size_t n;

    get_info(ctx, &info);
    br = find_real_bridge(&info, ctx->argv[1], true);
    verify_controllers(br->br_cfg);

    delete_controllers(br->ctrl, br->n_ctrl);

    n = ctx->argc - 2;
    controllers = insert_controllers(ctx->txn, &ctx->argv[2], n);
    ovsrec_bridge_set_controller(br->br_cfg, controllers, n);
    free(controllers);

    free_info(&info);
}

static void
cmd_get_fail_mode(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *br;

    get_info(ctx, &info);
    br = find_bridge(&info, ctx->argv[1], true);

    if (br->br_cfg) {
        ovsrec_bridge_verify_fail_mode(br->br_cfg);
    }
    if (br->fail_mode && strlen(br->fail_mode)) {
        ds_put_format(&ctx->output, "%s\n", br->fail_mode);
    }

    free_info(&info);
}

static void
cmd_del_fail_mode(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *br;

    get_info(ctx, &info);
    br = find_real_bridge(&info, ctx->argv[1], true);

    ovsrec_bridge_set_fail_mode(br->br_cfg, NULL);

    free_info(&info);
}

static void
cmd_set_fail_mode(struct vsctl_context *ctx)
{
    struct vsctl_info info;
    struct vsctl_bridge *br;
    const char *fail_mode = ctx->argv[2];

    get_info(ctx, &info);
    br = find_real_bridge(&info, ctx->argv[1], true);

    if (strcmp(fail_mode, "standalone") && strcmp(fail_mode, "secure")) {
        vsctl_fatal("fail-mode must be \"standalone\" or \"secure\"");
    }

    ovsrec_bridge_set_fail_mode(br->br_cfg, fail_mode);

    free_info(&info);
}

static void
verify_managers(const struct ovsrec_open_vswitch *ovs)
{
    size_t i;

    ovsrec_open_vswitch_verify_manager_options(ovs);

    for (i = 0; i < ovs->n_manager_options; ++i) {
        const struct ovsrec_manager *mgr = ovs->manager_options[i];

        ovsrec_manager_verify_target(mgr);
    }
}

static void
pre_manager(struct vsctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_manager_options);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_manager_col_target);
}

static void
cmd_get_manager(struct vsctl_context *ctx)
{
    const struct ovsrec_open_vswitch *ovs = ctx->ovs;
    struct svec targets;
    size_t i;

    verify_managers(ovs);

    /* Print the targets in sorted order for reproducibility. */
    svec_init(&targets);

    for (i = 0; i < ovs->n_manager_options; i++) {
        svec_add(&targets, ovs->manager_options[i]->target);
    }

    svec_sort_unique(&targets);
    for (i = 0; i < targets.n; i++) {
        ds_put_format(&ctx->output, "%s\n", targets.names[i]);
    }
    svec_destroy(&targets);
}

static void
delete_managers(const struct vsctl_context *ctx)
{
    const struct ovsrec_open_vswitch *ovs = ctx->ovs;
    size_t i;

    /* Delete Manager rows pointed to by 'manager_options' column. */
    for (i = 0; i < ovs->n_manager_options; i++) {
        ovsrec_manager_delete(ovs->manager_options[i]);
    }

    /* Delete 'Manager' row refs in 'manager_options' column. */
    ovsrec_open_vswitch_set_manager_options(ovs, NULL, 0);
}

static void
cmd_del_manager(struct vsctl_context *ctx)
{
    const struct ovsrec_open_vswitch *ovs = ctx->ovs;

    verify_managers(ovs);
    delete_managers(ctx);
}

static void
insert_managers(struct vsctl_context *ctx, char *targets[], size_t n)
{
    struct ovsrec_manager **managers;
    size_t i;

    /* Insert each manager in a new row in Manager table. */
    managers = xmalloc(n * sizeof *managers);
    for (i = 0; i < n; i++) {
        if (stream_verify_name(targets[i]) && pstream_verify_name(targets[i])) {
            VLOG_WARN("target type \"%s\" is possibly erroneous", targets[i]);
        }
        managers[i] = ovsrec_manager_insert(ctx->txn);
        ovsrec_manager_set_target(managers[i], targets[i]);
    }

    /* Store uuids of new Manager rows in 'manager_options' column. */
    ovsrec_open_vswitch_set_manager_options(ctx->ovs, managers, n);
    free(managers);
}

static void
cmd_set_manager(struct vsctl_context *ctx)
{
    const size_t n = ctx->argc - 1;

    verify_managers(ctx->ovs);
    delete_managers(ctx);
    insert_managers(ctx, &ctx->argv[1], n);
}

static void
pre_cmd_get_ssl(struct vsctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_ssl);

    ovsdb_idl_add_column(ctx->idl, &ovsrec_ssl_col_private_key);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_ssl_col_certificate);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_ssl_col_ca_cert);
    ovsdb_idl_add_column(ctx->idl, &ovsrec_ssl_col_bootstrap_ca_cert);
}

static void
cmd_get_ssl(struct vsctl_context *ctx)
{
    struct ovsrec_ssl *ssl = ctx->ovs->ssl;

    ovsrec_open_vswitch_verify_ssl(ctx->ovs);
    if (ssl) {
        ovsrec_ssl_verify_private_key(ssl);
        ovsrec_ssl_verify_certificate(ssl);
        ovsrec_ssl_verify_ca_cert(ssl);
        ovsrec_ssl_verify_bootstrap_ca_cert(ssl);

        ds_put_format(&ctx->output, "Private key: %s\n", ssl->private_key);
        ds_put_format(&ctx->output, "Certificate: %s\n", ssl->certificate);
        ds_put_format(&ctx->output, "CA Certificate: %s\n", ssl->ca_cert);
        ds_put_format(&ctx->output, "Bootstrap: %s\n",
                ssl->bootstrap_ca_cert ? "true" : "false");
    }
}

static void
pre_cmd_del_ssl(struct vsctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_ssl);
}

static void
cmd_del_ssl(struct vsctl_context *ctx)
{
    struct ovsrec_ssl *ssl = ctx->ovs->ssl;

    if (ssl) {
        ovsrec_open_vswitch_verify_ssl(ctx->ovs);
        ovsrec_ssl_delete(ssl);
        ovsrec_open_vswitch_set_ssl(ctx->ovs, NULL);
    }
}

static void
pre_cmd_set_ssl(struct vsctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &ovsrec_open_vswitch_col_ssl);
}

static void
cmd_set_ssl(struct vsctl_context *ctx)
{
    bool bootstrap = shash_find(&ctx->options, "--bootstrap");
    struct ovsrec_ssl *ssl = ctx->ovs->ssl;

    ovsrec_open_vswitch_verify_ssl(ctx->ovs);
    if (ssl) {
        ovsrec_ssl_delete(ssl);
    }
    ssl = ovsrec_ssl_insert(ctx->txn);

    ovsrec_ssl_set_private_key(ssl, ctx->argv[1]);
    ovsrec_ssl_set_certificate(ssl, ctx->argv[2]);
    ovsrec_ssl_set_ca_cert(ssl, ctx->argv[3]);

    ovsrec_ssl_set_bootstrap_ca_cert(ssl, bootstrap);

    ovsrec_open_vswitch_set_ssl(ctx->ovs, ssl);
}

/* Parameter commands. */

struct vsctl_row_id {
    const struct ovsdb_idl_table_class *table;
    const struct ovsdb_idl_column *name_column;
    const struct ovsdb_idl_column *uuid_column;
};

struct vsctl_table_class {
    struct ovsdb_idl_table_class *class;
    struct vsctl_row_id row_ids[2];
};

static const struct vsctl_table_class tables[] = {
    {&ovsrec_table_bridge,
     {{&ovsrec_table_bridge, &ovsrec_bridge_col_name, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_controller,
     {{&ovsrec_table_bridge,
       &ovsrec_bridge_col_name,
       &ovsrec_bridge_col_controller}}},

    {&ovsrec_table_interface,
     {{&ovsrec_table_interface, &ovsrec_interface_col_name, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_mirror,
     {{&ovsrec_table_mirror, &ovsrec_mirror_col_name, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_manager,
     {{&ovsrec_table_manager, &ovsrec_manager_col_target, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_netflow,
     {{&ovsrec_table_bridge,
       &ovsrec_bridge_col_name,
       &ovsrec_bridge_col_netflow},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_open_vswitch,
     {{&ovsrec_table_open_vswitch, NULL, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_port,
     {{&ovsrec_table_port, &ovsrec_port_col_name, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_qos,
     {{&ovsrec_table_port, &ovsrec_port_col_name, &ovsrec_port_col_qos},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_queue,
     {{NULL, NULL, NULL},
      {NULL, NULL, NULL}}},

    {&ovsrec_table_ssl,
     {{&ovsrec_table_open_vswitch, NULL, &ovsrec_open_vswitch_col_ssl}}},

    {&ovsrec_table_sflow,
     {{&ovsrec_table_bridge,
       &ovsrec_bridge_col_name,
       &ovsrec_bridge_col_sflow},
      {NULL, NULL, NULL}}},

    {NULL, {{NULL, NULL, NULL}, {NULL, NULL, NULL}}}
};

static void
die_if_error(char *error)
{
    if (error) {
        vsctl_fatal("%s", error);
    }
}

static int
to_lower_and_underscores(unsigned c)
{
    return c == '-' ? '_' : tolower(c);
}

static unsigned int
score_partial_match(const char *name, const char *s)
{
    int score;

    if (!strcmp(name, s)) {
        return UINT_MAX;
    }
    for (score = 0; ; score++, name++, s++) {
        if (to_lower_and_underscores(*name) != to_lower_and_underscores(*s)) {
            break;
        } else if (*name == '\0') {
            return UINT_MAX - 1;
        }
    }
    return *s == '\0' ? score : 0;
}

static const struct vsctl_table_class *
get_table(const char *table_name)
{
    const struct vsctl_table_class *table;
    const struct vsctl_table_class *best_match = NULL;
    unsigned int best_score = 0;

    for (table = tables; table->class; table++) {
        unsigned int score = score_partial_match(table->class->name,
                                                 table_name);
        if (score > best_score) {
            best_match = table;
            best_score = score;
        } else if (score == best_score) {
            best_match = NULL;
        }
    }
    if (best_match) {
        return best_match;
    } else if (best_score) {
        vsctl_fatal("multiple table names match \"%s\"", table_name);
    } else {
        vsctl_fatal("unknown table \"%s\"", table_name);
    }
}

static const struct vsctl_table_class *
pre_get_table(struct vsctl_context *ctx, const char *table_name)
{
    const struct vsctl_table_class *table_class;
    int i;

    table_class = get_table(table_name);
    ovsdb_idl_add_table(ctx->idl, table_class->class);

    for (i = 0; i < ARRAY_SIZE(table_class->row_ids); i++) {
        const struct vsctl_row_id *id = &table_class->row_ids[i];
        if (id->table) {
            ovsdb_idl_add_table(ctx->idl, id->table);
        }
        if (id->name_column) {
            ovsdb_idl_add_column(ctx->idl, id->name_column);
        }
        if (id->uuid_column) {
            ovsdb_idl_add_column(ctx->idl, id->uuid_column);
        }
    }

    return table_class;
}

static const struct ovsdb_idl_row *
get_row_by_id(struct vsctl_context *ctx, const struct vsctl_table_class *table,
              const struct vsctl_row_id *id, const char *record_id)
{
    const struct ovsdb_idl_row *referrer, *final;

    if (!id->table) {
        return NULL;
    }

    if (!id->name_column) {
        if (strcmp(record_id, ".")) {
            return NULL;
        }
        referrer = ovsdb_idl_first_row(ctx->idl, id->table);
        if (!referrer || ovsdb_idl_next_row(referrer)) {
            return NULL;
        }
    } else {
        const struct ovsdb_idl_row *row;

        referrer = NULL;
        for (row = ovsdb_idl_first_row(ctx->idl, id->table);
             row != NULL;
             row = ovsdb_idl_next_row(row))
        {
            const struct ovsdb_datum *name;

            name = ovsdb_idl_get(row, id->name_column,
                                 OVSDB_TYPE_STRING, OVSDB_TYPE_VOID);
            if (name->n == 1 && !strcmp(name->keys[0].string, record_id)) {
                if (referrer) {
                    vsctl_fatal("multiple rows in %s match \"%s\"",
                                table->class->name, record_id);
                }
                referrer = row;
            }
        }
    }
    if (!referrer) {
        return NULL;
    }

    final = NULL;
    if (id->uuid_column) {
        const struct ovsdb_datum *uuid;

        ovsdb_idl_txn_verify(referrer, id->uuid_column);
        uuid = ovsdb_idl_get(referrer, id->uuid_column,
                             OVSDB_TYPE_UUID, OVSDB_TYPE_VOID);
        if (uuid->n == 1) {
            final = ovsdb_idl_get_row_for_uuid(ctx->idl, table->class,
                                               &uuid->keys[0].uuid);
        }
    } else {
        final = referrer;
    }

    return final;
}

static const struct ovsdb_idl_row *
get_row (struct vsctl_context *ctx,
         const struct vsctl_table_class *table, const char *record_id)
{
    const struct ovsdb_idl_row *row;
    struct uuid uuid;

    if (uuid_from_string(&uuid, record_id)) {
        row = ovsdb_idl_get_row_for_uuid(ctx->idl, table->class, &uuid);
    } else {
        int i;

        for (i = 0; i < ARRAY_SIZE(table->row_ids); i++) {
            row = get_row_by_id(ctx, table, &table->row_ids[i], record_id);
            if (row) {
                break;
            }
        }
    }
    return row;
}

static const struct ovsdb_idl_row *
must_get_row(struct vsctl_context *ctx,
             const struct vsctl_table_class *table, const char *record_id)
{
    const struct ovsdb_idl_row *row = get_row(ctx, table, record_id);
    if (!row) {
        vsctl_fatal("no row \"%s\" in table %s",
                    record_id, table->class->name);
    }
    return row;
}

static char *
get_column(const struct vsctl_table_class *table, const char *column_name,
           const struct ovsdb_idl_column **columnp)
{
    const struct ovsdb_idl_column *best_match = NULL;
    unsigned int best_score = 0;
    size_t i;

    for (i = 0; i < table->class->n_columns; i++) {
        const struct ovsdb_idl_column *column = &table->class->columns[i];
        unsigned int score = score_partial_match(column->name, column_name);
        if (score > best_score) {
            best_match = column;
            best_score = score;
        } else if (score == best_score) {
            best_match = NULL;
        }
    }

    *columnp = best_match;
    if (best_match) {
        return NULL;
    } else if (best_score) {
        return xasprintf("%s contains more than one column whose name "
                         "matches \"%s\"", table->class->name, column_name);
    } else {
        return xasprintf("%s does not contain a column whose name matches "
                         "\"%s\"", table->class->name, column_name);
    }
}

static struct ovsdb_symbol *
create_symbol(struct ovsdb_symbol_table *symtab, const char *id, bool *newp)
{
    struct ovsdb_symbol *symbol;

    if (id[0] != '@') {
        vsctl_fatal("row id \"%s\" does not begin with \"@\"", id);
    }

    if (newp) {
        *newp = ovsdb_symbol_table_get(symtab, id) == NULL;
    }

    symbol = ovsdb_symbol_table_insert(symtab, id);
    if (symbol->created) {
        vsctl_fatal("row id \"%s\" may only be specified on one --id option",
                    id);
    }
    symbol->created = true;
    return symbol;
}

static void
pre_get_column(struct vsctl_context *ctx,
               const struct vsctl_table_class *table, const char *column_name,
               const struct ovsdb_idl_column **columnp)
{
    die_if_error(get_column(table, column_name, columnp));
    ovsdb_idl_add_column(ctx->idl, *columnp);
}

static char *
missing_operator_error(const char *arg, const char **allowed_operators,
                       size_t n_allowed)
{
    struct ds s;

    ds_init(&s);
    ds_put_format(&s, "%s: argument does not end in ", arg);
    ds_put_format(&s, "\"%s\"", allowed_operators[0]);
    if (n_allowed == 2) {
        ds_put_format(&s, " or \"%s\"", allowed_operators[1]);
    } else if (n_allowed > 2) {
        size_t i;

        for (i = 1; i < n_allowed - 1; i++) {
            ds_put_format(&s, ", \"%s\"", allowed_operators[i]);
        }
        ds_put_format(&s, ", or \"%s\"", allowed_operators[i]);
    }
    ds_put_format(&s, " followed by a value.");

    return ds_steal_cstr(&s);
}

/* Breaks 'arg' apart into a number of fields in the following order:
 *
 *      - The name of a column in 'table', stored into '*columnp'.  The column
 *        name may be abbreviated.
 *
 *      - Optionally ':' followed by a key string.  The key is stored as a
 *        malloc()'d string into '*keyp', or NULL if no key is present in
 *        'arg'.
 *
 *      - If 'valuep' is nonnull, an operator followed by a value string.  The
 *        allowed operators are the 'n_allowed' string in 'allowed_operators',
 *        or just "=" if 'n_allowed' is 0.  If 'operatorp' is nonnull, then the
 *        operator is stored into '*operatorp' (one of the pointers from
 *        'allowed_operators' is stored; nothing is malloc()'d).  The value is
 *        stored as a malloc()'d string into '*valuep', or NULL if no value is
 *        present in 'arg'.
 *
 * On success, returns NULL.  On failure, returned a malloc()'d string error
 * message and stores NULL into all of the nonnull output arguments. */
static char * WARN_UNUSED_RESULT
parse_column_key_value(const char *arg,
                       const struct vsctl_table_class *table,
                       const struct ovsdb_idl_column **columnp, char **keyp,
                       const char **operatorp,
                       const char **allowed_operators, size_t n_allowed,
                       char **valuep)
{
    const char *p = arg;
    char *column_name;
    char *error;

    assert(!(operatorp && !valuep));
    *keyp = NULL;
    if (valuep) {
        *valuep = NULL;
    }

    /* Parse column name. */
    error = ovsdb_token_parse(&p, &column_name);
    if (error) {
        goto error;
    }
    if (column_name[0] == '\0') {
        free(column_name);
        error = xasprintf("%s: missing column name", arg);
        goto error;
    }
    error = get_column(table, column_name, columnp);
    free(column_name);
    if (error) {
        goto error;
    }

    /* Parse key string. */
    if (*p == ':') {
        p++;
        error = ovsdb_token_parse(&p, keyp);
        if (error) {
            goto error;
        }
    }

    /* Parse value string. */
    if (valuep) {
        const char *best;
        size_t best_len;
        size_t i;

        if (!allowed_operators) {
            static const char *equals = "=";
            allowed_operators = &equals;
            n_allowed = 1;
        }

        best = NULL;
        best_len = 0;
        for (i = 0; i < n_allowed; i++) {
            const char *op = allowed_operators[i];
            size_t op_len = strlen(op);

            if (op_len > best_len && !strncmp(op, p, op_len) && p[op_len]) {
                best_len = op_len;
                best = op;
            }
        }
        if (!best) {
            error = missing_operator_error(arg, allowed_operators, n_allowed);
            goto error;
        }

        if (operatorp) {
            *operatorp = best;
        }
        *valuep = xstrdup(p + best_len);
    } else {
        if (*p != '\0') {
            error = xasprintf("%s: trailing garbage \"%s\" in argument",
                              arg, p);
            goto error;
        }
    }
    return NULL;

error:
    *columnp = NULL;
    free(*keyp);
    *keyp = NULL;
    if (valuep) {
        free(*valuep);
        *valuep = NULL;
        if (operatorp) {
            *operatorp = NULL;
        }
    }
    return error;
}

static void
pre_parse_column_key_value(struct vsctl_context *ctx,
                           const char *arg,
                           const struct vsctl_table_class *table)
{
    const struct ovsdb_idl_column *column;
    const char *p;
    char *column_name;

    p = arg;
    die_if_error(ovsdb_token_parse(&p, &column_name));
    if (column_name[0] == '\0') {
        vsctl_fatal("%s: missing column name", arg);
    }

    pre_get_column(ctx, table, column_name, &column);
    free(column_name);
}

static void
pre_cmd_get(struct vsctl_context *ctx)
{
    const char *id = shash_find_data(&ctx->options, "--id");
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    int i;

    /* Using "get" without --id or a column name could possibly make sense.
     * Maybe, for example, a ovs-vsctl run wants to assert that a row exists.
     * But it is unlikely that an interactive user would want to do that, so
     * issue a warning if we're running on a terminal. */
    if (!id && ctx->argc <= 3 && isatty(STDOUT_FILENO)) {
        VLOG_WARN("\"get\" command without row arguments or \"--id\" is "
                  "possibly erroneous");
    }

    table = pre_get_table(ctx, table_name);
    for (i = 3; i < ctx->argc; i++) {
        if (!strcasecmp(ctx->argv[i], "_uuid")
            || !strcasecmp(ctx->argv[i], "-uuid")) {
            continue;
        }

        pre_parse_column_key_value(ctx, ctx->argv[i], table);
    }
}

static void
cmd_get(struct vsctl_context *ctx)
{
    const char *id = shash_find_data(&ctx->options, "--id");
    bool if_exists = shash_find(&ctx->options, "--if-exists");
    const char *table_name = ctx->argv[1];
    const char *record_id = ctx->argv[2];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_row *row;
    struct ds *out = &ctx->output;
    int i;

    table = get_table(table_name);
    row = must_get_row(ctx, table, record_id);
    if (id) {
        struct ovsdb_symbol *symbol;
        bool new;

        symbol = create_symbol(ctx->symtab, id, &new);
        if (!new) {
            vsctl_fatal("row id \"%s\" specified on \"get\" command was used "
                        "before it was defined", id);
        }
        symbol->uuid = row->uuid;

        /* This symbol refers to a row that already exists, so disable warnings
         * about it being unreferenced. */
        symbol->strong_ref = true;
    }
    for (i = 3; i < ctx->argc; i++) {
        const struct ovsdb_idl_column *column;
        const struct ovsdb_datum *datum;
        char *key_string;

        /* Special case for obtaining the UUID of a row.  We can't just do this
         * through parse_column_key_value() below since it returns a "struct
         * ovsdb_idl_column" and the UUID column doesn't have one. */
        if (!strcasecmp(ctx->argv[i], "_uuid")
            || !strcasecmp(ctx->argv[i], "-uuid")) {
            ds_put_format(out, UUID_FMT"\n", UUID_ARGS(&row->uuid));
            continue;
        }

        die_if_error(parse_column_key_value(ctx->argv[i], table,
                                            &column, &key_string,
                                            NULL, NULL, 0, NULL));

        ovsdb_idl_txn_verify(row, column);
        datum = ovsdb_idl_read(row, column);
        if (key_string) {
            union ovsdb_atom key;
            unsigned int idx;

            if (column->type.value.type == OVSDB_TYPE_VOID) {
                vsctl_fatal("cannot specify key to get for non-map column %s",
                            column->name);
            }

            die_if_error(ovsdb_atom_from_string(&key,
                                                &column->type.key,
                                                key_string, ctx->symtab));

            idx = ovsdb_datum_find_key(datum, &key,
                                       column->type.key.type);
            if (idx == UINT_MAX) {
                if (!if_exists) {
                    vsctl_fatal("no key \"%s\" in %s record \"%s\" column %s",
                                key_string, table->class->name, record_id,
                                column->name);
                }
            } else {
                ovsdb_atom_to_string(&datum->values[idx],
                                     column->type.value.type, out);
            }
            ovsdb_atom_destroy(&key, column->type.key.type);
        } else {
            ovsdb_datum_to_string(datum, &column->type, out);
        }
        ds_put_char(out, '\n');

        free(key_string);
    }
}

static void
parse_column_names(const char *column_names,
                   const struct vsctl_table_class *table,
                   const struct ovsdb_idl_column ***columnsp,
                   size_t *n_columnsp)
{
    const struct ovsdb_idl_column **columns;
    size_t n_columns;

    if (!column_names) {
        size_t i;

        n_columns = table->class->n_columns + 1;
        columns = xmalloc(n_columns * sizeof *columns);
        columns[0] = NULL;
        for (i = 0; i < table->class->n_columns; i++) {
            columns[i + 1] = &table->class->columns[i];
        }
    } else {
        char *s = xstrdup(column_names);
        size_t allocated_columns;
        char *save_ptr = NULL;
        char *column_name;

        columns = NULL;
        allocated_columns = n_columns = 0;
        for (column_name = strtok_r(s, ", ", &save_ptr); column_name;
             column_name = strtok_r(NULL, ", ", &save_ptr)) {
            const struct ovsdb_idl_column *column;

            if (!strcasecmp(column_name, "_uuid")) {
                column = NULL;
            } else {
                die_if_error(get_column(table, column_name, &column));
            }
            if (n_columns >= allocated_columns) {
                columns = x2nrealloc(columns, &allocated_columns,
                                     sizeof *columns);
            }
            columns[n_columns++] = column;
        }
        free(s);

        if (!n_columns) {
            vsctl_fatal("must specify at least one column name");
        }
    }
    *columnsp = columns;
    *n_columnsp = n_columns;
}


static void
pre_list_columns(struct vsctl_context *ctx,
                 const struct vsctl_table_class *table,
                 const char *column_names)
{
    const struct ovsdb_idl_column **columns;
    size_t n_columns;
    size_t i;

    parse_column_names(column_names, table, &columns, &n_columns);
    for (i = 0; i < n_columns; i++) {
        if (columns[i]) {
            ovsdb_idl_add_column(ctx->idl, columns[i]);
        }
    }
    free(columns);
}

static void
pre_cmd_list(struct vsctl_context *ctx)
{
    const char *column_names = shash_find_data(&ctx->options, "--columns");
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;

    table = pre_get_table(ctx, table_name);
    pre_list_columns(ctx, table, column_names);
}

static struct table *
list_make_table(const struct ovsdb_idl_column **columns, size_t n_columns)
{
    struct table *out;
    size_t i;

    out = xmalloc(sizeof *out);
    table_init(out);

    for (i = 0; i < n_columns; i++) {
        const struct ovsdb_idl_column *column = columns[i];
        const char *column_name = column ? column->name : "_uuid";

        table_add_column(out, "%s", column_name);
    }

    return out;
}

static void
list_record(const struct ovsdb_idl_row *row,
            const struct ovsdb_idl_column **columns, size_t n_columns,
            struct table *out)
{
    size_t i;

    table_add_row(out);
    for (i = 0; i < n_columns; i++) {
        const struct ovsdb_idl_column *column = columns[i];
        struct cell *cell = table_add_cell(out);

        if (!column) {
            struct ovsdb_datum datum;
            union ovsdb_atom atom;

            atom.uuid = row->uuid;

            datum.keys = &atom;
            datum.values = NULL;
            datum.n = 1;

            cell->json = ovsdb_datum_to_json(&datum, &ovsdb_type_uuid);
            cell->type = &ovsdb_type_uuid;
        } else {
            const struct ovsdb_datum *datum = ovsdb_idl_read(row, column);

            cell->json = ovsdb_datum_to_json(datum, &column->type);
            cell->type = &column->type;
        }
    }
}

static void
cmd_list(struct vsctl_context *ctx)
{
    const char *column_names = shash_find_data(&ctx->options, "--columns");
    const struct ovsdb_idl_column **columns;
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    struct table *out;
    size_t n_columns;
    int i;

    table = get_table(table_name);
    parse_column_names(column_names, table, &columns, &n_columns);
    out = ctx->table = list_make_table(columns, n_columns);
    if (ctx->argc > 2) {
        for (i = 2; i < ctx->argc; i++) {
            list_record(must_get_row(ctx, table, ctx->argv[i]),
                        columns, n_columns, out);
        }
    } else {
        const struct ovsdb_idl_row *row;

        for (row = ovsdb_idl_first_row(ctx->idl, table->class); row != NULL;
             row = ovsdb_idl_next_row(row)) {
            list_record(row, columns, n_columns, out);
        }
    }
    free(columns);
}

static void
pre_cmd_find(struct vsctl_context *ctx)
{
    const char *column_names = shash_find_data(&ctx->options, "--columns");
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    int i;

    table = pre_get_table(ctx, table_name);
    pre_list_columns(ctx, table, column_names);
    for (i = 2; i < ctx->argc; i++) {
        pre_parse_column_key_value(ctx, ctx->argv[i], table);
    }
}

static void
cmd_find(struct vsctl_context *ctx)
{
    const char *column_names = shash_find_data(&ctx->options, "--columns");
    const struct ovsdb_idl_column **columns;
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_row *row;
    struct table *out;
    size_t n_columns;

    table = get_table(table_name);
    parse_column_names(column_names, table, &columns, &n_columns);
    out = ctx->table = list_make_table(columns, n_columns);
    for (row = ovsdb_idl_first_row(ctx->idl, table->class); row;
         row = ovsdb_idl_next_row(row)) {
        int i;

        for (i = 2; i < ctx->argc; i++) {
            if (!is_condition_satisfied(table, row, ctx->argv[i],
                                        ctx->symtab)) {
                goto next_row;
            }
        }
        list_record(row, columns, n_columns, out);

    next_row: ;
    }
    free(columns);
}

static void
pre_cmd_set(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    int i;

    table = pre_get_table(ctx, table_name);
    for (i = 3; i < ctx->argc; i++) {
        pre_parse_column_key_value(ctx, ctx->argv[i], table);
    }
}

static void
set_column(const struct vsctl_table_class *table,
           const struct ovsdb_idl_row *row, const char *arg,
           struct ovsdb_symbol_table *symtab)
{
    const struct ovsdb_idl_column *column;
    char *key_string, *value_string;
    char *error;

    error = parse_column_key_value(arg, table, &column, &key_string,
                                   NULL, NULL, 0, &value_string);
    die_if_error(error);
    if (!value_string) {
        vsctl_fatal("%s: missing value", arg);
    }

    if (key_string) {
        union ovsdb_atom key, value;
        struct ovsdb_datum datum;

        if (column->type.value.type == OVSDB_TYPE_VOID) {
            vsctl_fatal("cannot specify key to set for non-map column %s",
                        column->name);
        }

        die_if_error(ovsdb_atom_from_string(&key, &column->type.key,
                                            key_string, symtab));
        die_if_error(ovsdb_atom_from_string(&value, &column->type.value,
                                            value_string, symtab));

        ovsdb_datum_init_empty(&datum);
        ovsdb_datum_add_unsafe(&datum, &key, &value, &column->type);

        ovsdb_atom_destroy(&key, column->type.key.type);
        ovsdb_atom_destroy(&value, column->type.value.type);

        ovsdb_datum_union(&datum, ovsdb_idl_read(row, column),
                          &column->type, false);
        ovsdb_idl_txn_write(row, column, &datum);
    } else {
        struct ovsdb_datum datum;

        die_if_error(ovsdb_datum_from_string(&datum, &column->type,
                                             value_string, symtab));
        ovsdb_idl_txn_write(row, column, &datum);
    }

    free(key_string);
    free(value_string);
}

static void
cmd_set(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *record_id = ctx->argv[2];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_row *row;
    int i;

    table = get_table(table_name);
    row = must_get_row(ctx, table, record_id);
    for (i = 3; i < ctx->argc; i++) {
        set_column(table, row, ctx->argv[i], ctx->symtab);
    }
}

static void
pre_cmd_add(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *column_name = ctx->argv[3];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_column *column;

    table = pre_get_table(ctx, table_name);
    pre_get_column(ctx, table, column_name, &column);
}

static void
cmd_add(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *record_id = ctx->argv[2];
    const char *column_name = ctx->argv[3];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_column *column;
    const struct ovsdb_idl_row *row;
    const struct ovsdb_type *type;
    struct ovsdb_datum old;
    int i;

    table = get_table(table_name);
    row = must_get_row(ctx, table, record_id);
    die_if_error(get_column(table, column_name, &column));

    type = &column->type;
    ovsdb_datum_clone(&old, ovsdb_idl_read(row, column), &column->type);
    for (i = 4; i < ctx->argc; i++) {
        struct ovsdb_type add_type;
        struct ovsdb_datum add;

        add_type = *type;
        add_type.n_min = 1;
        add_type.n_max = UINT_MAX;
        die_if_error(ovsdb_datum_from_string(&add, &add_type, ctx->argv[i],
                                             ctx->symtab));
        ovsdb_datum_union(&old, &add, type, false);
        ovsdb_datum_destroy(&add, type);
    }
    if (old.n > type->n_max) {
        vsctl_fatal("\"add\" operation would put %u %s in column %s of "
                    "table %s but the maximum number is %u",
                    old.n,
                    type->value.type == OVSDB_TYPE_VOID ? "values" : "pairs",
                    column->name, table->class->name, type->n_max);
    }
    ovsdb_idl_txn_verify(row, column);
    ovsdb_idl_txn_write(row, column, &old);
}

static void
pre_cmd_remove(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *column_name = ctx->argv[3];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_column *column;

    table = pre_get_table(ctx, table_name);
    pre_get_column(ctx, table, column_name, &column);
}

static void
cmd_remove(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *record_id = ctx->argv[2];
    const char *column_name = ctx->argv[3];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_column *column;
    const struct ovsdb_idl_row *row;
    const struct ovsdb_type *type;
    struct ovsdb_datum old;
    int i;

    table = get_table(table_name);
    row = must_get_row(ctx, table, record_id);
    die_if_error(get_column(table, column_name, &column));

    type = &column->type;
    ovsdb_datum_clone(&old, ovsdb_idl_read(row, column), &column->type);
    for (i = 4; i < ctx->argc; i++) {
        struct ovsdb_type rm_type;
        struct ovsdb_datum rm;
        char *error;

        rm_type = *type;
        rm_type.n_min = 1;
        rm_type.n_max = UINT_MAX;
        error = ovsdb_datum_from_string(&rm, &rm_type,
                                        ctx->argv[i], ctx->symtab);
        if (error && ovsdb_type_is_map(&rm_type)) {
            free(error);
            rm_type.value.type = OVSDB_TYPE_VOID;
            die_if_error(ovsdb_datum_from_string(&rm, &rm_type,
                                                 ctx->argv[i], ctx->symtab));
        }
        ovsdb_datum_subtract(&old, type, &rm, &rm_type);
        ovsdb_datum_destroy(&rm, &rm_type);
    }
    if (old.n < type->n_min) {
        vsctl_fatal("\"remove\" operation would put %u %s in column %s of "
                    "table %s but the minimum number is %u",
                    old.n,
                    type->value.type == OVSDB_TYPE_VOID ? "values" : "pairs",
                    column->name, table->class->name, type->n_min);
    }
    ovsdb_idl_txn_verify(row, column);
    ovsdb_idl_txn_write(row, column, &old);
}

static void
pre_cmd_clear(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    int i;

    table = pre_get_table(ctx, table_name);
    for (i = 3; i < ctx->argc; i++) {
        const struct ovsdb_idl_column *column;

        pre_get_column(ctx, table, ctx->argv[i], &column);
    }
}

static void
cmd_clear(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *record_id = ctx->argv[2];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_row *row;
    int i;

    table = get_table(table_name);
    row = must_get_row(ctx, table, record_id);
    for (i = 3; i < ctx->argc; i++) {
        const struct ovsdb_idl_column *column;
        const struct ovsdb_type *type;
        struct ovsdb_datum datum;

        die_if_error(get_column(table, ctx->argv[i], &column));

        type = &column->type;
        if (type->n_min > 0) {
            vsctl_fatal("\"clear\" operation cannot be applied to column %s "
                        "of table %s, which is not allowed to be empty",
                        column->name, table->class->name);
        }

        ovsdb_datum_init_empty(&datum);
        ovsdb_idl_txn_write(row, column, &datum);
    }
}

static void
pre_create(struct vsctl_context *ctx)
{
    const char *id = shash_find_data(&ctx->options, "--id");
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;

    table = get_table(table_name);
    if (!id && !table->class->is_root) {
        VLOG_WARN("applying \"create\" command to table %s without --id "
                  "option will have no effect", table->class->name);
    }
}

static void
cmd_create(struct vsctl_context *ctx)
{
    const char *id = shash_find_data(&ctx->options, "--id");
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table = get_table(table_name);
    const struct ovsdb_idl_row *row;
    const struct uuid *uuid;
    int i;

    if (id) {
        struct ovsdb_symbol *symbol = create_symbol(ctx->symtab, id, NULL);
        if (table->class->is_root) {
            /* This table is in the root set, meaning that rows created in it
             * won't disappear even if they are unreferenced, so disable
             * warnings about that by pretending that there is a reference. */
            symbol->strong_ref = true;
        }
        uuid = &symbol->uuid;
    } else {
        uuid = NULL;
    }

    row = ovsdb_idl_txn_insert(ctx->txn, table->class, uuid);
    for (i = 2; i < ctx->argc; i++) {
        set_column(table, row, ctx->argv[i], ctx->symtab);
    }
    ds_put_format(&ctx->output, UUID_FMT, UUID_ARGS(&row->uuid));
}

/* This function may be used as the 'postprocess' function for commands that
 * insert new rows into the database.  It expects that the command's 'run'
 * function prints the UUID reported by ovsdb_idl_txn_insert() as the command's
 * sole output.  It replaces that output by the row's permanent UUID assigned
 * by the database server and appends a new-line.
 *
 * Currently we use this only for "create", because the higher-level commands
 * are supposed to be independent of the actual structure of the vswitch
 * configuration. */
static void
post_create(struct vsctl_context *ctx)
{
    const struct uuid *real;
    struct uuid dummy;

    if (!uuid_from_string(&dummy, ds_cstr(&ctx->output))) {
        NOT_REACHED();
    }
    real = ovsdb_idl_txn_get_insert_uuid(ctx->txn, &dummy);
    if (real) {
        ds_clear(&ctx->output);
        ds_put_format(&ctx->output, UUID_FMT, UUID_ARGS(real));
    }
    ds_put_char(&ctx->output, '\n');
}

static void
pre_cmd_destroy(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];

    pre_get_table(ctx, table_name);
}

static void
cmd_destroy(struct vsctl_context *ctx)
{
    bool must_exist = !shash_find(&ctx->options, "--if-exists");
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    int i;

    table = get_table(table_name);
    for (i = 2; i < ctx->argc; i++) {
        const struct ovsdb_idl_row *row;

        row = (must_exist ? must_get_row : get_row)(ctx, table, ctx->argv[i]);
        if (row) {
            ovsdb_idl_txn_delete(row);
        }
    }
}

static bool
is_condition_satisfied(const struct vsctl_table_class *table,
                       const struct ovsdb_idl_row *row, const char *arg,
                       struct ovsdb_symbol_table *symtab)
{
    static const char *operators[] = {
        "=", "!=", "<", ">", "<=", ">="
    };

    const struct ovsdb_idl_column *column;
    const struct ovsdb_datum *have_datum;
    char *key_string, *value_string;
    const char *operator;
    unsigned int idx;
    char *error;
    int cmp = 0;

    error = parse_column_key_value(arg, table, &column, &key_string,
                                   &operator, operators, ARRAY_SIZE(operators),
                                   &value_string);
    die_if_error(error);
    if (!value_string) {
        vsctl_fatal("%s: missing value", arg);
    }

    have_datum = ovsdb_idl_read(row, column);
    if (key_string) {
        union ovsdb_atom want_key, want_value;

        if (column->type.value.type == OVSDB_TYPE_VOID) {
            vsctl_fatal("cannot specify key to check for non-map column %s",
                        column->name);
        }

        die_if_error(ovsdb_atom_from_string(&want_key, &column->type.key,
                                            key_string, symtab));
        die_if_error(ovsdb_atom_from_string(&want_value, &column->type.value,
                                            value_string, symtab));

        idx = ovsdb_datum_find_key(have_datum,
                                   &want_key, column->type.key.type);
        if (idx != UINT_MAX) {
            cmp = ovsdb_atom_compare_3way(&have_datum->values[idx],
                                          &want_value,
                                          column->type.value.type);
        }

        ovsdb_atom_destroy(&want_key, column->type.key.type);
        ovsdb_atom_destroy(&want_value, column->type.value.type);
    } else {
        struct ovsdb_datum want_datum;

        die_if_error(ovsdb_datum_from_string(&want_datum, &column->type,
                                             value_string, symtab));
        idx = 0;
        cmp = ovsdb_datum_compare_3way(have_datum, &want_datum,
                                       &column->type);
        ovsdb_datum_destroy(&want_datum, &column->type);
    }

    free(key_string);
    free(value_string);

    return (idx == UINT_MAX ? false
            : !strcmp(operator, "=") ? cmp == 0
            : !strcmp(operator, "!=") ? cmp != 0
            : !strcmp(operator, "<") ? cmp < 0
            : !strcmp(operator, ">") ? cmp > 0
            : !strcmp(operator, "<=") ? cmp <= 0
            : !strcmp(operator, ">=") ? cmp >= 0
            : (abort(), 0));
}

static void
pre_cmd_wait_until(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const struct vsctl_table_class *table;
    int i;

    table = pre_get_table(ctx, table_name);

    for (i = 3; i < ctx->argc; i++) {
        pre_parse_column_key_value(ctx, ctx->argv[i], table);
    }
}

static void
cmd_wait_until(struct vsctl_context *ctx)
{
    const char *table_name = ctx->argv[1];
    const char *record_id = ctx->argv[2];
    const struct vsctl_table_class *table;
    const struct ovsdb_idl_row *row;
    int i;

    table = get_table(table_name);

    row = get_row(ctx, table, record_id);
    if (!row) {
        ctx->try_again = true;
        return;
    }

    for (i = 3; i < ctx->argc; i++) {
        if (!is_condition_satisfied(table, row, ctx->argv[i], ctx->symtab)) {
            ctx->try_again = true;
            return;
        }
    }
}

static struct json *
where_uuid_equals(const struct uuid *uuid)
{
    return
        json_array_create_1(
            json_array_create_3(
                json_string_create("_uuid"),
                json_string_create("=="),
                json_array_create_2(
                    json_string_create("uuid"),
                    json_string_create_nocopy(
                        xasprintf(UUID_FMT, UUID_ARGS(uuid))))));
}

static void
vsctl_context_init(struct vsctl_context *ctx, struct vsctl_command *command,
                   struct ovsdb_idl *idl, struct ovsdb_idl_txn *txn,
                   const struct ovsrec_open_vswitch *ovs,
                   struct ovsdb_symbol_table *symtab)
{
    ctx->argc = command->argc;
    ctx->argv = command->argv;
    ctx->options = command->options;

    ds_swap(&ctx->output, &command->output);
    ctx->table = command->table;
    ctx->idl = idl;
    ctx->txn = txn;
    ctx->ovs = ovs;
    ctx->symtab = symtab;
    ctx->verified_ports = false;

    ctx->try_again = false;
}

static void
vsctl_context_done(struct vsctl_context *ctx, struct vsctl_command *command)
{
    ds_swap(&ctx->output, &command->output);
    command->table = ctx->table;
}

static void
run_prerequisites(struct vsctl_command *commands, size_t n_commands,
                  struct ovsdb_idl *idl)
{
    struct vsctl_command *c;

    ovsdb_idl_add_table(idl, &ovsrec_table_open_vswitch);
    if (wait_for_reload) {
        ovsdb_idl_add_column(idl, &ovsrec_open_vswitch_col_cur_cfg);
    }
    for (c = commands; c < &commands[n_commands]; c++) {
        if (c->syntax->prerequisites) {
            struct vsctl_context ctx;

            ds_init(&c->output);
            c->table = NULL;

            vsctl_context_init(&ctx, c, idl, NULL, NULL, NULL);
            (c->syntax->prerequisites)(&ctx);
            vsctl_context_done(&ctx, c);

            assert(!c->output.string);
            assert(!c->table);
        }
    }
}

static enum ovsdb_idl_txn_status
do_vsctl(const char *args, struct vsctl_command *commands, size_t n_commands,
         struct ovsdb_idl *idl)
{
    struct ovsdb_idl_txn *txn;
    const struct ovsrec_open_vswitch *ovs;
    enum ovsdb_idl_txn_status status;
    struct ovsdb_symbol_table *symtab;
    struct vsctl_command *c;
    struct shash_node *node;
    int64_t next_cfg = 0;
    char *error = NULL;

    txn = the_idl_txn = ovsdb_idl_txn_create(idl);
    if (dry_run) {
        ovsdb_idl_txn_set_dry_run(txn);
    }

    ovsdb_idl_txn_add_comment(txn, "ovs-vsctl: %s", args);

    ovs = ovsrec_open_vswitch_first(idl);
    if (!ovs) {
        /* XXX add verification that table is empty */
        ovs = ovsrec_open_vswitch_insert(txn);
    }

    if (wait_for_reload) {
        struct json *where = where_uuid_equals(&ovs->header_.uuid);
        ovsdb_idl_txn_increment(txn, "Open_vSwitch", "next_cfg", where);
        json_destroy(where);
    }


    symtab = ovsdb_symbol_table_create();
    for (c = commands; c < &commands[n_commands]; c++) {
        ds_init(&c->output);
        c->table = NULL;
    }
    for (c = commands; c < &commands[n_commands]; c++) {
        struct vsctl_context ctx;

        vsctl_context_init(&ctx, c, idl, txn, ovs, symtab);
        if (c->syntax->run) {
            (c->syntax->run)(&ctx);
        }
        vsctl_context_done(&ctx, c);

        if (ctx.try_again) {
            status = TXN_AGAIN_WAIT;
            goto try_again;
        }
    }

    SHASH_FOR_EACH (node, &symtab->sh) {
        struct ovsdb_symbol *symbol = node->data;
        if (!symbol->created) {
            vsctl_fatal("row id \"%s\" is referenced but never created (e.g. "
                        "with \"-- --id=%s create ...\")",
                        node->name, node->name);
        }
        if (!symbol->strong_ref) {
            if (!symbol->weak_ref) {
                VLOG_WARN("row id \"%s\" was created but no reference to it "
                          "was inserted, so it will not actually appear in "
                          "the database", node->name);
            } else {
                VLOG_WARN("row id \"%s\" was created but only a weak "
                          "reference to it was inserted, so it will not "
                          "actually appear in the database", node->name);
            }
        }
    }
 
    status = ovsdb_idl_txn_commit_block(txn);
    if (wait_for_reload && status == TXN_SUCCESS) {
        next_cfg = ovsdb_idl_txn_get_increment_new_value(txn);
    }
    if (status == TXN_UNCHANGED || status == TXN_SUCCESS) {
        for (c = commands; c < &commands[n_commands]; c++) {
            if (c->syntax->postprocess) {
                struct vsctl_context ctx;

                vsctl_context_init(&ctx, c, idl, txn, ovs, symtab);
                (c->syntax->postprocess)(&ctx);
                vsctl_context_done(&ctx, c);
            }
        }
    }
    error = xstrdup(ovsdb_idl_txn_get_error(txn));
    ovsdb_idl_txn_destroy(txn);
    txn = the_idl_txn = NULL;
    

    switch (status) {
    case TXN_UNCOMMITTED:
    case TXN_INCOMPLETE:
        NOT_REACHED();

    case TXN_ABORTED:
        /* Should not happen--we never call ovsdb_idl_txn_abort(). */
        vsctl_fatal("transaction aborted");

    case TXN_UNCHANGED:
    case TXN_SUCCESS:
        break;

    case TXN_AGAIN_WAIT:
    case TXN_AGAIN_NOW:
        goto try_again;

    case TXN_ERROR:
        vsctl_fatal("transaction error: %s", error);

    case TXN_NOT_LOCKED:
        /* Should not happen--we never call ovsdb_idl_set_lock(). */
        vsctl_fatal("database not locked");

    default:
        NOT_REACHED();
    }
    free(error);

    ovsdb_symbol_table_destroy(symtab);
   
    for (c = commands; c < &commands[n_commands]; c++) {
        struct ds *ds = &c->output;

        if (c->table) {
            table_print(c->table, &table_style);
        } else if (oneline) {
            size_t j;

            ds_chomp(ds, '\n');
            for (j = 0; j < ds->length; j++) {
                int ch = ds->string[j];
                switch (ch) {
                case '\n':
                    fputs("\\n", stdout);
                    break;

                case '\\':
                    fputs("\\\\", stdout);
                    break;

                default:
                    putchar(ch);
                }
            }
            putchar('\n');
        } else {
            fputs(ds_cstr(ds), stdout);
	    
     	    int k;
    	    if(verifyqos[sp] == false)
		    for(k=0;k<36;k++)	
			sqos[sp][k] = ds_cstr(ds)[k];
    	
	    
        }
        ds_destroy(&c->output);
        table_destroy(c->table);
        free(c->table);

        smap_destroy(&c->options);
    }
    free(commands);
    
    
    /*if (wait_for_reload && status != TXN_UNCHANGED) {
	VLOG_INFO("6");
        for (;;) {
	  
            ovsdb_idl_run(idl);
            OVSREC_OPEN_VSWITCH_FOR_EACH (ovs, idl) {
                if (ovs->cur_cfg >= next_cfg) {
                    goto done;
                }
            }
            ovsdb_idl_wait(idl);
            poll_block();
        }
    VLOG_INFO("7");
    done: ;
    VLOG_INFO("8");
    }*/
    
    
    ovsdb_idl_destroy(idl);
 

    return status;

try_again:
    /* Our transaction needs to be rerun, or a prerequisite was not met.  Free
     * resources and return so that the caller can try again. */
    if (txn) {
        ovsdb_idl_txn_abort(txn);
        ovsdb_idl_txn_destroy(txn);
    }
    ovsdb_symbol_table_destroy(symtab);
    for (c = commands; c < &commands[n_commands]; c++) {
        ds_destroy(&c->output);
        table_destroy(c->table);
        free(c->table);
    }
    free(error);

    return status;
}

static const struct vsctl_command_syntax all_commands[] = {
    /* Open vSwitch commands. */
    {"init", 0, 0, NULL, cmd_init, NULL, "", RW},
    {"show", 0, 0, pre_cmd_show, cmd_show, NULL, "", RO},

    /* Bridge commands. */
    {"add-br", 1, 3, pre_get_info, cmd_add_br, NULL, "--may-exist", RW},
    {"del-br", 1, 1, pre_get_info, cmd_del_br, NULL, "--if-exists", RW},
    {"list-br", 0, 0, pre_get_info, cmd_list_br, NULL, "", RO},
    {"br-exists", 1, 1, pre_get_info, cmd_br_exists, NULL, "", RO},
    {"br-to-vlan", 1, 1, pre_get_info, cmd_br_to_vlan, NULL, "", RO},
    {"br-to-parent", 1, 1, pre_get_info, cmd_br_to_parent, NULL, "", RO},
    {"br-set-external-id", 2, 3, pre_cmd_br_set_external_id,
     cmd_br_set_external_id, NULL, "", RW},
    {"br-get-external-id", 1, 2, pre_cmd_br_get_external_id,
     cmd_br_get_external_id, NULL, "", RO},

    /* Port commands. */
    {"list-ports", 1, 1, pre_get_info, cmd_list_ports, NULL, "", RO},
    {"add-port", 2, INT_MAX, pre_get_info, cmd_add_port, NULL, "--may-exist",
     RW},
    {"add-bond", 4, INT_MAX, pre_get_info, cmd_add_bond, NULL,
     "--may-exist,--fake-iface", RW},
    {"del-port", 1, 2, pre_get_info, cmd_del_port, NULL,
     "--if-exists,--with-iface", RW},
    {"port-to-br", 1, 1, pre_get_info, cmd_port_to_br, NULL, "", RO},

    /* Interface commands. */
    {"list-ifaces", 1, 1, pre_get_info, cmd_list_ifaces, NULL, "", RO},
    {"iface-to-br", 1, 1, pre_get_info, cmd_iface_to_br, NULL, "", RO},

    /* Controller commands. */
    {"get-controller", 1, 1, pre_controller, cmd_get_controller, NULL, "", RO},
    {"del-controller", 1, 1, pre_controller, cmd_del_controller, NULL, "", RW},
    {"set-controller", 1, INT_MAX, pre_controller, cmd_set_controller, NULL,
     "", RW},
    {"get-fail-mode", 1, 1, pre_get_info, cmd_get_fail_mode, NULL, "", RO},
    {"del-fail-mode", 1, 1, pre_get_info, cmd_del_fail_mode, NULL, "", RW},
    {"set-fail-mode", 2, 2, pre_get_info, cmd_set_fail_mode, NULL, "", RW},

    /* Manager commands. */
    {"get-manager", 0, 0, pre_manager, cmd_get_manager, NULL, "", RO},
    {"del-manager", 0, INT_MAX, pre_manager, cmd_del_manager, NULL, "", RW},
    {"set-manager", 1, INT_MAX, pre_manager, cmd_set_manager, NULL, "", RW},

    /* SSL commands. */
    {"get-ssl", 0, 0, pre_cmd_get_ssl, cmd_get_ssl, NULL, "", RO},
    {"del-ssl", 0, 0, pre_cmd_del_ssl, cmd_del_ssl, NULL, "", RW},
    {"set-ssl", 3, 3, pre_cmd_set_ssl, cmd_set_ssl, NULL, "--bootstrap", RW},

    /* Switch commands. */
    {"emer-reset", 0, 0, pre_cmd_emer_reset, cmd_emer_reset, NULL, "", RW},

    /* Database commands. */
    {"comment", 0, INT_MAX, NULL, NULL, NULL, "", RO},
    {"get", 2, INT_MAX, pre_cmd_get, cmd_get, NULL, "--if-exists,--id=", RO},
    {"list", 1, INT_MAX, pre_cmd_list, cmd_list, NULL, "--columns=", RO},
    {"find", 1, INT_MAX, pre_cmd_find, cmd_find, NULL, "--columns=", RO},
    {"set", 3, INT_MAX, pre_cmd_set, cmd_set, NULL, "", RW},
    {"add", 4, INT_MAX, pre_cmd_add, cmd_add, NULL, "", RW},
    {"remove", 4, INT_MAX, pre_cmd_remove, cmd_remove, NULL, "", RW},
    {"clear", 3, INT_MAX, pre_cmd_clear, cmd_clear, NULL, "", RW},
    {"create", 2, INT_MAX, pre_create, cmd_create, post_create, "--id=", RW},
    {"destroy", 1, INT_MAX, pre_cmd_destroy, cmd_destroy, NULL, "--if-exists",
     RW},
    {"wait-until", 2, INT_MAX, pre_cmd_wait_until, cmd_wait_until, NULL, "",
     RO},

    {NULL, 0, 0, NULL, NULL, NULL, NULL, RO},
};

