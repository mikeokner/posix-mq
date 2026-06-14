/* Forked from the `pmq' project by Brian White (https://github.com/mscdex/pmq)
 *
 * by Michael Okner (https://github.com/mikeokner)
 *
 * Added additional features:
 *      (2014-09)
 *        Allow for user-specified flags
 *        Allow pushing strings directly rather than requiring a Buffer instance
 *      (2015-10)
 *        Add support for Node v0.12 & v4 using Native Abstractions
 *      (2019-11)
 *        Add support for Node v12 with more NAN changes
 *      (2026-06)
 *        Replace NAN with the stable Node-API
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <mqueue.h>

#include <node_api.h>
#include <uv.h>

#include <string>
#include <vector>

/* Linux/BSD queue compatibility */
#if defined(__linux__)
#define MQDES_TO_FD(mqueue) (int)(mqueue)
#elif defined(__FreeBSD__)
#define MQDES_TO_FD(mqueue) __mq_oshandle(mqueue)
#endif

static const mqd_t MQDES_INVALID = (mqd_t)-1;

static bool CheckNapi(napi_env env, napi_status status)
{
    if (status == napi_ok) {
        return true;
    }

    const napi_extended_error_info* info = NULL;
    napi_get_last_error_info(env, &info);
    const char* message = info && info->error_message ? info->error_message : "Node-API error";
    napi_throw_error(env, NULL, message);
    return false;
}

static napi_value Undefined(napi_env env)
{
    napi_value value;
    if (!CheckNapi(env, napi_get_undefined(env, &value))) {
        return NULL;
    }
    return value;
}

static bool IsType(napi_env env, napi_value value, napi_valuetype expected)
{
    napi_valuetype actual;
    return CheckNapi(env, napi_typeof(env, value, &actual)) && actual == expected;
}

static bool IsUndefined(napi_env env, napi_value value)
{
    return IsType(env, value, napi_undefined);
}

static bool IsObject(napi_env env, napi_value value)
{
    napi_valuetype type;
    if (!CheckNapi(env, napi_typeof(env, value, &type))) {
        return false;
    }
    if (type == napi_function) {
        return true;
    }
    if (type != napi_object) {
        return false;
    }

    napi_value null_value;
    bool is_null;
    return CheckNapi(env, napi_get_null(env, &null_value))
        && CheckNapi(env, napi_strict_equals(env, value, null_value, &is_null)) && !is_null;
}

static bool IsUint32(napi_env env, napi_value value)
{
    if (!IsType(env, value, napi_number)) {
        return false;
    }

    double number;
    if (!CheckNapi(env, napi_get_value_double(env, value, &number))) {
        return false;
    }
    return isfinite(number) && number >= 0 && number <= 4294967295.0 && floor(number) == number;
}

static bool ToBool(napi_env env, napi_value value, bool* result)
{
    napi_value boolean;
    return CheckNapi(env, napi_coerce_to_bool(env, value, &boolean))
        && CheckNapi(env, napi_get_value_bool(env, boolean, result));
}

static bool GetUtf8(napi_env env, napi_value value, std::string* result)
{
    size_t length;
    if (!CheckNapi(env, napi_get_value_string_utf8(env, value, NULL, 0, &length))) {
        return false;
    }

    std::vector<char> buffer(length + 1);
    size_t copied;
    if (!CheckNapi(
            env, napi_get_value_string_utf8(env, value, &buffer[0], buffer.size(), &copied))) {
        return false;
    }
    result->assign(&buffer[0], copied);
    return true;
}

static bool GetNamedProperty(
    napi_env env, napi_value object, const char* name, napi_value* result)
{
    return CheckNapi(env, napi_get_named_property(env, object, name, result));
}

class PosixMQ {
public:
    explicit PosixMQ(napi_env environment)
        : env(environment)
        , wrapper(NULL)
        , Emit(NULL)
        , async_context(NULL)
        , async_resource(NULL)
        , mqueue(MQDES_INVALID)
        , mqpollhandle(NULL)
        , mqname(NULL)
        , canread(false)
        , canwrite(false)
        , eventmask(0){};

    ~PosixMQ()
    {
        close();
        if (mqname) {
            free(mqname);
            mqname = NULL;
        }
        if (Emit) {
            napi_delete_reference(env, Emit);
            Emit = NULL;
        }
        if (wrapper) {
            napi_delete_reference(env, wrapper);
            wrapper = NULL;
        }
    }

    int close()
    {
        /* Cleanup and call mq_close() */
        int r = 0;
        if (mqueue != MQDES_INVALID) {
            uv_poll_stop(mqpollhandle);
            uv_close(reinterpret_cast<uv_handle_t*>(mqpollhandle), on_close);
            r = mq_close(mqueue);
            mqueue = MQDES_INVALID;
        }
        DestroyAsyncContext();
        return r;
    }

    void DestroyAsyncContext()
    {
        if (async_context) {
            napi_async_destroy(env, async_context);
            async_context = NULL;
        }
        if (async_resource) {
            napi_delete_reference(env, async_resource);
            async_resource = NULL;
        }
    }

    bool CreateAsyncContext()
    {
        DestroyAsyncContext();

        napi_value resource;
        napi_value resource_name;
        if (!CheckNapi(env, napi_create_object(env, &resource))
            || !CheckNapi(env,
                napi_create_string_utf8(
                    env, "posix-mq:poll", NAPI_AUTO_LENGTH, &resource_name))
            || !CheckNapi(env,
                napi_async_init(env, resource, resource_name, &async_context))) {
            return false;
        }

        if (!CheckNapi(env, napi_create_reference(env, resource, 1, &async_resource))) {
            napi_async_destroy(env, async_context);
            async_context = NULL;
            return false;
        }
        return true;
    }

    static void Finalize(napi_env env, void* data, void* hint)
    {
        delete static_cast<PosixMQ*>(data);
    }

    static void on_close(uv_handle_t* handle)
    {
        PosixMQ* obj = static_cast<PosixMQ*>(handle->data);
        delete obj->mqpollhandle;
        obj->mqpollhandle = NULL;
    }

    static void poll_cb(uv_poll_t* handle, int status, int events)
    {
        assert(status == 0);

        PosixMQ* obj = static_cast<PosixMQ*>(handle->data);

        // mq_getattr(obj->mqueue, &(obj->mqattrs));

        if ((events & UV_READABLE) && !obj->canread) {
            obj->eventmask &= ~UV_READABLE;
            obj->canread = true;
            obj->EmitEvent("messages");
        }
        else if (!(events & UV_READABLE)) {
            obj->eventmask |= UV_READABLE;
            obj->canread = false;
        }

        if ((events & UV_WRITABLE) && !obj->canwrite) {
            obj->eventmask &= ~UV_WRITABLE;
            obj->canwrite = true;
            obj->EmitEvent("drain");
        }
        else if (!(events & UV_WRITABLE)) {
            obj->eventmask |= UV_WRITABLE;
            obj->canwrite = false;
        }

        if (obj->mqueue != MQDES_INVALID) {
            uv_poll_start(obj->mqpollhandle, obj->eventmask, poll_cb);
        }
    }

    void EmitEvent(const char* event_name)
    {
        if (!Emit || !wrapper) {
            return;
        }

        napi_handle_scope scope;
        if (napi_open_handle_scope(env, &scope) != napi_ok) {
            return;
        }

        napi_value receiver;
        napi_value emit_function;
        napi_value event;
        napi_status status = napi_get_reference_value(env, wrapper, &receiver);
        if (status == napi_ok) {
            status = napi_get_reference_value(env, Emit, &emit_function);
        }
        if (status == napi_ok) {
            status = napi_create_string_utf8(env, event_name, NAPI_AUTO_LENGTH, &event);
        }
        if (status == napi_ok && receiver && emit_function) {
            napi_value result;
            status = napi_make_callback(
                env, async_context, receiver, emit_function, 1, &event, &result);
        }

        if (status == napi_pending_exception) {
            napi_value error;
            if (napi_get_and_clear_last_exception(env, &error) == napi_ok) {
                napi_fatal_exception(env, error);
            }
        }
        napi_close_handle_scope(env, scope);
    }

    bool CaptureEmit(napi_value object)
    {
        napi_value val;
        if (!GetNamedProperty(env, object, "emit", &val)) {
            return false;
        }
        if (Emit) {
            napi_delete_reference(env, Emit);
            Emit = NULL;
        }
        return CheckNapi(env, napi_create_reference(env, val, 1, &Emit));
    }

    static bool GetCallback(
        napi_env env, napi_callback_info info, size_t* argc, napi_value* args,
        napi_value* this_arg, PosixMQ** obj)
    {
        if (!CheckNapi(env, napi_get_cb_info(env, info, argc, args, this_arg, NULL))) {
            return false;
        }
        return CheckNapi(env, napi_unwrap(env, *this_arg, reinterpret_cast<void**>(obj)));
    }

    static napi_value New(napi_env env, napi_callback_info info)
    {
        /* Create a new instance of this class */
        napi_value new_target;
        if (!CheckNapi(env, napi_get_new_target(env, info, &new_target))) {
            return NULL;
        }
        if (!new_target) {
            napi_throw_type_error(env, NULL, "Use `new` to create instances of this object.");
            return NULL;
        }

        size_t argc = 0;
        napi_value this_arg;
        if (!CheckNapi(env, napi_get_cb_info(env, info, &argc, NULL, &this_arg, NULL))) {
            return NULL;
        }

        PosixMQ* obj = new PosixMQ(env);
        napi_status status =
            napi_wrap(env, this_arg, obj, Finalize, NULL, &obj->wrapper);
        if (!CheckNapi(env, status)) {
            delete obj;
            return NULL;
        }
        return this_arg;
    }

    static napi_value Open(napi_env env, napi_callback_info info)
    {
        /* Create/open a queue with mq_open()
         * TODO: clean up this method & its reuse of `val`
         */
        size_t argc = 2;
        napi_value args[2];
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, args, &this_arg, &obj)) {
            return NULL;
        }
        if (argc != 1) {
            napi_throw_type_error(env, NULL, "Expecting 1 argument");
            return NULL;
        }
        if (!IsObject(env, args[0])) {
            napi_throw_type_error(env, NULL, "Argument must be an object");
            return NULL;
        }

        if (obj->mqueue != MQDES_INVALID) {
            obj->close();
        }

        napi_value config = args[0];
        napi_value val;
        bool doCreate = false;
        int flags = O_RDWR | O_NONBLOCK;
        mode_t mode;

        if (!GetNamedProperty(env, config, "create", &val)) {
            return NULL;
        }
        if (!IsUndefined(env, val)) {
            if (!IsType(env, val, napi_boolean)) {
                napi_throw_type_error(env, NULL, "'create' property must be a boolean");
                return NULL;
            }
            if (!CheckNapi(env, napi_get_value_bool(env, val, &doCreate))) {
                return NULL;
            }
        }

        if (!GetNamedProperty(env, config, "flags", &val)) {
            return NULL;
        }
        if (!IsUndefined(env, val)) {
            if (!IsUint32(env, val)) {
                napi_throw_type_error(env, NULL, "'flags' property must be an int");
                return NULL;
            }
            int32_t int_flags;
            if (!CheckNapi(env, napi_get_value_int32(env, val, &int_flags))) {
                return NULL;
            }
            flags = int_flags;
        }

        if (!GetNamedProperty(env, config, "name", &val)) {
            return NULL;
        }
        if (!IsType(env, val, napi_string)) {
            napi_throw_type_error(env, NULL, "'name' property must be a string");
            return NULL;
        }
        std::string name;
        if (!GetUtf8(env, val, &name)) {
            return NULL;
        }

        if (!GetNamedProperty(env, config, "mode", &val)) {
            return NULL;
        }
        if (doCreate) {
            if (IsUint32(env, val)) {
                uint32_t int_mode;
                if (!CheckNapi(env, napi_get_value_uint32(env, val, &int_mode))) {
                    return NULL;
                }
                mode = static_cast<mode_t>(int_mode);
            }
            else if (IsType(env, val, napi_string)) {
                std::string mode_chars;
                if (!GetUtf8(env, val, &mode_chars)) {
                    return NULL;
                }
                mode = static_cast<mode_t>(strtoul(mode_chars.c_str(), NULL, 8));
            }
            else {
                napi_throw_type_error(env, NULL, "'mode' property must be a string or integer");
                return NULL;
            }

            flags |= O_CREAT;
            if (!GetNamedProperty(env, config, "exclusive", &val)) {
                return NULL;
            }
            bool exclusive = false;
            if (IsType(env, val, napi_boolean)
                && CheckNapi(env, napi_get_value_bool(env, val, &exclusive)) && exclusive) {
                flags |= O_EXCL;
            }

            if (!GetNamedProperty(env, config, "maxmsgs", &val)) {
                return NULL;
            }
            if (IsUint32(env, val)) {
                uint32_t maxmsgs;
                if (!CheckNapi(env, napi_get_value_uint32(env, val, &maxmsgs))) {
                    return NULL;
                }
                obj->mqattrs.mq_maxmsg = maxmsgs;
            }
            else {
                obj->mqattrs.mq_maxmsg = 10;
            }

            if (!GetNamedProperty(env, config, "msgsize", &val)) {
                return NULL;
            }
            if (IsUint32(env, val)) {
                uint32_t msgsize;
                if (!CheckNapi(env, napi_get_value_uint32(env, val, &msgsize))) {
                    return NULL;
                }
                obj->mqattrs.mq_msgsize = msgsize;
            }
            else {
                obj->mqattrs.mq_msgsize = 8192;
            }

            obj->mqueue = mq_open(name.c_str(), flags, mode, &obj->mqattrs);
        }
        else {
            obj->mqueue = mq_open(name.c_str(), flags);
        }

        if (obj->mqueue == MQDES_INVALID) {
            napi_throw_error(env, NULL, strerror(errno));
            return NULL;
        }

        if (mq_getattr(obj->mqueue, &obj->mqattrs) == -1) {
            napi_throw_error(env, NULL, strerror(errno));
            return NULL;
        }

        bool capture_emit = obj->mqname == NULL;
        if (obj->mqname) {
            free(obj->mqname);
            obj->mqname = NULL;
        }
        if (capture_emit && !obj->CaptureEmit(this_arg)) {
            obj->close();
            return NULL;
        }
        if (!obj->CreateAsyncContext()) {
            obj->close();
            return NULL;
        }

        obj->mqname = strdup(name.c_str());
        obj->canread = !(obj->mqattrs.mq_curmsgs > 0);
        obj->canwrite = !(obj->mqattrs.mq_curmsgs < obj->mqattrs.mq_maxmsg);
        if (!obj->mqpollhandle) {
            obj->mqpollhandle = new uv_poll_t;
        }
        obj->mqpollhandle->data = obj;
        obj->eventmask = UV_READABLE | UV_WRITABLE;

        uv_loop_t* loop;
        if (!CheckNapi(env, napi_get_uv_event_loop(env, &loop))) {
            return NULL;
        }
        uv_poll_init(loop, obj->mqpollhandle, MQDES_TO_FD(obj->mqueue));
        uv_poll_start(obj->mqpollhandle, obj->eventmask, poll_cb);

        return Undefined(env);
    }

    static napi_value Close(napi_env env, napi_callback_info info)
    {
        size_t argc = 0;
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, NULL, &this_arg, &obj)) {
            return NULL;
        }
        if (obj->mqueue == MQDES_INVALID) {
            napi_throw_error(env, NULL, "Queue already closed");
            return NULL;
        }

        int r = obj->close();
        if (r < 0) {
            napi_throw_error(env, NULL, uv_strerror(r));
            return NULL;
        }
        return Undefined(env);
    }

    static napi_value Unlink(napi_env env, napi_callback_info info)
    {
        size_t argc = 0;
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, NULL, &this_arg, &obj)) {
            return NULL;
        }
        if (!obj->mqname) {
            napi_throw_error(env, NULL, "Nothing to unlink");
            return NULL;
        }
        if (mq_unlink(obj->mqname) == -1) {
            napi_throw_error(env, NULL, strerror(errno));
            return NULL;
        }
        free(obj->mqname);
        obj->mqname = NULL;
        return Undefined(env);
    }

    static napi_value Send(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value args[2];
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, args, &this_arg, &obj)) {
            return NULL;
        }
        if (argc < 1) {
            napi_throw_type_error(env, NULL, "Expected at least 1 argument");
            return NULL;
        }

        bool is_buffer;
        if (!CheckNapi(env, napi_is_buffer(env, args[0], &is_buffer))) {
            return NULL;
        }
        bool is_string = IsType(env, args[0], napi_string);
        if (!is_buffer && !is_string) {
            napi_throw_type_error(env, NULL, "First argument must be a node::Buffer or v8::String");
            return NULL;
        }

        uint32_t priority = 0;
        if (argc >= 2) {
            if (!IsUint32(env, args[1])) {
                napi_throw_type_error(env, NULL, "Second argument must be an integer 0 <= n < 32");
                return NULL;
            }
            if (!CheckNapi(env, napi_get_value_uint32(env, args[1], &priority))) {
                return NULL;
            }
            if (priority >= 32) {
                napi_throw_type_error(env, NULL, "Second argument must be an integer 0 <= n < 32");
                return NULL;
            }
        }

        int send_result;
        if (is_buffer) {
            void* data;
            size_t length;
            if (!CheckNapi(env, napi_get_buffer_info(env, args[0], &data, &length))) {
                return NULL;
            }
            send_result = mq_send(obj->mqueue, static_cast<char*>(data), length, priority);
        }
        else {
            std::string message;
            if (!GetUtf8(env, args[0], &message)) {
                return NULL;
            }
            send_result = mq_send(obj->mqueue, message.c_str(), strlen(message.c_str()), priority);
        }

        bool ret = true;
        if (send_result == -1) {
            if (errno != EAGAIN) {
                napi_throw_error(env, NULL, strerror(errno));
                return NULL;
            }
            ret = false;
        }
        mq_getattr(obj->mqueue, &obj->mqattrs);

        napi_value value;
        if (!CheckNapi(env, napi_get_boolean(env, ret, &value))) {
            return NULL;
        }
        return value;
    }

    static napi_value Receive(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value args[2];
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, args, &this_arg, &obj)) {
            return NULL;
        }
        if (argc < 1) {
            napi_throw_type_error(env, NULL, "Expected at least 1 argument");
            return NULL;
        }

        bool is_buffer;
        if (!CheckNapi(env, napi_is_buffer(env, args[0], &is_buffer))) {
            return NULL;
        }
        if (!is_buffer) {
            napi_throw_type_error(env, NULL, "First argument must be a node::Buffer");
            return NULL;
        }

        bool retTuple = false;
        if (argc > 1 && !ToBool(env, args[1], &retTuple)) {
            return NULL;
        }

        void* data;
        size_t length;
        if (!CheckNapi(env, napi_get_buffer_info(env, args[0], &data, &length))) {
            return NULL;
        }

        uint32_t priority;
        ssize_t nBytes = mq_receive(
            obj->mqueue, static_cast<char*>(data), length, &priority);
        napi_value ret;
        if (nBytes == -1) {
            if (errno != EAGAIN) {
                napi_throw_error(env, NULL, strerror(errno));
                return NULL;
            }
            if (!CheckNapi(env, napi_get_boolean(env, false, &ret))) {
                return NULL;
            }
        }
        else if (!retTuple) {
            if (!CheckNapi(env, napi_create_uint32(env, static_cast<uint32_t>(nBytes), &ret))) {
                return NULL;
            }
        }
        else {
            if (!CheckNapi(env, napi_create_array_with_length(env, 2, &ret))) {
                return NULL;
            }
            napi_value bytes_value;
            napi_value priority_value;
            if (!CheckNapi(
                    env, napi_create_uint32(env, static_cast<uint32_t>(nBytes), &bytes_value))
                || !CheckNapi(env, napi_create_uint32(env, priority, &priority_value))
                || !CheckNapi(env, napi_set_element(env, ret, 0, bytes_value))
                || !CheckNapi(env, napi_set_element(env, ret, 1, priority_value))) {
                return NULL;
            }
        }
        mq_getattr(obj->mqueue, &obj->mqattrs);
        return ret;
    }

    static napi_value MsgsizeGetter(napi_env env, napi_callback_info info)
    {
        size_t argc = 0;
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, NULL, &this_arg, &obj)) {
            return NULL;
        }
        mq_getattr(obj->mqueue, &obj->mqattrs);

        napi_value result;
        if (!CheckNapi(
                env, napi_create_uint32(env, obj->mqattrs.mq_msgsize, &result))) {
            return NULL;
        }
        return result;
    }

    static napi_value MaxmsgsGetter(napi_env env, napi_callback_info info)
    {
        size_t argc = 0;
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, NULL, &this_arg, &obj)) {
            return NULL;
        }
        mq_getattr(obj->mqueue, &obj->mqattrs);

        napi_value result;
        if (!CheckNapi(
                env, napi_create_uint32(env, obj->mqattrs.mq_maxmsg, &result))) {
            return NULL;
        }
        return result;
    }

    static napi_value CurmsgsGetter(napi_env env, napi_callback_info info)
    {
        size_t argc = 0;
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, NULL, &this_arg, &obj)) {
            return NULL;
        }
        mq_getattr(obj->mqueue, &obj->mqattrs);

        napi_value result;
        if (!CheckNapi(
                env, napi_create_uint32(env, obj->mqattrs.mq_curmsgs, &result))) {
            return NULL;
        }
        return result;
    }

    static napi_value IsfullGetter(napi_env env, napi_callback_info info)
    {
        size_t argc = 0;
        napi_value this_arg;
        PosixMQ* obj;
        if (!GetCallback(env, info, &argc, NULL, &this_arg, &obj)) {
            return NULL;
        }
        mq_getattr(obj->mqueue, &obj->mqattrs);

        napi_value result;
        if (!CheckNapi(env,
                napi_get_boolean(env,
                    obj->mqattrs.mq_curmsgs == obj->mqattrs.mq_maxmsg, &result))) {
            return NULL;
        }
        return result;
    }

    napi_env env;
    napi_ref wrapper;
    napi_ref Emit;
    napi_async_context async_context;
    napi_ref async_resource;
    mqd_t mqueue;
    struct mq_attr mqattrs;
    uv_poll_t* mqpollhandle;
    char* mqname;
    bool canread;
    bool canwrite;
    int eventmask;
};

static napi_value Initialize(napi_env env, napi_value exports)
{
    napi_property_attributes method_attributes = static_cast<napi_property_attributes>(
        napi_writable | napi_enumerable | napi_configurable);
    napi_property_attributes accessor_attributes = static_cast<napi_property_attributes>(
        napi_enumerable | napi_configurable);
    napi_property_descriptor properties[] = {
        { "open", NULL, PosixMQ::Open, NULL, NULL, NULL, method_attributes, NULL },
        { "close", NULL, PosixMQ::Close, NULL, NULL, NULL, method_attributes, NULL },
        { "push", NULL, PosixMQ::Send, NULL, NULL, NULL, method_attributes, NULL },
        { "shift", NULL, PosixMQ::Receive, NULL, NULL, NULL, method_attributes, NULL },
        { "unlink", NULL, PosixMQ::Unlink, NULL, NULL, NULL, method_attributes, NULL },
        { "msgsize", NULL, NULL, PosixMQ::MsgsizeGetter, NULL, NULL, accessor_attributes, NULL },
        { "maxmsgs", NULL, NULL, PosixMQ::MaxmsgsGetter, NULL, NULL, accessor_attributes, NULL },
        { "curmsgs", NULL, NULL, PosixMQ::CurmsgsGetter, NULL, NULL, accessor_attributes, NULL },
        { "isFull", NULL, NULL, PosixMQ::IsfullGetter, NULL, NULL, accessor_attributes, NULL },
    };

    napi_value constructor;
    if (!CheckNapi(env,
            napi_define_class(env, "PosixMQ", NAPI_AUTO_LENGTH, PosixMQ::New, NULL,
                sizeof(properties) / sizeof(properties[0]), properties, &constructor))
        || !CheckNapi(env, napi_set_named_property(env, exports, "PosixMQ", constructor))) {
        return NULL;
    }
    return exports;
}

NAPI_MODULE(posixmq, Initialize)
