/*
 * sd-bus-libdbus.c — sd-bus API implementation for macOS using libdbus-1.
 */

#include <sdbus-c++/macos/sd-bus-libdbus.h>

#include <dbus/dbus.h>

#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
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
    SLOT_PENDING,
} SlotType;

/* ── Internal struct definitions ─────────────────────────────────────────── */

struct sd_bus_slot {
    SlotType type;
    sd_bus  *bus;       /* strong reference (released in sd_bus_slot_unref) */

    /* SLOT_VTABLE / SLOT_OBJECT_MANAGER */
    char              *path;
    char              *interface;
    const sd_bus_vtable *vtable;
    void              *userdata;

    /* SLOT_MATCH */
    sd_bus_message_handler_t match_callback;
    void                    *match_userdata;
    char *match_rule;     /* full rule, for dbus_bus_remove_match on unref */
    char *m_type;         /* parsed rule criteria for client-side filtering */
    char *m_sender;
    char *m_iface;
    char *m_member;
    char *m_path;

    /* SLOT_PENDING (async method call) */
    DBusPendingCall          *pending;
    sd_bus_message_handler_t  pending_callback;
    void                     *pending_userdata;
    bool                      pending_done;

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
    sd_bus      *bus;   /* strong reference (sd_bus_send(NULL, m, ..) resolves the bus from here) */
    int          ref_count;

    /* Write iterator stack */
    DBusMessageIter wr_stack[SBUS_MAX_ITER_DEPTH];
    int             wr_depth;        /* current write level */
    bool            wr_init;         /* wr_stack[0] initialised */

    /* Read iterator stack; rd_start[d] snapshots the iterator at the start of
     * container level d, so sd_bus_message_rewind(complete=false) can reset
     * the current container's read position. */
    DBusMessageIter rd_stack[SBUS_MAX_ITER_DEPTH];
    DBusMessageIter rd_start[SBUS_MAX_ITER_DEPTH];
    int             rd_depth;
    bool            rd_init;

    /* Deferred string for append_string_space */
    char  *pending_str;
    size_t pending_str_len;

    /* Lazily parsed error (for sd_bus_message_get_error) */
    sd_bus_error parsed_error;
    bool         error_parsed;

    /* Cached container-contents signature returned by peek_type */
    char *peek_sig;

    /* Set after sd_bus_send() */
    bool reply_sent;
};

struct sd_bus {
    DBusConnection *conn;

    /* Pre-start config */
    char *pending_address;
    bool  is_bus_client;

    /* Slots (vtables + matches + pending calls) */
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

static sd_bus *bus_ref(sd_bus *bus)
{
    if (bus) bus->ref_count++;
    return bus;
}

static sd_bus_message *msg_alloc(sd_bus *bus, DBusMessage *dbus_msg)
{
    sd_bus_message *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->msg       = dbus_message_ref(dbus_msg);
    m->bus       = bus_ref(bus);
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
        m->rd_start[0] = m->rd_stack[0];
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

/* Map a D-Bus error name to a negative errno (sd-bus convention). */
static int errno_from_dbus_error(const char *name)
{
    if (!name) return -EIO;
    if (strcmp(name, DBUS_ERROR_NO_MEMORY) == 0)            return -ENOMEM;
    if (strcmp(name, DBUS_ERROR_SERVICE_UNKNOWN) == 0 ||
        strcmp(name, DBUS_ERROR_NAME_HAS_NO_OWNER) == 0)    return -EHOSTUNREACH;
    if (strcmp(name, DBUS_ERROR_NO_REPLY) == 0 ||
        strcmp(name, DBUS_ERROR_TIMEOUT) == 0 ||
        strcmp(name, DBUS_ERROR_TIMED_OUT) == 0)            return -ETIMEDOUT;
    if (strcmp(name, DBUS_ERROR_ACCESS_DENIED) == 0 ||
        strcmp(name, DBUS_ERROR_AUTH_FAILED) == 0)          return -EACCES;
    if (strcmp(name, DBUS_ERROR_INVALID_ARGS) == 0)         return -EINVAL;
    if (strcmp(name, DBUS_ERROR_UNKNOWN_METHOD) == 0)       return -ENXIO;
    if (strcmp(name, DBUS_ERROR_DISCONNECTED) == 0)         return -ECONNRESET;
    if (strcmp(name, DBUS_ERROR_LIMITS_EXCEEDED) == 0)      return -ENOBUFS;
    return -EIO;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Generic value copy between two messages (powers sd_bus_message_copy,
 * which sdbus-c++ uses for the whole Variant machinery)
 * ───────────────────────────────────────────────────────────────────────────*/

static int copy_values(DBusMessageIter *rd, DBusMessageIter *wr, bool all);

/* Copy the single complete value rd points at into wr (does not advance rd).
 * Returns 1 if a value was copied, 0 at end of input, negative errno on error. */
static int copy_one_value(DBusMessageIter *rd, DBusMessageIter *wr)
{
    int type = dbus_message_iter_get_arg_type(rd);
    if (type == DBUS_TYPE_INVALID) return 0;

    if (dbus_type_is_basic(type)) {
        DBusBasicValue v;
        dbus_message_iter_get_basic(rd, &v);
        if (!dbus_message_iter_append_basic(wr, type, &v)) return -ENOMEM;
        return 1;
    }

    /* Container: recurse. Arrays and variants need the contents signature. */
    DBusMessageIter sub_rd, sub_wr;
    dbus_message_iter_recurse(rd, &sub_rd);

    char *content_sig = NULL;
    const char *sig = NULL;
    if (type == DBUS_TYPE_ARRAY || type == DBUS_TYPE_VARIANT) {
        content_sig = dbus_message_iter_get_signature(&sub_rd);
        if (!content_sig) return -ENOMEM;
        sig = content_sig;
    }
    dbus_bool_t opened = dbus_message_iter_open_container(wr, type, sig, &sub_wr);
    if (content_sig) dbus_free(content_sig);
    if (!opened) return -ENOMEM;

    int r = copy_values(&sub_rd, &sub_wr, true);
    if (r < 0) {
        dbus_message_iter_abandon_container(wr, &sub_wr);
        return r;
    }
    if (!dbus_message_iter_close_container(wr, &sub_wr)) return -ENOMEM;
    return 1;
}

/* Copy one (all=false) or all remaining (all=true) complete values,
 * advancing rd past everything copied. */
static int copy_values(DBusMessageIter *rd, DBusMessageIter *wr, bool all)
{
    int copied = 0;
    for (;;) {
        int r = copy_one_value(rd, wr);
        if (r < 0) return r;
        if (r == 0) return copied ? 1 : 0;
        copied = 1;
        dbus_message_iter_next(rd);
        if (!all) return 1;
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

/* Return a pointer just past the single complete D-Bus type starting at p. */
static const char *sig_skip_one(const char *p)
{
    switch (*p) {
    case '\0':
        return p;
    case 'a':
        return sig_skip_one(p + 1);
    case '(':
        p++;
        while (*p && *p != ')') p = sig_skip_one(p);
        return *p ? p + 1 : p;
    case '{':
        p++;
        while (*p && *p != '}') p = sig_skip_one(p);
        return *p ? p + 1 : p;
    default:
        return p + 1;
    }
}

/* Walk a signature string and emit <arg> elements. direction: "in" or "out". */
static int emit_args(char **buf, size_t *cap, size_t *len,
                     const char *sig, const char *direction)
{
    if (!sig || !*sig) return 0;
    /* Each top-level complete type becomes one arg */
    const char *p = sig;
    while (*p) {
        const char *end = sig_skip_one(p);
        if (buf_appendf(buf, cap, len,
                "      <arg direction=\"%s\" type=\"%.*s\"/>\n",
                direction, (int)(end - p), p) < 0)
            return -ENOMEM;
        p = end;
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
    bool have_properties = false;
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
                have_properties = true;
            }
        }
        buf_appendf(&buf, &cap, &len, "  </interface>\n");
    }

    /* Standard interfaces */
    buf_appendf(&buf, &cap, &len,
        "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
        "    <method name=\"Introspect\">\n"
        "      <arg direction=\"out\" type=\"s\"/>\n"
        "    </method>\n"
        "  </interface>\n");
    if (have_properties)
        buf_appendf(&buf, &cap, &len,
            "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
            "    <method name=\"Get\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"out\" type=\"v\"/>\n"
            "    </method>\n"
            "    <method name=\"GetAll\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"out\" type=\"a{sv}\"/>\n"
            "    </method>\n"
            "    <method name=\"Set\">\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"s\"/>\n"
            "      <arg direction=\"in\" type=\"v\"/>\n"
            "    </method>\n"
            "    <signal name=\"PropertiesChanged\">\n"
            "      <arg type=\"s\"/>\n"
            "      <arg type=\"a{sv}\"/>\n"
            "      <arg type=\"as\"/>\n"
            "    </signal>\n"
            "  </interface>\n");
    buf_appendf(&buf, &cap, &len, "</node>\n");

    return buf;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Error reply helpers
 * ───────────────────────────────────────────────────────────────────────────*/

static void send_dbus_error(DBusConnection *conn, DBusMessage *req,
                            const char *name, const char *text)
{
    DBusMessage *er = dbus_message_new_error(req, name, text ? text : "");
    if (er) { dbus_connection_send(conn, er, NULL); dbus_message_unref(er); }
}

static void send_handler_error(DBusConnection *conn, DBusMessage *req,
                               const sd_bus_error *err)
{
    const char *name = (err && err->name && *err->name)
                       ? err->name : "org.freedesktop.DBus.Error.Failed";
    const char *text = (err && err->message) ? err->message : "Handler error";
    send_dbus_error(conn, req, name, text);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Property lookup & dispatch (org.freedesktop.DBus.Properties)
 * ───────────────────────────────────────────────────────────────────────────*/

static const sd_bus_vtable *find_property_vt(sd_bus *bus, const char *path,
                                             const char *iface, const char *prop,
                                             sd_bus_slot **slot_out)
{
    for (sd_bus_slot *s = bus->slots; s; s = s->next) {
        if (s->type != SLOT_VTABLE) continue;
        if (strcmp(s->path, path) != 0) continue;
        if (iface && *iface && strcmp(s->interface, iface) != 0) continue;
        for (const sd_bus_vtable *vt = s->vtable;
             vt->type != _SD_BUS_VTABLE_END; vt++) {
            if (vt->type != _SD_BUS_VTABLE_PROPERTY &&
                vt->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY) continue;
            if (strcmp(vt->x.property.member, prop) != 0) continue;
            if (slot_out) *slot_out = s;
            return vt;
        }
    }
    return NULL;
}

/* Append "v" containing the current value of the given property. */
static int append_property_variant(sd_bus *bus, sd_bus_message *m,
                                   sd_bus_slot *s, const sd_bus_vtable *vt)
{
    int r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT,
                                          vt->x.property.signature);
    if (r < 0) return r;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = vt->x.property.get(bus, s->path, s->interface, vt->x.property.member,
                           m, s->userdata, &err);
    bool failed = (r < 0 || sd_bus_error_is_set(&err));
    sd_bus_error_free(&err);
    if (failed) return r < 0 ? r : -EIO;
    return sd_bus_message_close_container(m);
}

static DBusHandlerResult handle_prop_get(sd_bus *bus, DBusConnection *conn,
                                         DBusMessage *req)
{
    const char *iface = NULL, *prop = NULL;
    if (!dbus_message_get_args(req, NULL, DBUS_TYPE_STRING, &iface,
                               DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
        send_dbus_error(conn, req, DBUS_ERROR_INVALID_ARGS, "Expected (ss)");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    const char *path = dbus_message_get_path(req);
    sd_bus_slot *slot = NULL;
    const sd_bus_vtable *vt = find_property_vt(bus, path, iface, prop, &slot);
    if (!vt || !vt->x.property.get) {
        send_dbus_error(conn, req, DBUS_ERROR_UNKNOWN_PROPERTY, prop);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    DBusMessage *dr = dbus_message_new_method_return(req);
    if (!dr) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    sd_bus_message *reply = msg_alloc(bus, dr);
    dbus_message_unref(dr);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    int r = append_property_variant(bus, reply, slot, vt);
    if (r < 0) {
        send_dbus_error(conn, req, DBUS_ERROR_FAILED, "Property getter failed");
    } else {
        flush_pending_str(reply);
        dbus_connection_send(conn, reply->msg, NULL);
    }
    sd_bus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_prop_getall(sd_bus *bus, DBusConnection *conn,
                                            DBusMessage *req)
{
    const char *iface = NULL;
    if (!dbus_message_get_args(req, NULL, DBUS_TYPE_STRING, &iface,
                               DBUS_TYPE_INVALID)) {
        send_dbus_error(conn, req, DBUS_ERROR_INVALID_ARGS, "Expected (s)");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    const char *path = dbus_message_get_path(req);

    DBusMessage *dr = dbus_message_new_method_return(req);
    if (!dr) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    sd_bus_message *reply = msg_alloc(bus, dr);
    dbus_message_unref(dr);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    int r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
    for (sd_bus_slot *s = bus->slots; s && r >= 0; s = s->next) {
        if (s->type != SLOT_VTABLE) continue;
        if (strcmp(s->path, path) != 0) continue;
        if (iface && *iface && strcmp(s->interface, iface) != 0) continue;
        for (const sd_bus_vtable *vt = s->vtable;
             vt->type != _SD_BUS_VTABLE_END && r >= 0; vt++) {
            if ((vt->type != _SD_BUS_VTABLE_PROPERTY &&
                 vt->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY) ||
                !vt->x.property.get) continue;
            r = sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv");
            if (r >= 0) r = sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING,
                                                        vt->x.property.member);
            if (r >= 0) r = append_property_variant(bus, reply, s, vt);
            if (r >= 0) r = sd_bus_message_close_container(reply);
        }
    }
    if (r >= 0) r = sd_bus_message_close_container(reply);

    if (r < 0)
        send_dbus_error(conn, req, DBUS_ERROR_FAILED, "GetAll failed");
    else
        dbus_connection_send(conn, reply->msg, NULL);
    sd_bus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_prop_set(sd_bus *bus, DBusConnection *conn,
                                         DBusMessage *req)
{
    const char *path = dbus_message_get_path(req);
    sd_bus_message *m = msg_alloc(bus, req);
    if (!m) return DBUS_HANDLER_RESULT_NEED_MEMORY;

    const char *iface = NULL, *prop = NULL;
    int r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &iface);
    if (r > 0) r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &prop);
    if (r <= 0) {
        send_dbus_error(conn, req, DBUS_ERROR_INVALID_ARGS, "Expected (ssv)");
        sd_bus_message_unref(m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    sd_bus_slot *slot = NULL;
    const sd_bus_vtable *vt = find_property_vt(bus, path, iface, prop, &slot);
    if (!vt) {
        send_dbus_error(conn, req, DBUS_ERROR_UNKNOWN_PROPERTY, prop);
        sd_bus_message_unref(m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (vt->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY || !vt->x.property.set) {
        send_dbus_error(conn, req, DBUS_ERROR_PROPERTY_READ_ONLY, prop);
        sd_bus_message_unref(m);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, NULL);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (r > 0)
        r = vt->x.property.set(bus, path, slot->interface, prop, m,
                               slot->userdata, &err);
    if (r < 0 || sd_bus_error_is_set(&err)) {
        send_handler_error(conn, req, &err);
    } else {
        DBusMessage *dr = dbus_message_new_method_return(req);
        if (dr) { dbus_connection_send(conn, dr, NULL); dbus_message_unref(dr); }
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Match rule parsing & client-side signal filtering
 * ───────────────────────────────────────────────────────────────────────────*/

/* Extract the value of "key='value'" (or key=value) from a match rule. */
static char *match_value_dup(const char *rule, const char *key)
{
    size_t klen = strlen(key);
    const char *p = rule;
    while ((p = strstr(p, key)) != NULL) {
        bool at_boundary = (p == rule || p[-1] == ',' || p[-1] == ' ');
        if (at_boundary && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e;
            if (*v == '\'') {
                v++;
                e = strchr(v, '\'');
                if (!e) return NULL;
            } else {
                e = strchr(v, ',');
                if (!e) e = v + strlen(v);
            }
            return strndup(v, (size_t)(e - v));
        }
        p += klen;
    }
    return NULL;
}

/* Client-side check of a parsed match rule against a signal message.
 *
 * Senders given as well-known names are not compared literally (the message
 * carries the unique name): we rely on the daemon-side rule — which includes
 * the sender — having filtered the message already. argN matches are not
 * implemented client-side for the same reason. This can over-deliver across
 * multiple registered matches but never silently drops a matching signal. */
static bool slot_matches_signal(sd_bus_slot *s, DBusMessage *msg)
{
    const char *v;
    if (s->m_type && strcmp(s->m_type, "signal") != 0) return false;
    if (s->m_path) {
        v = dbus_message_get_path(msg);
        if (!v || strcmp(s->m_path, v) != 0) return false;
    }
    if (s->m_iface) {
        v = dbus_message_get_interface(msg);
        if (!v || strcmp(s->m_iface, v) != 0) return false;
    }
    if (s->m_member) {
        v = dbus_message_get_member(msg);
        if (!v || strcmp(s->m_member, v) != 0) return false;
    }
    if (s->m_sender && s->m_sender[0] == ':') {
        v = dbus_message_get_sender(msg);
        if (!v || strcmp(s->m_sender, v) != 0) return false;
    }
    return true;
}

/* Connection-wide filter: fans incoming signals out to matching match slots. */
static DBusHandlerResult signal_filter(DBusConnection *conn,
                                       DBusMessage    *dbus_msg,
                                       void           *user_data)
{
    (void)conn;
    sd_bus *bus = (sd_bus *)user_data;
    if (dbus_message_get_type(dbus_msg) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    sd_bus_slot *s = bus->slots;
    while (s) {
        sd_bus_slot *next = s->next; /* the callback may unref this very slot */
        if (s->type == SLOT_MATCH && s->match_callback &&
            slot_matches_signal(s, dbus_msg)) {
            sd_bus_message *m = msg_alloc(bus, dbus_msg);
            if (!m) return DBUS_HANDLER_RESULT_NEED_MEMORY;
            bus->current_message = m;
            sd_bus_error err = SD_BUS_ERROR_NULL;
            s->match_callback(m, s->match_userdata, &err);
            sd_bus_error_free(&err);
            bus->current_message = NULL;
            sd_bus_message_unref(m);
        }
        s = next;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
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

    /* ── org.freedesktop.DBus.Properties ── */
    if (iface && strcmp(iface, "org.freedesktop.DBus.Properties") == 0 && member) {
        if (strcmp(member, "Get") == 0)    return handle_prop_get(bus, conn, dbus_msg);
        if (strcmp(member, "GetAll") == 0) return handle_prop_getall(bus, conn, dbus_msg);
        if (strcmp(member, "Set") == 0)    return handle_prop_set(bus, conn, dbus_msg);
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

/* Register the fallback handler and the signal filter once per connection. */
static int setup_connection(sd_bus *bus)
{
    DBusError derr;
    dbus_error_init(&derr);
    dbus_connection_try_register_fallback(
        bus->conn, "/", &g_fallback_vtable, bus, &derr);
    /* Ignore "already registered" error */
    dbus_error_free(&derr);

    if (!dbus_connection_add_filter(bus->conn, signal_filter, bus, NULL))
        return -ENOMEM;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Bus open / lifecycle
 * ───────────────────────────────────────────────────────────────────────────*/

static void init_threads(void)
{
    dbus_threads_init_default();
}

static sd_bus *bus_alloc(void)
{
    /* Enable libdbus thread-safety exactly once, race-free */
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_threads);

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
    setup_connection(b);
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
    setup_connection(bus);
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
    /* No slots can exist here: every slot holds a bus reference, so the
     * refcount cannot drop to zero while any slot is alive. */
    assert(bus->slots == NULL);
    sd_bus_close(bus);
    if (bus->conn) {
        dbus_connection_remove_filter(bus->conn, signal_filter, bus);
        dbus_connection_unref(bus->conn);
        bus->conn = NULL;
    }
    free(bus->pending_address);
    free(bus->unique_name);
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
    unsigned int dflags = 0;
    if (flags & SD_BUS_NAME_ALLOW_REPLACEMENT) dflags |= DBUS_NAME_FLAG_ALLOW_REPLACEMENT;
    if (flags & SD_BUS_NAME_REPLACE_EXISTING)  dflags |= DBUS_NAME_FLAG_REPLACE_EXISTING;
    if (!(flags & SD_BUS_NAME_QUEUE))          dflags |= DBUS_NAME_FLAG_DO_NOT_QUEUE;

    DBusError derr; dbus_error_init(&derr);
    int r = dbus_bus_request_name(bus->conn, name, dflags, &derr);
    if (r < 0) {
        int e = errno_from_dbus_error(derr.name);
        dbus_error_free(&derr);
        return e;
    }
    dbus_error_free(&derr);
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
    if (st == DBUS_DISPATCH_DATA_REMAINS) return 1;
    /* Report disconnection (after draining queued data) so the event loop
     * exits instead of busy-looping on a permanently readable EOF socket. */
    if (!dbus_connection_get_is_connected(bus->conn)) return -ECONNRESET;
    return 0;
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
    if (!bus->conn) return 0;
    int events = POLLIN;
    if (dbus_connection_has_messages_to_send(bus->conn))
        events |= POLLOUT;
    return events;
}

int sd_bus_get_timeout(sd_bus *bus, uint64_t *timeout_usec)
{
    if (!bus || !timeout_usec) return -EINVAL;
    if (!bus->conn) { *timeout_usec = UINT64_MAX; return 0; }
    DBusDispatchStatus st = dbus_connection_get_dispatch_status(bus->conn);
    bool ready = (st == DBUS_DISPATCH_DATA_REMAINS) ||
                 dbus_connection_has_messages_to_send(bus->conn);
    *timeout_usec = ready ? 0 : UINT64_MAX;
    return 0;
}

int sd_bus_get_n_queued_read(sd_bus *bus, uint64_t *ret)
{
    if (!bus || !ret) return -EINVAL;
    if (!bus->conn) { *ret = 0; return 0; }
    DBusDispatchStatus st = dbus_connection_get_dispatch_status(bus->conn);
    *ret = (st == DBUS_DISPATCH_DATA_REMAINS) ? 1 : 0;
    return 0;
}

int sd_bus_get_n_queued_write(sd_bus *bus, uint64_t *ret)
{
    if (!bus || !ret) return -EINVAL;
    if (!bus->conn) { *ret = 0; return 0; }
    *ret = dbus_connection_has_messages_to_send(bus->conn) ? 1 : 0;
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
    free(m->peek_sig);
    sd_bus_error_free(&m->parsed_error);
    dbus_message_unref(m->msg);
    sd_bus_unref(m->bus);
    free(m);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message creation
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_message_new(sd_bus *bus, sd_bus_message **ret, uint8_t type)
{
    if (!bus || !ret) return -EINVAL;
    /* sdbus-c++ passes _SD_BUS_MESSAGE_TYPE_INVALID (0) to create plain
     * messages used as local data containers (e.g. for Variant). libdbus
     * refuses to create INVALID-typed messages, so use METHOD_CALL as the
     * underlying container type — the message is never sent. */
    int dtype = (type == _SD_BUS_MESSAGE_TYPE_INVALID)
                ? DBUS_MESSAGE_TYPE_METHOD_CALL : (int)type;
    DBusMessage *dm = dbus_message_new(dtype);
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
    if (!m || dbus_message_get_type(m->msg) != DBUS_MESSAGE_TYPE_ERROR)
        return NULL;
    if (!m->error_parsed) {
        const char *name = dbus_message_get_error_name(m->msg);
        const char *text = NULL;
        DBusMessageIter it;
        if (dbus_message_iter_init(m->msg, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&it, &text);
        sd_bus_error_set(&m->parsed_error,
                         name ? name : "org.freedesktop.DBus.Error.Failed", text);
        m->error_parsed = true;
    }
    return &m->parsed_error;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Message send / call
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_send(sd_bus *bus, sd_bus_message *m, uint64_t *cookie)
{
    if (!m) return -EINVAL;
    if (!bus) bus = m->bus;          /* sdbus-c++ passes NULL; fall back to the message's own bus */
    if (!bus || !bus->conn) return -EINVAL;
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
    if (!m) return -EINVAL;
    if (!bus) bus = m->bus;          /* sdbus-c++ passes NULL; fall back to the message's own bus */
    if (!bus || !bus->conn) return -EINVAL;
    flush_pending_str(m);

    int timeout_ms = usec ? (int)(usec / 1000) : -1;
    DBusError derr; dbus_error_init(&derr);
    DBusMessage *dr = dbus_connection_send_with_reply_and_block(
        bus->conn, m->msg, timeout_ms, &derr);
    if (!dr) {
        int e = errno_from_dbus_error(derr.name);
        if (ret_error)
            sd_bus_error_set(ret_error, derr.name, derr.message);
        dbus_error_free(&derr);
        return e;
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

/* Forward declaration (slot helpers live further down) */
static sd_bus_slot *slot_new(sd_bus *bus, SlotType type);

static void pending_notify(DBusPendingCall *pc, void *user_data)
{
    sd_bus_slot *slot = (sd_bus_slot *)user_data;
    sd_bus *bus = slot->bus;

    slot->pending_done = true;
    sd_bus_message_handler_t cb = slot->pending_callback;
    void *ud = slot->pending_userdata;

    DBusMessage *reply = dbus_pending_call_steal_reply(pc);
    sd_bus_message *m = reply ? msg_alloc(bus, reply) : NULL;
    if (reply) dbus_message_unref(reply);

    if (m && cb) {
        bus->current_message = m;
        sd_bus_error err = SD_BUS_ERROR_NULL;
        cb(m, ud, &err);     /* may re-entrantly sd_bus_slot_unref(slot) */
        sd_bus_error_free(&err);
        bus->current_message = NULL;
    }
    if (m) sd_bus_message_unref(m);
    /* Do not touch `slot` here: the callback may have freed it. */
}

int sd_bus_call_async(sd_bus *bus, sd_bus_slot **slot_out, sd_bus_message *m,
                      sd_bus_message_handler_t callback, void *userdata,
                      uint64_t usec)
{
    if (!m) return -EINVAL;
    if (!bus) bus = m->bus;
    if (!bus || !bus->conn) return -EINVAL;
    flush_pending_str(m);

    if (!callback) {
        if (slot_out) *slot_out = NULL;
        return sd_bus_send(bus, m, NULL);
    }

    uint64_t t = usec ? usec
                      : (bus->call_timeout_usec ? bus->call_timeout_usec : 25000000ULL);
    uint64_t t_ms = t / 1000ULL;
    int timeout_ms = (t_ms > (uint64_t)INT_MAX) ? INT_MAX : (int)t_ms;

    DBusPendingCall *pc = NULL;
    if (!dbus_connection_send_with_reply(bus->conn, m->msg, &pc, timeout_ms))
        return -ENOMEM;
    if (!pc) return -ECONNRESET; /* connection is disconnected */
    m->reply_sent = true;

    sd_bus_slot *s = slot_new(bus, SLOT_PENDING);
    if (!s) {
        dbus_pending_call_cancel(pc);
        dbus_pending_call_unref(pc);
        return -ENOMEM;
    }
    s->pending          = pc;
    s->pending_callback = callback;
    s->pending_userdata = userdata;

    if (!dbus_pending_call_set_notify(pc, pending_notify, s, NULL)) {
        sd_bus_slot_unref(s);
        return -ENOMEM;
    }
    /* If the reply raced in before set_notify, libdbus won't fire the notify
     * itself — deliver it now. */
    if (dbus_pending_call_get_completed(pc) && !s->pending_done)
        pending_notify(pc, s);

    if (slot_out) *slot_out = s;
    return 0;
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
    /* UNIX_FD not supported in basic libdbus portable path.
     *
     * sd-bus convention: for string types ('s', 'o', 'g'), the caller passes
     * const char* directly as p (e.g. sd_bus_message_append_basic(m, 's', str)).
     * libdbus convention: dbus_message_iter_append_basic expects const char** for
     * string types — it dereferences p to obtain the char pointer. Pass &p so that
     * *((const char **)&p) == p (the original const char *) is what libdbus reads. */
    if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH || type == DBUS_TYPE_SIGNATURE) {
        if (!dbus_message_iter_append_basic(it, (int)type, &p)) return -ENOMEM;
    } else {
        if (!dbus_message_iter_append_basic(it, (int)type, p)) return -ENOMEM;
    }
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
    m->rd_start[m->rd_depth + 1] = m->rd_stack[m->rd_depth + 1];
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
    if (type) *type = (char)t;
    if (contents) {
        if (t == DBUS_TYPE_ARRAY || t == DBUS_TYPE_VARIANT ||
            t == DBUS_TYPE_STRUCT || t == DBUS_TYPE_DICT_ENTRY) {
            /* Recurse to obtain the contents signature (signature-driven in
             * libdbus, so this also works for empty arrays). */
            DBusMessageIter sub;
            dbus_message_iter_recurse(it, &sub);
            char *sig = dbus_message_iter_get_signature(&sub);
            if (!sig) return -ENOMEM;
            char *copy = strdup(sig);
            dbus_free(sig);
            if (!copy) return -ENOMEM;
            free(m->peek_sig);
            m->peek_sig = copy;
            *contents = m->peek_sig;
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
        /* Reset the read pointer to the beginning of the message */
        dbus_message_iter_init(m->msg, &m->rd_stack[0]);
        m->rd_start[0] = m->rd_stack[0];
        m->rd_depth = 0;
        m->rd_init  = true;
    } else {
        /* Reset only the current container's read pointer to its start */
        ensure_rd_init(m);
        m->rd_stack[m->rd_depth] = m->rd_start[m->rd_depth];
    }
    return 0;
}

int sd_bus_message_copy(sd_bus_message *dst, sd_bus_message *src, int all)
{
    if (!dst || !src) return -EINVAL;
    ensure_rd_init(src);
    ensure_wr_init(dst);
    int r = flush_pending_str(dst);
    if (r < 0) return r;
    return copy_values(&src->rd_stack[src->rd_depth],
                       &dst->wr_stack[dst->wr_depth], all != 0);
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
    s->bus  = bus_ref(bus);
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
    if (!s->path || !s->interface) { sd_bus_slot_unref(s); return -ENOMEM; }
    if (slot_out) *slot_out = s;
    return 0;
}

int sd_bus_add_object_manager(sd_bus *bus, sd_bus_slot **slot_out, const char *path)
{
    if (!bus || !path) return -EINVAL;
    sd_bus_slot *s = slot_new(bus, SLOT_OBJECT_MANAGER);
    if (!s) return -ENOMEM;
    s->path = strdup(path);
    if (!s->path) { sd_bus_slot_unref(s); return -ENOMEM; }
    if (slot_out) *slot_out = s;
    return 0;
}

int sd_bus_add_match(sd_bus *bus, sd_bus_slot **slot_out, const char *match,
                     sd_bus_message_handler_t callback, void *userdata)
{
    if (!bus || !bus->conn || !match) return -EINVAL;

    sd_bus_slot *s = slot_new(bus, SLOT_MATCH);
    if (!s) return -ENOMEM;
    s->match_callback = callback;
    s->match_userdata = userdata;
    s->match_rule = strdup(match);
    if (!s->match_rule) { sd_bus_slot_unref(s); return -ENOMEM; }
    /* Parse the criteria we filter on client-side */
    s->m_type   = match_value_dup(match, "type");
    s->m_sender = match_value_dup(match, "sender");
    s->m_iface  = match_value_dup(match, "interface");
    s->m_member = match_value_dup(match, "member");
    s->m_path   = match_value_dup(match, "path");

    DBusError derr; dbus_error_init(&derr);
    dbus_bus_add_match(bus->conn, match, &derr);
    if (dbus_error_is_set(&derr)) {
        int e = errno_from_dbus_error(derr.name);
        dbus_error_free(&derr);
        free(s->match_rule);
        s->match_rule = NULL; /* nothing to remove from the daemon */
        sd_bus_slot_unref(s);
        return e;
    }
    dbus_error_free(&derr);

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

    if (slot->type == SLOT_MATCH && slot->match_rule && bus->conn &&
        dbus_connection_get_is_connected(bus->conn))
        dbus_bus_remove_match(bus->conn, slot->match_rule, NULL);

    if (slot->type == SLOT_PENDING && slot->pending) {
        if (!slot->pending_done)
            dbus_pending_call_cancel(slot->pending);
        dbus_pending_call_unref(slot->pending);
    }

    free(slot->path);
    free(slot->interface);
    free(slot->match_rule);
    free(slot->m_type);
    free(slot->m_sender);
    free(slot->m_iface);
    free(slot->m_member);
    free(slot->m_path);
    free(slot);
    sd_bus_unref(bus);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Emit helpers
 * ───────────────────────────────────────────────────────────────────────────*/

int sd_bus_emit_properties_changed_strv(sd_bus *bus, const char *path,
                                        const char *interface, char **names)
{
    if (!bus || !bus->conn || !path || !interface) return -EINVAL;

    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_signal(bus, &m, path,
                                      "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged");
    if (r < 0) return r;

    r = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING, interface);
    if (r >= 0) r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r >= 0) {
        if (names) {
            for (char **n = names; *n && r >= 0; n++) {
                sd_bus_slot *s = NULL;
                const sd_bus_vtable *vt = find_property_vt(bus, path, interface, *n, &s);
                if (!vt || !vt->x.property.get) { r = -ENOENT; break; }
                r = sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv");
                if (r >= 0) r = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING, *n);
                if (r >= 0) r = append_property_variant(bus, m, s, vt);
                if (r >= 0) r = sd_bus_message_close_container(m);
            }
        } else {
            /* NULL strv: emit current values of all properties of the interface */
            for (sd_bus_slot *s = bus->slots; s && r >= 0; s = s->next) {
                if (s->type != SLOT_VTABLE) continue;
                if (strcmp(s->path, path) != 0) continue;
                if (strcmp(s->interface, interface) != 0) continue;
                for (const sd_bus_vtable *vt = s->vtable;
                     vt->type != _SD_BUS_VTABLE_END && r >= 0; vt++) {
                    if ((vt->type != _SD_BUS_VTABLE_PROPERTY &&
                         vt->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY) ||
                        !vt->x.property.get) continue;
                    r = sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv");
                    if (r >= 0) r = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING,
                                                                vt->x.property.member);
                    if (r >= 0) r = append_property_variant(bus, m, s, vt);
                    if (r >= 0) r = sd_bus_message_close_container(m);
                }
            }
        }
    }
    if (r >= 0) r = sd_bus_message_close_container(m);                     /* a{sv} */
    if (r >= 0) r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "s"); /* invalidated */
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_send(bus, m, NULL);
    sd_bus_message_unref(m);
    return r < 0 ? r : 0;
}

/* Find the object manager whose path is `path` or an ancestor of it. */
static sd_bus_slot *find_object_manager(sd_bus *bus, const char *path)
{
    sd_bus_slot *best = NULL;
    size_t best_len = 0;
    for (sd_bus_slot *s = bus->slots; s; s = s->next) {
        if (s->type != SLOT_OBJECT_MANAGER) continue;
        size_t len = strlen(s->path);
        bool is_root = (strcmp(s->path, "/") == 0);
        if (strcmp(s->path, path) == 0 ||
            is_root ||
            (strncmp(s->path, path, len) == 0 && path[len] == '/')) {
            if (!best || len > best_len) { best = s; best_len = len; }
        }
    }
    return best;
}

/* Append "a{sv}" with all properties of the given vtable slot. */
static int append_all_properties(sd_bus *bus, sd_bus_message *m, sd_bus_slot *s)
{
    int r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    for (const sd_bus_vtable *vt = s->vtable;
         vt->type != _SD_BUS_VTABLE_END && r >= 0; vt++) {
        if ((vt->type != _SD_BUS_VTABLE_PROPERTY &&
             vt->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY) ||
            !vt->x.property.get) continue;
        r = sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv");
        if (r >= 0) r = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING,
                                                    vt->x.property.member);
        if (r >= 0) r = append_property_variant(bus, m, s, vt);
        if (r >= 0) r = sd_bus_message_close_container(m);
    }
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int sd_bus_emit_interfaces_added_strv(sd_bus *bus, const char *path, char **interfaces)
{
    if (!bus || !bus->conn || !path) return -EINVAL;
    sd_bus_slot *mgr = find_object_manager(bus, path);
    if (!mgr) return -ENOENT;

    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_signal(bus, &m, mgr->path,
                                      "org.freedesktop.DBus.ObjectManager",
                                      "InterfacesAdded");
    if (r < 0) return r;

    r = sd_bus_message_append_basic(m, SD_BUS_TYPE_OBJECT_PATH, path);
    if (r >= 0) r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sa{sv}}");
    if (r >= 0 && interfaces) {
        for (char **i = interfaces; *i && r >= 0; i++) {
            r = sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}");
            if (r >= 0) r = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING, *i);
            if (r >= 0) {
                /* Find the vtable for this interface to enumerate properties */
                sd_bus_slot *vs = NULL;
                for (sd_bus_slot *s = bus->slots; s; s = s->next)
                    if (s->type == SLOT_VTABLE && strcmp(s->path, path) == 0 &&
                        strcmp(s->interface, *i) == 0) { vs = s; break; }
                if (vs) {
                    r = append_all_properties(bus, m, vs);
                } else {
                    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
                    if (r >= 0) r = sd_bus_message_close_container(m);
                }
            }
            if (r >= 0) r = sd_bus_message_close_container(m);
        }
    }
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_send(bus, m, NULL);
    sd_bus_message_unref(m);
    return r < 0 ? r : 0;
}

int sd_bus_emit_interfaces_removed_strv(sd_bus *bus, const char *path, char **interfaces)
{
    if (!bus || !bus->conn || !path) return -EINVAL;
    sd_bus_slot *mgr = find_object_manager(bus, path);
    if (!mgr) return -ENOENT;

    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_signal(bus, &m, mgr->path,
                                      "org.freedesktop.DBus.ObjectManager",
                                      "InterfacesRemoved");
    if (r < 0) return r;

    r = sd_bus_message_append_basic(m, SD_BUS_TYPE_OBJECT_PATH, path);
    if (r >= 0) r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "s");
    if (r >= 0 && interfaces)
        for (char **i = interfaces; *i && r >= 0; i++)
            r = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING, *i);
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_send(bus, m, NULL);
    sd_bus_message_unref(m);
    return r < 0 ? r : 0;
}

/* Collect the interface names registered at `path` into a NULL-terminated strv. */
static char **interfaces_at_path(sd_bus *bus, const char *path)
{
    size_t n = 0;
    for (sd_bus_slot *s = bus->slots; s; s = s->next)
        if (s->type == SLOT_VTABLE && strcmp(s->path, path) == 0) n++;
    char **v = calloc(n + 1, sizeof(char *));
    if (!v) return NULL;
    size_t i = 0;
    for (sd_bus_slot *s = bus->slots; s; s = s->next)
        if (s->type == SLOT_VTABLE && strcmp(s->path, path) == 0)
            v[i++] = s->interface;
    return v; /* free(v) only; strings are borrowed from the slots */
}

int sd_bus_emit_object_added(sd_bus *bus, const char *path)
{
    if (!bus || !path) return -EINVAL;
    char **v = interfaces_at_path(bus, path);
    if (!v) return -ENOMEM;
    int r = sd_bus_emit_interfaces_added_strv(bus, path, v);
    free(v);
    return r;
}

int sd_bus_emit_object_removed(sd_bus *bus, const char *path)
{
    if (!bus || !path) return -EINVAL;
    char **v = interfaces_at_path(bus, path);
    if (!v) return -ENOMEM;
    int r = sd_bus_emit_interfaces_removed_strv(bus, path, v);
    free(v);
    return r;
}

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
