#ifndef SDBUS_CXX_MACOS_SD_BUS_STUB_H
#define SDBUS_CXX_MACOS_SD_BUS_STUB_H

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

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;
typedef struct sd_bus_creds sd_bus_creds;
typedef struct sd_event sd_event;
typedef struct sd_event_source sd_event_source;
typedef struct sd_id128
{
    uint8_t bytes[16];
} sd_id128_t;

struct sd_bus { int unused; };
struct sd_bus_message { int unused; };
struct sd_bus_slot { int unused; };
struct sd_bus_creds { int unused; };
struct sd_event { int unused; };
struct sd_event_source { int unused; };

typedef struct sd_bus_error
{
    const char *name;
    const char *message;
    int _need_free;
} sd_bus_error;

#define SD_BUS_ERROR_NULL { NULL, NULL, 0 }
#define SD_BUS_ERROR_NOT_SUPPORTED "org.freedesktop.DBus.Error.NotSupported"

typedef int (*sd_bus_message_handler_t)(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
typedef int (*sd_bus_property_get_t)(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
typedef int (*sd_bus_property_set_t)(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *value, void *userdata, sd_bus_error *ret_error);

enum
{
    _SD_BUS_VTABLE_START = '<',
    _SD_BUS_VTABLE_END = '>',
    _SD_BUS_VTABLE_METHOD = 'M',
    _SD_BUS_VTABLE_SIGNAL = 'S',
    _SD_BUS_VTABLE_PROPERTY = 'P',
    _SD_BUS_VTABLE_WRITABLE_PROPERTY = 'W',
};

typedef struct sd_bus_vtable
{
    uint8_t type;
    uint64_t flags;
    union
    {
        struct
        {
            const char *member;
            const char *signature;
            const char *result;
            sd_bus_message_handler_t handler;
            size_t offset;
            const char *names;
        } method;
        struct
        {
            const char *member;
            const char *signature;
            const char *names;
        } signal;
        struct
        {
            const char *member;
            const char *signature;
            sd_bus_property_get_t get;
            sd_bus_property_set_t set;
            size_t offset;
        } property;
    } x;
} sd_bus_vtable;

#define SD_BUS_VTABLE_START(_flags) { _SD_BUS_VTABLE_START, (_flags), { { NULL, NULL, NULL, NULL, 0, NULL } } }
#define SD_BUS_VTABLE_END { _SD_BUS_VTABLE_END, 0, { { NULL, NULL, NULL, NULL, 0, NULL } } }
#define SD_BUS_METHOD(_member, _signature, _result, _handler, _flags) { _SD_BUS_VTABLE_METHOD, (_flags), { .method = { (_member), (_signature), (_result), (_handler), 0, NULL } } }
#define SD_BUS_METHOD_WITH_NAMES(_member, _signature, _in_names, _result, _out_names, _handler, _flags) { _SD_BUS_VTABLE_METHOD, (_flags), { .method = { (_member), (_signature), (_result), (_handler), 0, (_in_names) } } }
#define SD_BUS_SIGNAL(_member, _signature, _flags) { _SD_BUS_VTABLE_SIGNAL, (_flags), { .signal = { (_member), (_signature), NULL } } }
#define SD_BUS_SIGNAL_WITH_NAMES(_member, _signature, _out_names, _flags) { _SD_BUS_VTABLE_SIGNAL, (_flags), { .signal = { (_member), (_signature), (_out_names) } } }
#define SD_BUS_PROPERTY(_member, _signature, _get, _offset, _flags) { _SD_BUS_VTABLE_PROPERTY, (_flags), { .property = { (_member), (_signature), (_get), NULL, (_offset) } } }
#define SD_BUS_WRITABLE_PROPERTY(_member, _signature, _get, _set, _offset, _flags) { _SD_BUS_VTABLE_WRITABLE_PROPERTY, (_flags), { .property = { (_member), (_signature), (_get), (_set), (_offset) } } }

#define SD_BUS_VTABLE_DEPRECATED (1ULL << 0U)
#define SD_BUS_VTABLE_METHOD_NO_REPLY (1ULL << 1U)
#define SD_BUS_VTABLE_PROPERTY_CONST (1ULL << 2U)
#define SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE (1ULL << 3U)
#define SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION (1ULL << 4U)
#define SD_BUS_VTABLE_UNPRIVILEGED (1ULL << 5U)

#define SD_BUS_CREDS_PID (1ULL << 0U)
#define SD_BUS_CREDS_UID (1ULL << 1U)
#define SD_BUS_CREDS_EUID (1ULL << 2U)
#define SD_BUS_CREDS_GID (1ULL << 3U)
#define SD_BUS_CREDS_EGID (1ULL << 4U)
#define SD_BUS_CREDS_SUPPLEMENTARY_GIDS (1ULL << 5U)
#define SD_BUS_CREDS_SELINUX_CONTEXT (1ULL << 6U)
#define SD_BUS_CREDS_AUGMENT (1ULL << 63U)

#define _SD_BUS_MESSAGE_TYPE_INVALID 0

#define SD_BUS_TYPE_BYTE 'y'
#define SD_BUS_TYPE_BOOLEAN 'b'
#define SD_BUS_TYPE_INT16 'n'
#define SD_BUS_TYPE_UINT16 'q'
#define SD_BUS_TYPE_INT32 'i'
#define SD_BUS_TYPE_UINT32 'u'
#define SD_BUS_TYPE_INT64 'x'
#define SD_BUS_TYPE_UINT64 't'
#define SD_BUS_TYPE_DOUBLE 'd'
#define SD_BUS_TYPE_STRING 's'
#define SD_BUS_TYPE_OBJECT_PATH 'o'
#define SD_BUS_TYPE_SIGNATURE 'g'
#define SD_BUS_TYPE_UNIX_FD 'h'
#define SD_BUS_TYPE_ARRAY 'a'
#define SD_BUS_TYPE_VARIANT 'v'
#define SD_BUS_TYPE_STRUCT 'r'
#define SD_BUS_TYPE_DICT_ENTRY 'e'

#define SD_EVENT_OFF 0
#define SD_EVENT_ONESHOT 1

static inline int sdbus_stub_error(void) { return -ENOTSUP; }

static inline char *sdbus_stub_strdup(const char *value)
{
    if (!value)
        return NULL;
    const size_t size = strlen(value) + 1U;
    char *copy = (char *)malloc(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

static inline void sd_bus_error_free(sd_bus_error *e)
{
    if (e && e->_need_free)
    {
        free((void *)e->name);
        free((void *)e->message);
    }
    if (e)
    {
        e->name = NULL;
        e->message = NULL;
        e->_need_free = 0;
    }
}

static inline int sd_bus_error_set(sd_bus_error *e, const char *name, const char *message)
{
    if (!e)
        return -EINVAL;
    sd_bus_error_free(e);
    e->name = sdbus_stub_strdup(name);
    e->message = sdbus_stub_strdup(message);
    e->_need_free = 1;
    return -ENOTSUP;
}

static inline int sd_bus_error_set_errno(sd_bus_error *e, int error)
{
    (void)error;
    return sd_bus_error_set(e, "org.freedesktop.DBus.Error.Failed", strerror(error));
}

static inline int sd_bus_error_is_set(const sd_bus_error *e)
{
    return e && e->name;
}

static inline sd_bus_message *sd_bus_message_ref(sd_bus_message *m) { return m; }
static inline sd_bus_message *sd_bus_message_unref(sd_bus_message *m) { return m; }
static inline sd_bus_slot *sd_bus_slot_unref(sd_bus_slot *slot) { return slot; }
static inline sd_bus_creds *sd_bus_creds_ref(sd_bus_creds *creds) { return creds; }
static inline sd_bus_creds *sd_bus_creds_unref(sd_bus_creds *creds) { return creds; }

static inline int sd_bus_send(sd_bus *bus, sd_bus_message *msg, uint64_t *cookie) { (void)bus; (void)msg; (void)cookie; return sdbus_stub_error(); }
static inline int sd_bus_call(sd_bus *bus, sd_bus_message *msg, uint64_t usec, sd_bus_error *ret_error, sd_bus_message **reply) { (void)bus; (void)msg; (void)usec; (void)ret_error; (void)reply; return sdbus_stub_error(); }
static inline int sd_bus_call_async(sd_bus *bus, sd_bus_slot **slot, sd_bus_message *msg, sd_bus_message_handler_t callback, void *userdata, uint64_t usec) { (void)bus; (void)slot; (void)msg; (void)callback; (void)userdata; (void)usec; return sdbus_stub_error(); }
static inline int sd_bus_message_new(sd_bus *bus, sd_bus_message **msg, uint8_t type) { (void)bus; (void)msg; (void)type; return sdbus_stub_error(); }
static inline int sd_bus_message_new_method_call(sd_bus *bus, sd_bus_message **msg, const char *destination, const char *path, const char *interface, const char *member) { (void)bus; (void)msg; (void)destination; (void)path; (void)interface; (void)member; return sdbus_stub_error(); }
static inline int sd_bus_message_new_signal(sd_bus *bus, sd_bus_message **msg, const char *path, const char *interface, const char *member) { (void)bus; (void)msg; (void)path; (void)interface; (void)member; return sdbus_stub_error(); }
static inline int sd_bus_message_new_method_return(sd_bus_message *call, sd_bus_message **msg) { (void)call; (void)msg; return sdbus_stub_error(); }
static inline int sd_bus_message_new_method_error(sd_bus_message *call, sd_bus_message **msg, const sd_bus_error *err) { (void)call; (void)msg; (void)err; return sdbus_stub_error(); }
static inline int sd_bus_set_method_call_timeout(sd_bus *bus, uint64_t usec) { (void)bus; (void)usec; return sdbus_stub_error(); }
static inline int sd_bus_get_method_call_timeout(sd_bus *bus, uint64_t *ret) { (void)bus; (void)ret; return sdbus_stub_error(); }
static inline int sd_bus_emit_properties_changed_strv(sd_bus *bus, const char *path, const char *interface, char **names) { (void)bus; (void)path; (void)interface; (void)names; return sdbus_stub_error(); }
static inline int sd_bus_emit_object_added(sd_bus *bus, const char *path) { (void)bus; (void)path; return sdbus_stub_error(); }
static inline int sd_bus_emit_object_removed(sd_bus *bus, const char *path) { (void)bus; (void)path; return sdbus_stub_error(); }
static inline int sd_bus_emit_interfaces_added_strv(sd_bus *bus, const char *path, char **interfaces) { (void)bus; (void)path; (void)interfaces; return sdbus_stub_error(); }
static inline int sd_bus_emit_interfaces_removed_strv(sd_bus *bus, const char *path, char **interfaces) { (void)bus; (void)path; (void)interfaces; return sdbus_stub_error(); }
static inline int sd_bus_open(sd_bus **ret) { (void)ret; return sdbus_stub_error(); }
static inline int sd_bus_open_system(sd_bus **ret) { (void)ret; return sdbus_stub_error(); }
static inline int sd_bus_open_user(sd_bus **ret) { (void)ret; return sdbus_stub_error(); }
static inline int sd_bus_open_system_remote(sd_bus **ret, const char *host) { (void)ret; (void)host; return sdbus_stub_error(); }
static inline int sd_bus_request_name(sd_bus *bus, const char *name, uint64_t flags) { (void)bus; (void)name; (void)flags; return sdbus_stub_error(); }
static inline int sd_bus_release_name(sd_bus *bus, const char *name) { (void)bus; (void)name; return sdbus_stub_error(); }
static inline int sd_bus_get_unique_name(sd_bus *bus, const char **name) { (void)bus; (void)name; return sdbus_stub_error(); }
static inline int sd_bus_add_object_vtable(sd_bus *bus, sd_bus_slot **slot, const char *path, const char *interface, const sd_bus_vtable *vtable, void *userdata) { (void)bus; (void)slot; (void)path; (void)interface; (void)vtable; (void)userdata; return sdbus_stub_error(); }
static inline int sd_bus_add_object_manager(sd_bus *bus, sd_bus_slot **slot, const char *path) { (void)bus; (void)slot; (void)path; return sdbus_stub_error(); }
static inline int sd_bus_add_match(sd_bus *bus, sd_bus_slot **slot, const char *match, sd_bus_message_handler_t callback, void *userdata) { (void)bus; (void)slot; (void)match; (void)callback; (void)userdata; return sdbus_stub_error(); }
static inline int sd_bus_add_match_async(sd_bus *bus, sd_bus_slot **slot, const char *match, sd_bus_message_handler_t callback, sd_bus_message_handler_t install_callback, void *userdata) { (void)bus; (void)slot; (void)match; (void)callback; (void)install_callback; (void)userdata; return sdbus_stub_error(); }
static inline int sd_bus_match_signal(sd_bus *bus, sd_bus_slot **ret, const char *sender, const char *path, const char *interface, const char *member, sd_bus_message_handler_t callback, void *userdata) { (void)bus; (void)ret; (void)sender; (void)path; (void)interface; (void)member; (void)callback; (void)userdata; return sdbus_stub_error(); }
static inline int sd_bus_new(sd_bus **ret) { (void)ret; return sdbus_stub_error(); }
static inline int sd_bus_start(sd_bus *bus) { (void)bus; return sdbus_stub_error(); }
static inline int sd_bus_process(sd_bus *bus, sd_bus_message **msg) { (void)bus; (void)msg; return sdbus_stub_error(); }
static inline sd_bus_message *sd_bus_get_current_message(sd_bus *bus) { (void)bus; return NULL; }
static inline int sd_bus_get_fd(sd_bus *bus) { (void)bus; return sdbus_stub_error(); }
static inline int sd_bus_get_events(sd_bus *bus) { (void)bus; return sdbus_stub_error(); }
static inline int sd_bus_get_timeout(sd_bus *bus, uint64_t *timeout_usec) { (void)bus; (void)timeout_usec; return sdbus_stub_error(); }
static inline int sd_bus_get_n_queued_read(sd_bus *bus, uint64_t *read) { (void)bus; (void)read; return sdbus_stub_error(); }
static inline int sd_bus_get_n_queued_write(sd_bus *bus, uint64_t *write) { (void)bus; (void)write; return sdbus_stub_error(); }
static inline int sd_bus_flush(sd_bus *bus) { (void)bus; return sdbus_stub_error(); }
static inline sd_bus *sd_bus_flush_close_unref(sd_bus *bus) { return bus; }
static inline sd_bus *sd_bus_close_unref(sd_bus *bus) { return bus; }
static inline void sd_bus_close(sd_bus *bus) { (void)bus; }
static inline sd_bus *sd_bus_unref(sd_bus *bus) { return bus; }
static inline int sd_bus_set_address(sd_bus *bus, const char *address) { (void)bus; (void)address; return sdbus_stub_error(); }
static inline int sd_bus_set_bus_client(sd_bus *bus, int b) { (void)bus; (void)b; return sdbus_stub_error(); }
static inline int sd_bus_set_trusted(sd_bus *bus, int b) { (void)bus; (void)b; return sdbus_stub_error(); }
static inline int sd_bus_set_fd(sd_bus *bus, int input_fd, int output_fd) { (void)bus; (void)input_fd; (void)output_fd; return sdbus_stub_error(); }
static inline int sd_id128_randomize(sd_id128_t *ret)
{
    if (ret)
        memset(ret, 0, sizeof(*ret));
    return 0;
}

static inline int sd_bus_set_server(sd_bus *bus, int b, sd_id128_t server_id) { (void)bus; (void)b; (void)server_id; return sdbus_stub_error(); }
static inline int sd_bus_set_anonymous(sd_bus *bus, int b) { (void)bus; (void)b; return sdbus_stub_error(); }
static inline int sd_bus_message_set_destination(sd_bus_message *msg, const char *destination) { (void)msg; (void)destination; return sdbus_stub_error(); }
static inline int sd_bus_query_sender_creds(sd_bus_message *msg, uint64_t mask, sd_bus_creds **creds) { (void)msg; (void)mask; (void)creds; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_pid(sd_bus_creds *creds, pid_t *pid) { (void)creds; (void)pid; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_uid(sd_bus_creds *creds, uid_t *uid) { (void)creds; (void)uid; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_euid(sd_bus_creds *creds, uid_t *euid) { (void)creds; (void)euid; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_gid(sd_bus_creds *creds, gid_t *gid) { (void)creds; (void)gid; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_egid(sd_bus_creds *creds, gid_t *egid) { (void)creds; (void)egid; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_supplementary_gids(sd_bus_creds *creds, const gid_t **gids) { (void)creds; (void)gids; return sdbus_stub_error(); }
static inline int sd_bus_creds_get_selinux_context(sd_bus_creds *creds, const char **label) { (void)creds; (void)label; return sdbus_stub_error(); }

static inline int sd_bus_message_append_basic(sd_bus_message *msg, char type, const void *p) { (void)msg; (void)type; (void)p; return sdbus_stub_error(); }
static inline int sd_bus_message_append_array(sd_bus_message *msg, char type, const void *ptr, size_t size) { (void)msg; (void)type; (void)ptr; (void)size; return sdbus_stub_error(); }
static inline int sd_bus_message_append_string_space(sd_bus_message *msg, size_t size, char **s) { (void)msg; (void)size; (void)s; return sdbus_stub_error(); }
static inline int sd_bus_message_read_basic(sd_bus_message *msg, char type, void *p) { (void)msg; (void)type; (void)p; return sdbus_stub_error(); }
static inline int sd_bus_message_read_array(sd_bus_message *msg, char type, const void **ptr, size_t *size) { (void)msg; (void)type; (void)ptr; (void)size; return sdbus_stub_error(); }
static inline int sd_bus_message_open_container(sd_bus_message *msg, char type, const char *contents) { (void)msg; (void)type; (void)contents; return sdbus_stub_error(); }
static inline int sd_bus_message_close_container(sd_bus_message *msg) { (void)msg; return sdbus_stub_error(); }
static inline int sd_bus_message_enter_container(sd_bus_message *msg, char type, const char *contents) { (void)msg; (void)type; (void)contents; return sdbus_stub_error(); }
static inline int sd_bus_message_exit_container(sd_bus_message *msg) { (void)msg; return sdbus_stub_error(); }
static inline int sd_bus_message_copy(sd_bus_message *dst, sd_bus_message *src, int all) { (void)dst; (void)src; (void)all; return sdbus_stub_error(); }
static inline int sd_bus_message_seal(sd_bus_message *msg, uint64_t cookie, uint64_t timeout) { (void)msg; (void)cookie; (void)timeout; return sdbus_stub_error(); }
static inline int sd_bus_message_rewind(sd_bus_message *msg, int complete) { (void)msg; (void)complete; return sdbus_stub_error(); }
static inline int sd_bus_message_dump(sd_bus_message *msg, FILE *f, uint64_t flags) { (void)msg; (void)f; (void)flags; return sdbus_stub_error(); }
static inline const char *sd_bus_message_get_interface(sd_bus_message *msg) { (void)msg; return NULL; }
static inline const char *sd_bus_message_get_member(sd_bus_message *msg) { (void)msg; return NULL; }
static inline const char *sd_bus_message_get_sender(sd_bus_message *msg) { (void)msg; return NULL; }
static inline const char *sd_bus_message_get_path(sd_bus_message *msg) { (void)msg; return NULL; }
static inline const char *sd_bus_message_get_destination(sd_bus_message *msg) { (void)msg; return NULL; }
static inline int sd_bus_message_get_cookie(sd_bus_message *msg, uint64_t *cookie) { (void)msg; (void)cookie; return sdbus_stub_error(); }
static inline int sd_bus_message_peek_type(sd_bus_message *msg, char *type, const char **contents) { (void)msg; (void)type; (void)contents; return sdbus_stub_error(); }
static inline int sd_bus_message_is_empty(sd_bus_message *msg) { (void)msg; return 1; }
static inline int sd_bus_message_at_end(sd_bus_message *msg, int complete) { (void)msg; (void)complete; return 1; }
static inline int sd_bus_message_set_expect_reply(sd_bus_message *msg, int b) { (void)msg; (void)b; return sdbus_stub_error(); }
static inline int sd_bus_message_get_expect_reply(sd_bus_message *msg) { (void)msg; return 0; }
static inline int sd_bus_message_get_reply_cookie(sd_bus_message *msg, uint64_t *cookie) { (void)msg; (void)cookie; return sdbus_stub_error(); }
static inline const sd_bus_error *sd_bus_message_get_error(sd_bus_message *msg) { (void)msg; return NULL; }

static inline int sd_bus_service_name_is_valid(const char *p) { return p && *p; }
static inline int sd_bus_object_path_is_valid(const char *p) { return p && *p == '/'; }
static inline int sd_bus_interface_name_is_valid(const char *p) { return p && *p; }
static inline int sd_bus_member_name_is_valid(const char *p) { return p && *p; }

#ifdef __cplusplus
}
#endif

#endif
