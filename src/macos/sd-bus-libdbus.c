/*
 * sd-bus-libdbus.c — sd-bus API implementation for macOS using libdbus-1.
 */

#include <sdbus-c++/macos/sd-bus-libdbus.h>

#include <dbus/dbus.h>

#include <assert.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Internal limits ──────────────────────────────────────────────────────── */

#define SBUS_MAX_ITER_DEPTH  32
#define SBUS_MAX_PENDING_STR 8192

/* ── Slot type tag ────────────────────────────────────────────────────────── */

typedef enum {
    SLOT_VTABLE,
    SLOT_OBJECT_MANAGER,
    SLOT_MATCH,
} SlotType;

/* ── Internal struct definitions ─────────────────────────────────────────── */

struct sd_bus_slot {
    SlotType type;
    sd_bus  *bus;

    /* SLOT_VTABLE / SLOT_OBJECT_MANAGER */
    char              *path;
    char              *interface;
    const sd_bus_vtable *vtable;
    void              *userdata;

    /* SLOT_MATCH */
    sd_bus_message_handler_t match_callback;
    void                    *match_userdata;

    struct sd_bus_slot *next;
};

struct sd_bus_creds {
    pid_t pid;
    uid_t uid, euid;
    gid_t gid, egid;
    int   ref_count;
};

struct sd_bus_message {
    DBusMessage *msg;
    sd_bus      *bus;
    int          ref_count;

    /* Write iterator stack */
    DBusMessageIter wr_stack[SBUS_MAX_ITER_DEPTH];
    int             wr_depth;        /* current write level */
    bool            wr_init;         /* wr_stack[0] initialised */

    /* Read iterator stack */
    DBusMessageIter rd_stack[SBUS_MAX_ITER_DEPTH];
    int             rd_depth;
    bool            rd_init;

    /* Deferred string for append_string_space */
    char  *pending_str;
    size_t pending_str_len;

    /* Set after sd_bus_send() */
    bool reply_sent;
};

struct sd_bus {
    DBusConnection *conn;

    /* Pre-start config */
    char *pending_address;
    bool  is_bus_client;

    /* Slots (vtables + matches) */
    sd_bus_slot *slots;

    /* Currently dispatched message (for sd_bus_get_current_message) */
    sd_bus_message *current_message;

    /* Cached unique name */
    char *unique_name;

    /* Method-call timeout (µs), 0 = default (25 s) */
    uint64_t call_timeout_usec;

    int ref_count;
};

/* ─────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ───────────────────────────────────────────────────────────────────────────*/

static sd_bus_message *msg_alloc(sd_bus *bus, DBusMessage *dbus_msg)
{
    sd_bus_message *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->msg       = dbus_message_ref(dbus_msg);
    m->bus       = bus;
    m->ref_count = 1;
    return m;
}

/* Flush a deferred pending_str into the current write iterator. */
static int flush_pending_str(sd_bus_message *m)
{
    if (!m->pending_str) return 0;
    const char *s = m->pending_str;
    DBusMessageIter *it = &m->wr_stack[m->wr_depth];
    int ok = dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &s);
    free(m->pending_str);
    m->pending_str     = NULL;
    m->pending_str_len = 0;
    return ok ? 0 : -ENOMEM;
}

/* Ensure the write iterator for level 0 is initialised. */
static void ensure_wr_init(sd_bus_message *m)
{
    if (!m->wr_init) {
        dbus_message_iter_init_append(m->msg, &m->wr_stack[0]);
        m->wr_init  = true;
        m->wr_depth = 0;
    }
}

/* Ensure the read iterator for level 0 is initialised. */
static void ensure_rd_init(sd_bus_message *m)
{
    if (!m->rd_init) {
        dbus_message_iter_init(m->msg, &m->rd_stack[0]);
        m->rd_init  = true;
        m->rd_depth = 0;
    }
}

/* Return the type-size for fixed-width D-Bus types (bytes). */
static int fixed_type_size(char type)
{
    switch (type) {
    case 'y': return 1;
    case 'b': return 4; /* D-Bus boolean is 4 bytes on wire */
    case 'n': case 'q': return 2;
    case 'i': case 'u': case 'h': return 4;
    case 'x': case 't': return 8;
    case 'd': return 8;
    default:  return 0;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Introspection XML generator
 * ───────────────────────────────────────────────────────────────────────────*/

/* Append a formatted string to a heap buffer; buf/cap updated. */
static int buf_appendf(char **buf, size_t *cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return -1;

    if (*len + (size_t)need + 1 > *cap) {
        size_t new_cap = (*cap + (size_t)need + 1) * 2;
        char *tmp = realloc(*buf, new_cap);
        if (!tmp) return -1;
        *buf = tmp;
        *cap = new_cap;
    }
    va_start(ap, fmt);
    vsnprintf(*buf + *len, *cap - *len, fmt, ap);
    va_end(ap);
    *len += (size_t)need;
    return 0;
}

/* Walk a signature string and emit <arg> elements. direction: "in" or "out". */
static int emit_args(char **buf, size_t *cap, size_t *len,
                     const char *sig, const char *direction)
{
    if (!sig || !*sig) return 0;
    /* Each top-level complete type becomes one arg */
    const char *p = sig;
    while (*p) {
        char type[2] = { *p, 0 };
        /* Skip nested types (struct/array etc.) — just emit the outer char for
         * introspection; full type traversal not needed for basic use. */
        if (buf_appendf(buf, cap, len,
                "      <arg direction=\"%s\" type=\"%s\"/>\n",
                direction, type) < 0)
            return -ENOMEM;
        p++;
        /* Skip past struct or array contents */
        if (*p == '(') {
            int depth = 1; p++;
            while (*p && depth) { if (*p=='(') depth++; else if (*p==')') depth--; p++; }
        } else if (*(p-1) == 'a') {
            if (*p == '(') {
                int depth = 1; p++;
                while (*p && depth) { if (*p=='(') depth++; else if (*p==')') depth--; p++; }
            } else {
                p++; /* skip array element type */
            }
        }
    }
    return 0;
}

static char *build_introspect_xml(sd_bus *bus, const char *path)
{
    size_t cap = 2048, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    buf_appendf(&buf, &cap, &len,
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-Bus Object Introspection 1.0//EN\"\n"
        "  \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node name=\"%s\">\n", path);

    /* Emit each registered vtable for this path */
    for (sd_bus_slot *s = bus->slots; s; s = s->next) {
        if (s->type != SLOT_VTABLE) continue;
        if (strcmp(s->path, path) != 0) continue;

        buf_appendf(&buf, &cap, &len,
            "  <interface name=\"%s\">\n", s->interface);

        for (const sd_bus_vtable *vt = s->vtable;
             vt->type != _SD_BUS_VTABLE_END; vt++) {
            if (vt->type == _SD_BUS_VTABLE_METHOD) {
                buf_appendf(&buf, &cap, &len,
                    "    <method name=\"%s\">\n", vt->x.method.member);
                emit_args(&buf, &cap, &len, vt->x.method.signature, "in");
                emit_args(&buf, &cap, &len, vt->x.method.result,    "out");
                buf_appendf(&buf, &cap, &len, "    </method>\n");
            } else if (vt->type == _SD_BUS_VTABLE_SIGNAL) {
                buf_appendf(&buf, &cap, &len,
                    "    <signal name=\"%s\">\n", vt->x.signal.member);
                emit_args(&buf, &cap, &len, vt->x.signal.signature, "out");
                buf_appendf(&buf, &cap, &len, "    </signal>\n");
            } else if (vt->type == _SD_BUS_VTABLE_PROPERTY ||
                       vt->type == _SD_BUS_VTABLE_WRITABLE_PROPERTY) {
                const char *access = (vt->type == _SD_BUS_VTABLE_WRITABLE_PROPERTY)
                                     ? "readwrite" : "read";
                buf_appendf(&buf, &cap, &len,
                    "    <property name=\"%s\" type=\"%s\" access=\"%s\"/>\n",
                    vt->x.property.member, vt->x.property.signature, access);
            }
        }
        buf_appendf(&buf, &cap, &len, "  </interface>\n");
    }

    /* Standard Introspectable interface */
    buf_appendf(&buf, &cap, &len,
        "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
        "    <method name=\"Introspect\">\n"
        "      <arg direction=\"out\" type=\"s\"/>\n"
        "    </method>\n"
        "  </interface>\n"
        "</node>\n");

    return buf;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Fallback message dispatcher (registered once per connection)
 * ───────────────────────────────────────────────────────────────────────────*/

static DBusHandlerResult fallback_handler(DBusConnection *conn,
                                          DBusMessage    *dbus_msg,
                                          void           *user_data)
{
    sd_bus *bus = (sd_bus *)user_data;
    int type = dbus_message_get_type(dbus_msg);
    if (type != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *path   = dbus_message_get_path(dbus_msg);
    const char *iface  = dbus_message_get_interface(dbus_msg);
    const char *member = dbus_message_get_member(dbus_msg);
    if (!path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /* ── Introspect ── */
    if ((!iface || strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0)
        && member && strcmp(member, "Introspect") == 0) {
        char *xml = build_introspect_xml(bus, path);
        DBusMessage *reply = dbus_message_new_method_return(dbus_msg);
        if (xml && reply) {
            const char *xp = xml;
            dbus_message_append_args(reply,
                DBUS_TYPE_STRING, &xp, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, NULL);
        }
        free(xml);
        if (reply) dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* ── Match signals ── */
    if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
        for (sd_bus_slot *s = bus->slots; s; s = s->next) {
            if (s->type != SLOT_MATCH || !s->match_callback) continue;
            sd_bus_message *m = msg_alloc(bus, dbus_msg);
            if (!m) return DBUS_HANDLER_RESULT_NEED_MEMORY;
            bus->current_message = m;
            sd_bus_error err = SD_BUS_ERROR_NULL;
            s->match_callback(m, s->match_userdata, &err);
            sd_bus_error_free(&err);
            bus->current_message = NULL;
            sd_bus_message_unref(m);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* ── Vtable method dispatch ── */
    for (sd_bus_slot *s = bus->slots; s; s = s->next) {
        if (s->type != SLOT_VTABLE) continue;
        if (strcmp(s->path, path) != 0) continue;
        if (iface && strcmp(s->interface, iface) != 0) continue;

        for (const sd_bus_vtable *vt = s->vtable;
             vt->type != _SD_BUS_VTABLE_END; vt++) {
            if (vt->type != _SD_BUS_VTABLE_METHOD) continue;
            if (!member || strcmp(vt->x.method.member, member) != 0) continue;

            sd_bus_message *m = msg_alloc(bus, dbus_msg);
            if (!m) return DBUS_HANDLER_RESULT_NEED_MEMORY;
            bus->current_message = m;

            sd_bus_error err = SD_BUS_ERROR_NULL;
            int r = vt->x.method.handler(m, s->userdata, &err);
            bus->current_message = NULL;

            if (r < 0 && !m->reply_sent) {
                const char *ename = (err.name && *err.name)
                                    ? err.name : "org.freedesktop.DBus.Error.Failed";
                const char *emsg  = err.message ? err.message : "Handler error";
                DBusMessage *er = dbus_message_new_error(dbus_msg, ename, emsg);
                if (er) { dbus_connection_send(conn, er, NULL); dbus_message_unref(er); }
            }
            sd_bus_error_free(&err);
            sd_bus_message_unref(m);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    /* Method not found */
    DBusMessage *err = dbus_message_new_error(dbus_msg,
        "org.freedesktop.DBus.Error.UnknownMethod", "Method not found");
    if (err) { dbus_connection_send(conn, err, NULL); dbus_message_unref(err); }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable g_fallback_vtable = {
    .unregister_function = NULL,
    .message_function    = fallback_handler,
};

/* Register the fallback handler once; ignore if already registered. */
static int register_fallback(sd_bus *bus)
{
    DBusError derr;
    dbus_error_init(&derr);
    dbus_connection_try_register_fallback(
        bus->conn, "/", &g_fallback_vtable, bus, &derr);
    /* Ignore "already registered" error */
    dbus_error_free(&derr);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Bus open / lifecycle
 * ───────────────────────────────────────────────────────────────────────────*/

static sd_bus *bus_alloc(void)
{
    /* Enable thread-safety on first use */
    static int threads_init = 0;
    if (!threads_init) { dbus_threads_init_default(); threads_init = 1; }

    sd_bus *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->ref_count = 1;
    return b;
}

static int bus_wrap(DBusConnection *conn, bool is_bus_client, sd_bus **ret)
{
    sd_bus *b = bus_alloc();
    if (!b) { dbus_connection_unref(conn); return -ENOMEM; }
    b->conn          = conn;
    b->is_bus_client = is_bus_client;
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    register_fallback(b);
    *ret = b;
    return 0;
}

int sd_bus_new(sd_bus **ret)
{
    if (!ret) return -EINVAL;
    sd_bus *b = bus_alloc();
    if (!b) return -ENOMEM;
    *ret = b;
    return 0;
}

int sd_bus_set_address(sd_bus *bus, const char *address)
{
    if (!bus || !address) return -EINVAL;
    free(bus->pending_address);
    bus->pending_address = strdup(address);
    return bus->pending_address ? 0 : -ENOMEM;
}

int sd_bus_set_bus_client(sd_bus *bus, int b)
{
    if (!bus) return -EINVAL;
    bus->is_bus_client = (b != 0);
    return 0;
}

int sd_bus_set_trusted(sd_bus *bus, int b)  { (void)bus; (void)b; return 0; }
int sd_bus_set_fd(sd_bus *bus, int in, int out) { (void)bus; (void)in; (void)out; return -EOPNOTSUPP; }
int sd_bus_set_server(sd_bus *bus, int b, sd_id128_t id) { (void)bus; (void)b; (void)id; return -EOPNOTSUPP; }
int sd_bus_set_anonymous(sd_bus *bus, int b) { (void)bus; (void)b; return 0; }
int sd_bus_set_method_call_timeout(sd_bus *bus, uint64_t usec)
    { if (!bus) return -EINVAL; bus->call_timeout_usec = usec; return 0; }
int sd_bus_get_method_call_timeout(sd_bus *bus, uint64_t *ret)
    { if (!bus || !ret) return -EINVAL; *ret = bus->call_timeout_usec ? bus->call_timeout_usec : 25000000ULL; return 0; }

int sd_bus_start(sd_bus *bus)
{
    if (!bus) return -EINVAL;
    if (!bus->pending_address) return -EINVAL;

    DBusError derr;
    dbus_error_init(&derr);
    bus->conn = dbus_connection_open_private(bus->pending_address, &derr);
    if (!bus->conn) { dbus_error_free(&derr); return -ECONNREFUSED; }

    if (bus->is_bus_client) {
        if (!dbus_bus_register(bus->conn, &derr)) {
            dbus_connection_close(bus->conn);
            dbus_connection_unref(bus->conn);
            bus->conn = NULL;
            dbus_error_free(&derr);
            return -EACCES;
        }
    }
    dbus_connection_set_exit_on_disconnect(bus->conn, FALSE);
    register_fallback(bus);
    free(bus->pending_address);
    bus->pending_address = NULL;
    return 0;
}

/* Open system or session bus, checking env var first. */
static int open_well_known_bus(DBusBusType dtype, const char *env_var, sd_bus **ret)
{
    DBusError derr;
    dbus_error_init(&derr);
    DBusConnection *conn = NULL;

    const char *addr = getenv(env_var);
    if (addr && *addr) {
        conn = dbus_connection_open_private(addr, &derr);
        if (conn && !dbus_bus_register(conn, &derr)) {
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            conn = NULL;
            dbus_error_free(&derr);
            dbus_error_init(&derr);
        }
    }
    if (!conn) {
        dbus_error_free(&derr);
        dbus_error_init(&derr);
        conn = dbus_bus_get_private(dtype, &derr);
    }
    if (!conn) { dbus_error_free(&derr); return -ECONNREFUSED; }
    return bus_wrap(conn, true, ret);
}

int sd_bus_open(sd_bus **ret)
{
    return open_well_known_bus(DBUS_BUS_SESSION, "DBUS_SESSION_BUS_ADDRESS", ret);
}
int sd_bus_open_system(sd_bus **ret)
{
    return open_well_known_bus(DBUS_BUS_SYSTEM, "DBUS_SYSTEM_BUS_ADDRESS", ret);
}
int sd_bus_open_user(sd_bus **ret)
{
    return open_well_known_bus(DBUS_BUS_SESSION, "DBUS_SESSION_BUS_ADDRESS", ret);
}
int sd_bus_open_system_remote(sd_bus **ret, const char *host)
    { (void)ret; (void)host; return -EOPNOTSUPP; }

int sd_bus_open_user_with_address(sd_bus **ret, const char *address)
{
    if (!ret || !address) return -EINVAL;
    DBusError derr; dbus_error_init(&derr);
    DBusConnection *conn = dbus_connection_open_private(address, &derr);
    if (!conn) { dbus_error_free(&derr); return -ECONNREFUSED; }
    if (!dbus_bus_register(conn, &derr)) {
        dbus_connection_close(conn); dbus_connection_unref(conn);
        dbus_error_free(&derr); return -EACCES;
    }
    return bus_wrap(conn, true, ret);
}

int sd_bus_open_direct(sd_bus **ret, const char *address)
{
    if (!ret || !address) return -EINVAL;
    DBusError derr; dbus_error_init(&derr);
    DBusConnection *conn = dbus_connection_open_private(address, &derr);
    if (!conn) { dbus_error_free(&derr); return -ECONNREFUSED; }
    return bus_wrap(conn, false, ret);
}

void sd_bus_close(sd_bus *bus)
{
    if (!bus || !bus->conn) return;
    dbus_connection_close(bus->conn);
}

static void bus_free(sd_bus *bus)
{
    if (!bus) return;
    sd_bus_close(bus);
    if (bus->conn) { dbus_connection_unref(bus->conn); bus->conn = NULL; }
    free(bus->pending_address);
    free(bus->unique_name);
    /* Free all slots */
    sd_bus_slot *s = bus->slots;
    while (s) {
        sd_bus_slot *next = s->next;
        free(s->path); free(s->interface); free(s);
        s = next;
    }
    free(bus);
}

sd_bus *sd_bus_unref(sd_bus *bus)
{
    if (!bus) return NULL;
    if (--bus->ref_count <= 0) bus_free(bus);
    return NULL;
}

int sd_bus_flush(sd_bus *bus)
{
    if (!bus || !bus->conn) return -EINVAL;
    dbus_connection_flush(bus->conn);
    return 0;
}

sd_bus *sd_bus_flush_close_unref(sd_bus *bus)
{
    if (bus) { sd_bus_flush(bus); sd_bus_close(bus); }
    return sd_bus_unref(bus);
}

sd_bus *sd_bus_close_unref(sd_bus *bus)
{
    if (bus) sd_bus_close(bus);
    return sd_bus_unref(bus);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Bus name
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_request_name(sd_bus *bus, const char *name, uint64_t flags)
{
    if (!bus || !bus->conn || !name) return -EINVAL;
    DBusError derr; dbus_error_init(&derr);
    /* DBUS_NAME_FLAG_DO_NOT_QUEUE = 4, REPLACE_EXISTING = 1, ALLOW_REPLACEMENT = 2 */
    unsigned int dflags = DBUS_NAME_FLAG_DO_NOT_QUEUE;
    int r = dbus_bus_request_name(bus->conn, name, dflags, &derr);
    dbus_error_free(&derr);
    if (r < 0) return -EACCES;
    if (r == DBUS_REQUEST_NAME_REPLY_EXISTS) return -EEXIST;
    return 0;
}

int sd_bus_release_name(sd_bus *bus, const char *name)
{
    if (!bus || !bus->conn || !name) return -EINVAL;
    DBusError derr; dbus_error_init(&derr);
    dbus_bus_release_name(bus->conn, name, &derr);
    dbus_error_free(&derr);
    return 0;
}

int sd_bus_get_unique_name(sd_bus *bus, const char **ret)
{
    if (!bus || !bus->conn || !ret) return -EINVAL;
    if (!bus->unique_name) {
        const char *n = dbus_bus_get_unique_name(bus->conn);
        if (!n) return -ENODATA;
        bus->unique_name = strdup(n);
        if (!bus->unique_name) return -ENOMEM;
    }
    *ret = bus->unique_name;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Event loop
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_process(sd_bus *bus, sd_bus_message **ret_msg)
{
    if (!bus || !bus->conn) return -EINVAL;
    if (ret_msg) *ret_msg = NULL;
    dbus_connection_read_write(bus->conn, 0);
    DBusDispatchStatus st = dbus_connection_dispatch(bus->conn);
    return (st == DBUS_DISPATCH_DATA_REMAINS) ? 1 : 0;
}

int sd_bus_get_fd(sd_bus *bus)
{
    if (!bus || !bus->conn) return -EINVAL;
    int fd;
    if (!dbus_connection_get_socket(bus->conn, &fd)) return -EIO;
    return fd;
}

int sd_bus_get_events(sd_bus *bus)
{
    if (!bus) return -EINVAL;
    return POLLIN;
}

int sd_bus_get_timeout(sd_bus *bus, uint64_t *timeout_usec)
{
    if (!bus || !timeout_usec) return -EINVAL;
    DBusDispatchStatus st = dbus_connection_get_dispatch_status(bus->conn);
    *timeout_usec = (st == DBUS_DISPATCH_DATA_REMAINS) ? 0 : UINT64_MAX;
    return 0;
}

int sd_bus_get_n_queued_read(sd_bus *bus, uint64_t *ret)
{
    if (!bus || !ret) return -EINVAL;
    DBusDispatchStatus st = dbus_connection_get_dispatch_status(bus->conn);
    *ret = (st == DBUS_DISPATCH_DATA_REMAINS) ? 1 : 0;
    return 0;
}

int sd_bus_get_n_queued_write(sd_bus *bus, uint64_t *ret)
{
    if (!bus || !ret) return -EINVAL;
    *ret = 0;
    return 0;
}

sd_bus_message *sd_bus_get_current_message(sd_bus *bus)
{
    return bus ? bus->current_message : NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message lifecycle
 * ───────────────────────────────────────────────────────────────────────────*/

sd_bus_message *sd_bus_message_ref(sd_bus_message *m)
{
    if (m) m->ref_count++;
    return m;
}

sd_bus_message *sd_bus_message_unref(sd_bus_message *m)
{
    if (!m) return NULL;
    if (--m->ref_count > 0) return NULL;
    free(m->pending_str);
    dbus_message_unref(m->msg);
    free(m);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message creation
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_message_new(sd_bus *bus, sd_bus_message **ret, uint8_t type)
{
    if (!bus || !ret) return -EINVAL;
    DBusMessage *dm = dbus_message_new((int)type);
    if (!dm) return -ENOMEM;
    sd_bus_message *m = msg_alloc(bus, dm);
    dbus_message_unref(dm);
    if (!m) return -ENOMEM;
    *ret = m;
    return 0;
}

int sd_bus_message_new_method_call(sd_bus *bus, sd_bus_message **ret,
                                   const char *destination, const char *path,
                                   const char *interface, const char *member)
{
    if (!bus || !ret || !path || !member) return -EINVAL;
    DBusMessage *dm = dbus_message_new_method_call(destination, path, interface, member);
    if (!dm) return -ENOMEM;
    sd_bus_message *m = msg_alloc(bus, dm);
    dbus_message_unref(dm);
    if (!m) return -ENOMEM;
    ensure_wr_init(m);
    *ret = m;
    return 0;
}

int sd_bus_message_new_signal(sd_bus *bus, sd_bus_message **ret,
                              const char *path, const char *interface,
                              const char *member)
{
    if (!bus || !ret || !path || !interface || !member) return -EINVAL;
    DBusMessage *dm = dbus_message_new_signal(path, interface, member);
    if (!dm) return -ENOMEM;
    sd_bus_message *m = msg_alloc(bus, dm);
    dbus_message_unref(dm);
    if (!m) return -ENOMEM;
    ensure_wr_init(m);
    *ret = m;
    return 0;
}

int sd_bus_message_new_method_return(sd_bus_message *call, sd_bus_message **ret)
{
    if (!call || !ret) return -EINVAL;
    DBusMessage *dm = dbus_message_new_method_return(call->msg);
    if (!dm) return -ENOMEM;
    sd_bus_message *m = msg_alloc(call->bus, dm);
    dbus_message_unref(dm);
    if (!m) return -ENOMEM;
    ensure_wr_init(m);
    *ret = m;
    return 0;
}

int sd_bus_message_new_method_error(sd_bus_message *call, sd_bus_message **ret,
                                    const sd_bus_error *err)
{
    if (!call || !ret) return -EINVAL;
    const char *name = (err && err->name) ? err->name : "org.freedesktop.DBus.Error.Failed";
    const char *msg  = (err && err->message) ? err->message : "";
    DBusMessage *dm = dbus_message_new_error(call->msg, name, msg);
    if (!dm) return -ENOMEM;
    sd_bus_message *m = msg_alloc(call->bus, dm);
    dbus_message_unref(dm);
    if (!m) return -ENOMEM;
    *ret = m;
    return 0;
}

int sd_bus_message_set_destination(sd_bus_message *m, const char *dest)
{
    if (!m || !dest) return -EINVAL;
    return dbus_message_set_destination(m->msg, dest) ? 0 : -ENOMEM;
}

int sd_bus_message_set_expect_reply(sd_bus_message *m, int b)
{
    if (!m) return -EINVAL;
    dbus_message_set_no_reply(m->msg, !b);
    return 0;
}

int sd_bus_message_get_expect_reply(sd_bus_message *m)
{
    if (!m) return 0;
    return !dbus_message_get_no_reply(m->msg);
}

int sd_bus_message_seal(sd_bus_message *m, uint64_t cookie, uint64_t timeout)
{
    (void)timeout;
    if (!m) return -EINVAL;
    flush_pending_str(m);
    if (cookie) dbus_message_set_serial(m->msg, (dbus_uint32_t)cookie);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message metadata
 * ───────────────────────────────────────────────────────────────────────────*/

const char *sd_bus_message_get_interface(sd_bus_message *m)
    { return m ? dbus_message_get_interface(m->msg) : NULL; }
const char *sd_bus_message_get_member(sd_bus_message *m)
    { return m ? dbus_message_get_member(m->msg) : NULL; }
const char *sd_bus_message_get_sender(sd_bus_message *m)
    { return m ? dbus_message_get_sender(m->msg) : NULL; }
const char *sd_bus_message_get_path(sd_bus_message *m)
    { return m ? dbus_message_get_path(m->msg) : NULL; }
const char *sd_bus_message_get_destination(sd_bus_message *m)
    { return m ? dbus_message_get_destination(m->msg) : NULL; }

int sd_bus_message_get_cookie(sd_bus_message *m, uint64_t *ret)
{
    if (!m || !ret) return -EINVAL;
    *ret = dbus_message_get_serial(m->msg);
    return 0;
}

int sd_bus_message_get_reply_cookie(sd_bus_message *m, uint64_t *ret)
{
    if (!m || !ret) return -EINVAL;
    *ret = dbus_message_get_reply_serial(m->msg);
    return 0;
}

const sd_bus_error *sd_bus_message_get_error(sd_bus_message *m)
{
    (void)m;
    return NULL; /* simplified: callers check return value */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message send / call
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_send(sd_bus *bus, sd_bus_message *m, uint64_t *cookie)
{
    if (!bus || !bus->conn || !m) return -EINVAL;
    flush_pending_str(m);
    dbus_uint32_t serial = 0;
    if (!dbus_connection_send(bus->conn, m->msg, &serial)) return -ENOMEM;
    if (cookie) *cookie = serial;
    m->reply_sent = true;
    return 0;
}

int sd_bus_call(sd_bus *bus, sd_bus_message *m, uint64_t usec,
                sd_bus_error *ret_error, sd_bus_message **reply)
{
    if (!bus || !bus->conn || !m) return -EINVAL;
    flush_pending_str(m);

    int timeout_ms = usec ? (int)(usec / 1000) : -1;
    DBusError derr; dbus_error_init(&derr);
    DBusMessage *dr = dbus_connection_send_with_reply_and_block(
        bus->conn, m->msg, timeout_ms, &derr);
    if (!dr) {
        if (ret_error)
            sd_bus_error_set(ret_error, derr.name, derr.message);
        dbus_error_free(&derr);
        return -EIO;
    }
    if (reply) {
        sd_bus_message *rm = msg_alloc(bus, dr);
        dbus_message_unref(dr);
        if (!rm) return -ENOMEM;
        *reply = rm;
    } else {
        dbus_message_unref(dr);
    }
    return 0;
}

int sd_bus_call_async(sd_bus *bus, sd_bus_slot **slot, sd_bus_message *m,
                      sd_bus_message_handler_t callback, void *userdata,
                      uint64_t usec)
{
    /* Simplified: send and forget; reply handled via dispatch */
    (void)slot; (void)callback; (void)userdata; (void)usec;
    return sd_bus_send(bus, m, NULL);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message append (write path)
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_message_append_basic(sd_bus_message *m, char type, const void *p)
{
    if (!m || !p) return -EINVAL;
    ensure_wr_init(m);
    int r = flush_pending_str(m);
    if (r < 0) return r;
    DBusMessageIter *it = &m->wr_stack[m->wr_depth];
    /* UNIX_FD not supported in basic libdbus portable path */
    if (!dbus_message_iter_append_basic(it, (int)type, p)) return -ENOMEM;
    return 0;
}

int sd_bus_message_append_string_space(sd_bus_message *m, size_t size, char **ret)
{
    if (!m || !ret) return -EINVAL;
    ensure_wr_init(m);
    /* Flush any previous pending string */
    int r = flush_pending_str(m);
    if (r < 0) return r;
    if (size >= SBUS_MAX_PENDING_STR) return -E2BIG;
    m->pending_str = calloc(size + 1, 1);
    if (!m->pending_str) return -ENOMEM;
    m->pending_str_len = size;
    *ret = m->pending_str;
    return 0;
}

int sd_bus_message_append_array(sd_bus_message *m, char type,
                                const void *ptr, size_t size)
{
    if (!m) return -EINVAL;
    ensure_wr_init(m);
    flush_pending_str(m);

    int esz = fixed_type_size(type);
    if (esz <= 0) return -EINVAL;
    int n_elements = (int)(size / (size_t)esz);

    DBusMessageIter *it = &m->wr_stack[m->wr_depth];
    char sig[2] = { type, 0 };
    DBusMessageIter sub;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, sig, &sub))
        return -ENOMEM;
    if (n_elements > 0 &&
        !dbus_message_iter_append_fixed_array(&sub, (int)type, &ptr, n_elements)) {
        dbus_message_iter_close_container(it, &sub);
        return -ENOMEM;
    }
    if (!dbus_message_iter_close_container(it, &sub)) return -ENOMEM;
    return 0;
}

int sd_bus_message_open_container(sd_bus_message *m, char type, const char *contents)
{
    if (!m) return -EINVAL;
    ensure_wr_init(m);
    flush_pending_str(m);
    if (m->wr_depth >= SBUS_MAX_ITER_DEPTH - 1) return -EOVERFLOW;

    DBusMessageIter *parent = &m->wr_stack[m->wr_depth];
    DBusMessageIter *child  = &m->wr_stack[m->wr_depth + 1];

    /* D-Bus type for struct is DBUS_TYPE_STRUCT ('r') */
    int dbus_type = (type == 'r') ? DBUS_TYPE_STRUCT : (int)type;
    /* Struct and dict_entry use NULL signature in libdbus */
    const char *sig = (dbus_type == DBUS_TYPE_STRUCT ||
                       dbus_type == DBUS_TYPE_DICT_ENTRY) ? NULL : contents;

    if (!dbus_message_iter_open_container(parent, dbus_type, sig, child))
        return -ENOMEM;
    m->wr_depth++;
    return 0;
}

int sd_bus_message_close_container(sd_bus_message *m)
{
    if (!m || m->wr_depth <= 0) return -EINVAL;
    flush_pending_str(m);
    DBusMessageIter *parent = &m->wr_stack[m->wr_depth - 1];
    DBusMessageIter *child  = &m->wr_stack[m->wr_depth];
    if (!dbus_message_iter_close_container(parent, child)) return -EINVAL;
    m->wr_depth--;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message read
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_message_read_basic(sd_bus_message *m, char type, void *p)
{
    if (!m || !p) return -EINVAL;
    ensure_rd_init(m);
    DBusMessageIter *it = &m->rd_stack[m->rd_depth];
    int cur = dbus_message_iter_get_arg_type(it);
    if (cur == DBUS_TYPE_INVALID) return 0; /* end of args */
    if (cur != (int)type) return -ENXIO;
    dbus_message_iter_get_basic(it, p);
    dbus_message_iter_next(it);
    return 1;
}

int sd_bus_message_read_array(sd_bus_message *m, char type,
                              const void **ret_ptr, size_t *ret_size)
{
    if (!m || !ret_ptr || !ret_size) return -EINVAL;
    ensure_rd_init(m);
    DBusMessageIter *it = &m->rd_stack[m->rd_depth];
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) return -ENXIO;

    DBusMessageIter sub;
    dbus_message_iter_recurse(it, &sub);

    int n_elements = 0;
    const void *ptr = NULL;
    dbus_message_iter_get_fixed_array(&sub, &ptr, &n_elements);

    int esz = fixed_type_size(type);
    *ret_ptr  = ptr;
    *ret_size = (esz > 0) ? (size_t)n_elements * (size_t)esz : 0;

    dbus_message_iter_next(it);
    return 1;
}

int sd_bus_message_enter_container(sd_bus_message *m, char type, const char *contents)
{
    (void)contents;
    if (!m) return -EINVAL;
    ensure_rd_init(m);
    if (m->rd_depth >= SBUS_MAX_ITER_DEPTH - 1) return -EOVERFLOW;
    DBusMessageIter *it = &m->rd_stack[m->rd_depth];
    int cur = dbus_message_iter_get_arg_type(it);
    if (cur == DBUS_TYPE_INVALID) return 0;
    int dbus_type = (type == 'r') ? DBUS_TYPE_STRUCT : (int)type;
    if (cur != dbus_type) return -ENXIO;

    dbus_message_iter_recurse(it, &m->rd_stack[m->rd_depth + 1]);
    dbus_message_iter_next(it); /* advance parent past container */
    m->rd_depth++;
    return 1;
}

int sd_bus_message_exit_container(sd_bus_message *m)
{
    if (!m || m->rd_depth <= 0) return -EINVAL;
    m->rd_depth--;
    return 0;
}

int sd_bus_message_peek_type(sd_bus_message *m, char *type, const char **contents)
{
    if (!m) return -EINVAL;
    ensure_rd_init(m);
    DBusMessageIter *it = &m->rd_stack[m->rd_depth];
    int t = dbus_message_iter_get_arg_type(it);
    if (t == DBUS_TYPE_INVALID) {
        if (type)     *type     = 0;
        if (contents) *contents = NULL;
        return 0;
    }
    if (type)     *type = (char)t;
    if (contents) {
        /* For containers, return element signature; for basics, NULL */
        if (t == DBUS_TYPE_ARRAY || t == DBUS_TYPE_VARIANT ||
            t == DBUS_TYPE_STRUCT || t == DBUS_TYPE_DICT_ENTRY) {
            /* libdbus doesn't easily expose the sub-signature from the iterator
             * without recursing; return empty string as safe default */
            *contents = "";
        } else {
            *contents = NULL;
        }
    }
    return 1;
}

int sd_bus_message_at_end(sd_bus_message *m, int complete)
{
    if (!m) return 1;
    ensure_rd_init(m);
    (void)complete;
    DBusMessageIter *it = &m->rd_stack[m->rd_depth];
    return dbus_message_iter_get_arg_type(it) == DBUS_TYPE_INVALID ? 1 : 0;
}

int sd_bus_message_is_empty(sd_bus_message *m)
{
    if (!m) return 1;
    DBusMessageIter it;
    dbus_message_iter_init(m->msg, &it);
    return dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID ? 1 : 0;
}

int sd_bus_message_rewind(sd_bus_message *m, int complete)
{
    if (!m) return -EINVAL;
    if (complete) {
        dbus_message_iter_init(m->msg, &m->rd_stack[0]);
        m->rd_depth = 0;
        m->rd_init  = true;
    }
    return 0;
}

int sd_bus_message_copy(sd_bus_message *dst, sd_bus_message *src, int all)
{
    (void)all;
    if (!dst || !src) return -EINVAL;
    /* Not fully implemented; sufficient for basic use */
    return 0;
}

int sd_bus_message_dump(sd_bus_message *m, FILE *f, uint64_t flags)
{
    (void)flags;
    if (!m || !f) return -EINVAL;
    fprintf(f, "[sd_bus_message type=%d path=%s iface=%s member=%s]\n",
            dbus_message_get_type(m->msg),
            dbus_message_get_path(m->msg)      ? dbus_message_get_path(m->msg)      : "(null)",
            dbus_message_get_interface(m->msg) ? dbus_message_get_interface(m->msg) : "(null)",
            dbus_message_get_member(m->msg)    ? dbus_message_get_member(m->msg)    : "(null)");
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Object / match registration
 * ───────────────────────────────────────────────────────────────────────────*/

static sd_bus_slot *slot_new(sd_bus *bus, SlotType type)
{
    sd_bus_slot *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->bus  = bus;
    s->type = type;
    s->next = bus->slots;
    bus->slots = s;
    return s;
}

int sd_bus_add_object_vtable(sd_bus *bus, sd_bus_slot **slot_out,
                             const char *path, const char *interface,
                             const sd_bus_vtable *vtable, void *userdata)
{
    if (!bus || !path || !interface || !vtable) return -EINVAL;
    sd_bus_slot *s = slot_new(bus, SLOT_VTABLE);
    if (!s) return -ENOMEM;
    s->path      = strdup(path);
    s->interface = strdup(interface);
    s->vtable    = vtable;
    s->userdata  = userdata;
    if (!s->path || !s->interface) { free(s->path); free(s->interface); free(s); return -ENOMEM; }
    if (slot_out) *slot_out = s;
    return 0;
}

int sd_bus_add_object_manager(sd_bus *bus, sd_bus_slot **slot_out, const char *path)
{
    if (!bus || !path) return -EINVAL;
    sd_bus_slot *s = slot_new(bus, SLOT_OBJECT_MANAGER);
    if (!s) return -ENOMEM;
    s->path = strdup(path);
    if (!s->path) { free(s); return -ENOMEM; }
    if (slot_out) *slot_out = s;
    return 0;
}

int sd_bus_add_match(sd_bus *bus, sd_bus_slot **slot_out, const char *match,
                     sd_bus_message_handler_t callback, void *userdata)
{
    if (!bus || !bus->conn) return -EINVAL;
    DBusError derr; dbus_error_init(&derr);
    dbus_bus_add_match(bus->conn, match, &derr);
    dbus_error_free(&derr);
    sd_bus_slot *s = slot_new(bus, SLOT_MATCH);
    if (!s) return -ENOMEM;
    s->match_callback = callback;
    s->match_userdata = userdata;
    if (slot_out) *slot_out = s;
    return 0;
}

int sd_bus_add_match_async(sd_bus *bus, sd_bus_slot **slot_out, const char *match,
                           sd_bus_message_handler_t callback,
                           sd_bus_message_handler_t install_callback, void *userdata)
{
    (void)install_callback;
    return sd_bus_add_match(bus, slot_out, match, callback, userdata);
}

int sd_bus_match_signal(sd_bus *bus, sd_bus_slot **ret, const char *sender,
                        const char *path, const char *interface, const char *member,
                        sd_bus_message_handler_t callback, void *userdata)
{
    if (!bus) return -EINVAL;
    char match[512] = "type='signal'";
    if (sender)    { strncat(match, ",sender='",   sizeof(match)-strlen(match)-1); strncat(match, sender,    sizeof(match)-strlen(match)-1); strncat(match, "'", sizeof(match)-strlen(match)-1); }
    if (path)      { strncat(match, ",path='",     sizeof(match)-strlen(match)-1); strncat(match, path,      sizeof(match)-strlen(match)-1); strncat(match, "'", sizeof(match)-strlen(match)-1); }
    if (interface) { strncat(match, ",interface='",sizeof(match)-strlen(match)-1); strncat(match, interface, sizeof(match)-strlen(match)-1); strncat(match, "'", sizeof(match)-strlen(match)-1); }
    if (member)    { strncat(match, ",member='",   sizeof(match)-strlen(match)-1); strncat(match, member,    sizeof(match)-strlen(match)-1); strncat(match, "'", sizeof(match)-strlen(match)-1); }
    return sd_bus_add_match(bus, ret, match, callback, userdata);
}

sd_bus_slot *sd_bus_slot_unref(sd_bus_slot *slot)
{
    if (!slot) return NULL;
    sd_bus *bus = slot->bus;
    /* Remove from bus linked list */
    sd_bus_slot **pp = &bus->slots;
    while (*pp && *pp != slot) pp = &(*pp)->next;
    if (*pp) *pp = slot->next;
    free(slot->path);
    free(slot->interface);
    free(slot);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Emit helpers (best-effort)
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_emit_properties_changed_strv(sd_bus *bus, const char *path,
                                        const char *interface, char **names)
{
    (void)bus; (void)path; (void)interface; (void)names;
    return 0;
}
int sd_bus_emit_object_added(sd_bus *bus, const char *path)
    { (void)bus; (void)path; return 0; }
int sd_bus_emit_object_removed(sd_bus *bus, const char *path)
    { (void)bus; (void)path; return 0; }
int sd_bus_emit_interfaces_added_strv(sd_bus *bus, const char *path, char **interfaces)
    { (void)bus; (void)path; (void)interfaces; return 0; }
int sd_bus_emit_interfaces_removed_strv(sd_bus *bus, const char *path, char **interfaces)
    { (void)bus; (void)path; (void)interfaces; return 0; }

/* ─────────────────────────────────────────────────────────────────────────────
 * Credentials (stub — libdbus doesn't expose process creds portably)
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_query_sender_creds(sd_bus_message *m, uint64_t mask, sd_bus_creds **ret)
{
    (void)m; (void)mask;
    if (!ret) return -EINVAL;
    sd_bus_creds *c = calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    c->ref_count = 1;
    *ret = c;
    return 0;
}
sd_bus_creds *sd_bus_creds_ref(sd_bus_creds *c)   { if (c) c->ref_count++; return c; }
sd_bus_creds *sd_bus_creds_unref(sd_bus_creds *c) { if (c && --c->ref_count <= 0) free(c); return NULL; }
int sd_bus_creds_get_pid(sd_bus_creds *c, pid_t *p)       { if (!c||!p) return -EINVAL; *p=c->pid; return 0; }
int sd_bus_creds_get_uid(sd_bus_creds *c, uid_t *p)       { if (!c||!p) return -EINVAL; *p=c->uid; return 0; }
int sd_bus_creds_get_euid(sd_bus_creds *c, uid_t *p)      { if (!c||!p) return -EINVAL; *p=c->euid; return 0; }
int sd_bus_creds_get_gid(sd_bus_creds *c, gid_t *p)       { if (!c||!p) return -EINVAL; *p=c->gid; return 0; }
int sd_bus_creds_get_egid(sd_bus_creds *c, uid_t *p)      { if (!c||!p) return -EINVAL; *p=c->egid; return 0; }
int sd_bus_creds_get_supplementary_gids(sd_bus_creds *c, const gid_t **p)
    { (void)c; if (!p) return -EINVAL; *p=NULL; return 0; }
int sd_bus_creds_get_selinux_context(sd_bus_creds *c, const char **p)
    { (void)c; if (!p) return -EINVAL; *p=NULL; return 0; }
