/*
 * sd-bus-libdbus.h — Real sd-bus API implementation for macOS using libdbus-1.
 *
 * Forward-declares sd_bus, sd_bus_message, sd_bus_slot, sd_bus_creds so that
 * the sdbus-c++ C++ layer compiles without exposing libdbus headers to callers.
 * The full struct bodies live in sd-bus-libdbus.c.
 */

#ifndef SDBUS_CXX_MACOS_SD_BUS_LIBDBUS_H
#define SDBUS_CXX_MACOS_SD_BUS_LIBDBUS_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types (struct bodies in sd-bus-libdbus.c) ─────────────────────── */

typedef struct sd_bus           sd_bus;
typedef struct sd_bus_message   sd_bus_message;
typedef struct sd_bus_slot      sd_bus_slot;
typedef struct sd_bus_creds     sd_bus_creds;
typedef struct sd_event         sd_event;
typedef struct sd_event_source  sd_event_source;

typedef struct sd_id128 { uint8_t bytes[16]; } sd_id128_t;

/* ── sd_bus_error (stack-allocated, no libdbus deps) ──────────────────────── */

typedef struct sd_bus_error {
    const char *name;
    const char *message;
    int _need_free;
} sd_bus_error;

#define SD_BUS_ERROR_NULL               { NULL, NULL, 0 }
#define SD_BUS_ERROR_NOT_SUPPORTED      "org.freedesktop.DBus.Error.NotSupported"

/* ── Callback typedefs ────────────────────────────────────────────────────── */

typedef int (*sd_bus_message_handler_t)(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
typedef int (*sd_bus_property_get_t)(sd_bus *bus, const char *path, const char *interface,
                                     const char *property, sd_bus_message *reply,
                                     void *userdata, sd_bus_error *ret_error);
typedef int (*sd_bus_property_set_t)(sd_bus *bus, const char *path, const char *interface,
                                     const char *property, sd_bus_message *value,
                                     void *userdata, sd_bus_error *ret_error);

/* ── vtable ───────────────────────────────────────────────────────────────── */

enum {
    _SD_BUS_VTABLE_START            = '<',
    _SD_BUS_VTABLE_END              = '>',
    _SD_BUS_VTABLE_METHOD           = 'M',
    _SD_BUS_VTABLE_SIGNAL           = 'S',
    _SD_BUS_VTABLE_PROPERTY         = 'P',
    _SD_BUS_VTABLE_WRITABLE_PROPERTY = 'W',
};

typedef struct sd_bus_vtable {
    uint8_t  type;
    uint64_t flags;
    union {
        struct {
            const char *member;
            const char *signature;
            const char *result;
            sd_bus_message_handler_t handler;
            size_t offset;
            const char *names;
        } method;
        struct {
            const char *member;
            const char *signature;
            const char *names;
        } signal;
        struct {
            const char *member;
            const char *signature;
            sd_bus_property_get_t get;
            sd_bus_property_set_t set;
            size_t offset;
        } property;
    } x;
} sd_bus_vtable;

#define SD_BUS_VTABLE_START(_flags) \
    { _SD_BUS_VTABLE_START, (_flags), { { NULL, NULL, NULL, NULL, 0, NULL } } }
#define SD_BUS_VTABLE_END \
    { _SD_BUS_VTABLE_END,  0,         { { NULL, NULL, NULL, NULL, 0, NULL } } }
#define SD_BUS_METHOD(_member, _sig, _res, _handler, _flags) \
    { _SD_BUS_VTABLE_METHOD, (_flags), \
      { .method = { (_member), (_sig), (_res), (_handler), 0, NULL } } }
#define SD_BUS_METHOD_WITH_NAMES(_member, _sig, _in_names, _res, _out_names, _handler, _flags) \
    { _SD_BUS_VTABLE_METHOD, (_flags), \
      { .method = { (_member), (_sig), (_res), (_handler), 0, (_in_names) } } }
#define SD_BUS_SIGNAL(_member, _sig, _flags) \
    { _SD_BUS_VTABLE_SIGNAL, (_flags), { .signal = { (_member), (_sig), NULL } } }
#define SD_BUS_SIGNAL_WITH_NAMES(_member, _sig, _out_names, _flags) \
    { _SD_BUS_VTABLE_SIGNAL, (_flags), { .signal = { (_member), (_sig), (_out_names) } } }
#define SD_BUS_PROPERTY(_member, _sig, _get, _offset, _flags) \
    { _SD_BUS_VTABLE_PROPERTY, (_flags), \
      { .property = { (_member), (_sig), (_get), NULL, (_offset) } } }
#define SD_BUS_WRITABLE_PROPERTY(_member, _sig, _get, _set, _offset, _flags) \
    { _SD_BUS_VTABLE_WRITABLE_PROPERTY, (_flags), \
      { .property = { (_member), (_sig), (_get), (_set), (_offset) } } }

/* ── vtable flags ─────────────────────────────────────────────────────────── */
#define SD_BUS_VTABLE_DEPRECATED                    (1ULL << 0U)
#define SD_BUS_VTABLE_METHOD_NO_REPLY               (1ULL << 1U)
#define SD_BUS_VTABLE_PROPERTY_CONST                (1ULL << 2U)
#define SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE         (1ULL << 3U)
#define SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION   (1ULL << 4U)
#define SD_BUS_VTABLE_UNPRIVILEGED                  (1ULL << 5U)

/* ── request-name flags ───────────────────────────────────────────────────── */
#define SD_BUS_NAME_ALLOW_REPLACEMENT       (1ULL << 0U)
#define SD_BUS_NAME_REPLACE_EXISTING        (1ULL << 1U)
#define SD_BUS_NAME_QUEUE                   (1ULL << 2U)

/* ── creds flags ──────────────────────────────────────────────────────────── */
#define SD_BUS_CREDS_PID                    (1ULL << 0U)
#define SD_BUS_CREDS_UID                    (1ULL << 1U)
#define SD_BUS_CREDS_EUID                   (1ULL << 2U)
#define SD_BUS_CREDS_GID                    (1ULL << 3U)
#define SD_BUS_CREDS_EGID                   (1ULL << 4U)
#define SD_BUS_CREDS_SUPPLEMENTARY_GIDS     (1ULL << 5U)
#define SD_BUS_CREDS_SELINUX_CONTEXT        (1ULL << 6U)
#define SD_BUS_CREDS_AUGMENT                (1ULL << 63U)

/* ── D-Bus type constants ─────────────────────────────────────────────────── */
#define _SD_BUS_MESSAGE_TYPE_INVALID 0

#define SD_BUS_TYPE_BYTE        'y'
#define SD_BUS_TYPE_BOOLEAN     'b'
#define SD_BUS_TYPE_INT16       'n'
#define SD_BUS_TYPE_UINT16      'q'
#define SD_BUS_TYPE_INT32       'i'
#define SD_BUS_TYPE_UINT32      'u'
#define SD_BUS_TYPE_INT64       'x'
#define SD_BUS_TYPE_UINT64      't'
#define SD_BUS_TYPE_DOUBLE      'd'
#define SD_BUS_TYPE_STRING      's'
#define SD_BUS_TYPE_OBJECT_PATH 'o'
#define SD_BUS_TYPE_SIGNATURE   'g'
#define SD_BUS_TYPE_UNIX_FD     'h'
#define SD_BUS_TYPE_ARRAY       'a'
#define SD_BUS_TYPE_VARIANT     'v'
#define SD_BUS_TYPE_STRUCT      'r'
#define SD_BUS_TYPE_DICT_ENTRY  'e'

#define SD_EVENT_OFF     0
#define SD_EVENT_ONESHOT 1

/* ── sd_bus_error utilities (pure string management, no libdbus) ──────────── */

static inline void sd_bus_error_free(sd_bus_error *e)
{
    if (!e) return;
    if (e->_need_free) {
        free((void *)e->name);
        free((void *)e->message);
    }
    e->name = NULL; e->message = NULL; e->_need_free = 0;
}

/* sd-bus convention: returns 0 if no error name is given, otherwise a negative
 * errno corresponding to the error (generic -EIO here; the error *name* is what
 * callers actually inspect). */
static inline int sd_bus_error_set(sd_bus_error *e, const char *name, const char *message)
{
    if (!e) return name ? -EIO : 0;
    sd_bus_error_free(e);
    if (name)    { e->name    = strdup(name);    if (!e->name)    return -ENOMEM; }
    if (message) { e->message = strdup(message); if (!e->message) { free((void*)e->name); e->name=NULL; return -ENOMEM; } }
    e->_need_free = 1;
    return name ? -EIO : 0;
}

static inline int sd_bus_error_set_errno(sd_bus_error *e, int error)
{
    if (error == 0) return 0;
    int positive = error < 0 ? -error : error;
    sd_bus_error_set(e, "org.freedesktop.DBus.Error.Failed", strerror(positive));
    return -positive;
}

static inline int sd_bus_error_is_set(const sd_bus_error *e)
{
    return e && e->name;
}

/* ── Validation helpers (pure string, no libdbus) ────────────────────────── */

static inline int sd_bus_service_name_is_valid(const char *p)   { return p && *p; }
static inline int sd_bus_object_path_is_valid(const char *p)    { return p && *p == '/'; }
static inline int sd_bus_interface_name_is_valid(const char *p) { return p && *p; }
static inline int sd_bus_member_name_is_valid(const char *p)    { return p && *p; }

/* ── id128 ────────────────────────────────────────────────────────────────── */
static inline int sd_id128_randomize(sd_id128_t *ret)
{
    if (ret) memset(ret, 0, sizeof(*ret));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Function declarations — implemented in sd-bus-libdbus.c
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Bus lifecycle */
int sd_bus_new(sd_bus **ret);
int sd_bus_open(sd_bus **ret);
int sd_bus_open_system(sd_bus **ret);
int sd_bus_open_user(sd_bus **ret);
int sd_bus_open_system_remote(sd_bus **ret, const char *host);
int sd_bus_open_user_with_address(sd_bus **ret, const char *address);
int sd_bus_open_direct(sd_bus **ret, const char *address);
int sd_bus_set_address(sd_bus *bus, const char *address);
int sd_bus_set_bus_client(sd_bus *bus, int b);
int sd_bus_set_trusted(sd_bus *bus, int b);
int sd_bus_set_fd(sd_bus *bus, int input_fd, int output_fd);
int sd_bus_set_server(sd_bus *bus, int b, sd_id128_t server_id);
int sd_bus_set_anonymous(sd_bus *bus, int b);
int sd_bus_set_method_call_timeout(sd_bus *bus, uint64_t usec);
int sd_bus_get_method_call_timeout(sd_bus *bus, uint64_t *ret);
int sd_bus_start(sd_bus *bus);
void sd_bus_close(sd_bus *bus);
sd_bus *sd_bus_unref(sd_bus *bus);
sd_bus *sd_bus_flush_close_unref(sd_bus *bus);
sd_bus *sd_bus_close_unref(sd_bus *bus);
int sd_bus_flush(sd_bus *bus);

/* Bus name */
int sd_bus_request_name(sd_bus *bus, const char *name, uint64_t flags);
int sd_bus_release_name(sd_bus *bus, const char *name);
int sd_bus_get_unique_name(sd_bus *bus, const char **name);

/* Event loop */
int sd_bus_process(sd_bus *bus, sd_bus_message **ret_msg);
int sd_bus_get_fd(sd_bus *bus);
int sd_bus_get_events(sd_bus *bus);
int sd_bus_get_timeout(sd_bus *bus, uint64_t *timeout_usec);
int sd_bus_get_n_queued_read(sd_bus *bus, uint64_t *ret);
int sd_bus_get_n_queued_write(sd_bus *bus, uint64_t *ret);

/* Message send/call */
int sd_bus_send(sd_bus *bus, sd_bus_message *msg, uint64_t *cookie);
int sd_bus_call(sd_bus *bus, sd_bus_message *msg, uint64_t usec,
                sd_bus_error *ret_error, sd_bus_message **reply);
int sd_bus_call_async(sd_bus *bus, sd_bus_slot **slot, sd_bus_message *msg,
                      sd_bus_message_handler_t callback, void *userdata, uint64_t usec);

/* Message lifecycle */
sd_bus_message *sd_bus_message_ref(sd_bus_message *m);
sd_bus_message *sd_bus_message_unref(sd_bus_message *m);

/* Message creation */
int sd_bus_message_new(sd_bus *bus, sd_bus_message **msg, uint8_t type);
int sd_bus_message_new_method_call(sd_bus *bus, sd_bus_message **msg,
                                   const char *destination, const char *path,
                                   const char *interface, const char *member);
int sd_bus_message_new_signal(sd_bus *bus, sd_bus_message **msg,
                              const char *path, const char *interface,
                              const char *member);
int sd_bus_message_new_method_return(sd_bus_message *call, sd_bus_message **ret);
int sd_bus_message_new_method_error(sd_bus_message *call, sd_bus_message **ret,
                                    const sd_bus_error *err);
int sd_bus_message_set_destination(sd_bus_message *msg, const char *destination);
int sd_bus_message_set_expect_reply(sd_bus_message *msg, int b);
int sd_bus_message_get_expect_reply(sd_bus_message *msg);
int sd_bus_message_seal(sd_bus_message *msg, uint64_t cookie, uint64_t timeout);

/* Message metadata */
const char *sd_bus_message_get_interface(sd_bus_message *msg);
const char *sd_bus_message_get_member(sd_bus_message *msg);
const char *sd_bus_message_get_sender(sd_bus_message *msg);
const char *sd_bus_message_get_path(sd_bus_message *msg);
const char *sd_bus_message_get_destination(sd_bus_message *msg);
int sd_bus_message_get_cookie(sd_bus_message *msg, uint64_t *cookie);
int sd_bus_message_get_reply_cookie(sd_bus_message *msg, uint64_t *cookie);
const sd_bus_error *sd_bus_message_get_error(sd_bus_message *msg);
sd_bus_message *sd_bus_get_current_message(sd_bus *bus);

/* Message serialization — append */
int sd_bus_message_append_basic(sd_bus_message *msg, char type, const void *p);
int sd_bus_message_append_array(sd_bus_message *msg, char type,
                                const void *ptr, size_t size);
int sd_bus_message_append_string_space(sd_bus_message *msg, size_t size, char **ret);
int sd_bus_message_open_container(sd_bus_message *msg, char type, const char *contents);
int sd_bus_message_close_container(sd_bus_message *msg);

/* Message serialization — read */
int sd_bus_message_read_basic(sd_bus_message *msg, char type, void *p);
int sd_bus_message_read_array(sd_bus_message *msg, char type,
                              const void **ret_ptr, size_t *ret_size);
int sd_bus_message_enter_container(sd_bus_message *msg, char type, const char *contents);
int sd_bus_message_exit_container(sd_bus_message *msg);
int sd_bus_message_peek_type(sd_bus_message *msg, char *type, const char **contents);
int sd_bus_message_at_end(sd_bus_message *msg, int complete);
int sd_bus_message_is_empty(sd_bus_message *msg);
int sd_bus_message_rewind(sd_bus_message *msg, int complete);
int sd_bus_message_copy(sd_bus_message *dst, sd_bus_message *src, int all);
int sd_bus_message_dump(sd_bus_message *msg, FILE *f, uint64_t flags);

/* Object / match registration */
int sd_bus_add_object_vtable(sd_bus *bus, sd_bus_slot **slot,
                             const char *path, const char *interface,
                             const sd_bus_vtable *vtable, void *userdata);
int sd_bus_add_object_manager(sd_bus *bus, sd_bus_slot **slot, const char *path);
int sd_bus_add_match(sd_bus *bus, sd_bus_slot **slot, const char *match,
                     sd_bus_message_handler_t callback, void *userdata);
int sd_bus_add_match_async(sd_bus *bus, sd_bus_slot **slot, const char *match,
                           sd_bus_message_handler_t callback,
                           sd_bus_message_handler_t install_callback, void *userdata);
int sd_bus_match_signal(sd_bus *bus, sd_bus_slot **ret, const char *sender,
                        const char *path, const char *interface, const char *member,
                        sd_bus_message_handler_t callback, void *userdata);
sd_bus_slot *sd_bus_slot_unref(sd_bus_slot *slot);

/* Emit helpers */
int sd_bus_emit_properties_changed_strv(sd_bus *bus, const char *path,
                                        const char *interface, char **names);
int sd_bus_emit_object_added(sd_bus *bus, const char *path);
int sd_bus_emit_object_removed(sd_bus *bus, const char *path);
int sd_bus_emit_interfaces_added_strv(sd_bus *bus, const char *path,
                                      char **interfaces);
int sd_bus_emit_interfaces_removed_strv(sd_bus *bus, const char *path,
                                        char **interfaces);

/* Credentials */
int sd_bus_query_sender_creds(sd_bus_message *msg, uint64_t mask,
                              sd_bus_creds **creds);
sd_bus_creds *sd_bus_creds_ref(sd_bus_creds *creds);
sd_bus_creds *sd_bus_creds_unref(sd_bus_creds *creds);
int sd_bus_creds_get_pid(sd_bus_creds *creds, pid_t *pid);
int sd_bus_creds_get_uid(sd_bus_creds *creds, uid_t *uid);
int sd_bus_creds_get_euid(sd_bus_creds *creds, uid_t *euid);
int sd_bus_creds_get_gid(sd_bus_creds *creds, gid_t *gid);
int sd_bus_creds_get_egid(sd_bus_creds *creds, uid_t *egid);
int sd_bus_creds_get_supplementary_gids(sd_bus_creds *creds, const gid_t **gids);
int sd_bus_creds_get_selinux_context(sd_bus_creds *creds, const char **label);

#ifdef __cplusplus
}
#endif

#endif /* SDBUS_CXX_MACOS_SD_BUS_LIBDBUS_H */
