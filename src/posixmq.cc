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
 */

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <mqueue.h>
#include <nan.h>

/* Linux/BSD queue compatibility */
#if defined(__linux__)
#define MQDES_TO_FD(mqueue) (int)(mqueue)
#elif defined(__FreeBSD__)
#define MQDES_TO_FD(mqueue) __mq_oshandle(mqueue)
#endif

/* libuv compatibility */
#ifdef UV_VERSION_MAJOR
#ifndef UV_VERSION_PATCH
#define UV_VERSION_PATCH 0
#endif
#define UV_VERSION ((UV_VERSION_MAJOR << 16) | (UV_VERSION_MINOR << 8) | (UV_VERSION_PATCH))
#else
#define UV_VERSION 0x000b00
#endif
#if UV_VERSION < 0x000b00
#define GET_UV_ERROR_STR(err) uv_strerror(uv_last_error(uv_default_loop()))
#else
#define GET_UV_ERROR_STR(err) uv_strerror((int)err)
#endif

static Nan::Persistent<v8::FunctionTemplate> constructor;
static const mqd_t MQDES_INVALID = (mqd_t)-1;

class PosixMQ : public Nan::ObjectWrap {
public:
    mqd_t mqueue;
    struct mq_attr mqattrs;
    uv_poll_t* mqpollhandle;
    char* mqname;
    Nan::Persistent<v8::Function> Emit;
    bool canread;
    bool canwrite;
    int eventmask;

    PosixMQ()
        : mqueue(MQDES_INVALID)
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
        Emit.Reset();
    }

    int close()
    {
        /* Cleanup and call mq_close() */
        Nan::HandleScope scope;
        int r = 0;
        if (mqueue != MQDES_INVALID) {
            uv_poll_stop(mqpollhandle);
            uv_close((uv_handle_t*)mqpollhandle, on_close);
            r = mq_close(mqueue);
            mqueue = MQDES_INVALID;
        }
        return r;
    }

    static void on_close(uv_handle_t* handle)
    {
        Nan::HandleScope scope;
        PosixMQ* obj = (PosixMQ*)handle->data;
        delete obj->mqpollhandle;
        obj->mqpollhandle = NULL;
    }

    static void poll_cb(uv_poll_t* handle, int status, int events)
    {
        Nan::HandleScope scope;
        assert(status == 0);

        PosixMQ* obj = (PosixMQ*)handle->data;

        // mq_getattr(obj->mqueue, &(obj->mqattrs));

        if ((events & UV_READABLE) && !obj->canread) {
            obj->eventmask &= ~UV_READABLE;
            obj->canread = true;
            v8::Local<v8::Function> emit = Nan::New(obj->Emit);
            v8::Local<v8::Value> read_emit_argv_local[1];
            read_emit_argv_local[0] = Nan::New<v8::String>("messages").ToLocalChecked();
            Nan::TryCatch try_catch;
            Nan::MakeCallback(obj->handle(), emit, 1, read_emit_argv_local);
            if (try_catch.HasCaught()) {
                Nan::FatalException(try_catch);
            }
        }
        else if (!(events & UV_READABLE)) {
            obj->eventmask |= UV_READABLE;
            obj->canread = false;
        }

        if ((events & UV_WRITABLE) && !obj->canwrite) {
            obj->eventmask &= ~UV_WRITABLE;
            obj->canwrite = true;
            v8::Local<v8::Function> emit = Nan::New(obj->Emit);
            v8::Local<v8::Value> write_emit_argv_local[1];
            write_emit_argv_local[0] = Nan::New<v8::String>("drain").ToLocalChecked();
            Nan::TryCatch try_catch;
            Nan::MakeCallback(obj->handle(), emit, 1, write_emit_argv_local);
            if (try_catch.HasCaught()) {
                Nan::FatalException(try_catch);
            }
        }
        else if (!(events & UV_WRITABLE)) {
            obj->eventmask |= UV_WRITABLE;
            obj->canwrite = false;
        }

        if (obj->mqueue != MQDES_INVALID) {
            uv_poll_start(obj->mqpollhandle, obj->eventmask, poll_cb);
        }
    }

    static void New(const Nan::FunctionCallbackInfo<v8::Value>& info)
    {
        /* Create a new instance of this class */
        Nan::HandleScope scope;
        if (!info.IsConstructCall()) {
            Nan::ThrowTypeError("Use `new` to create instances of this object.");
            return;
        }

        PosixMQ* obj = new PosixMQ();
        obj->Wrap(info.This());

        info.GetReturnValue().Set(info.This());
    }

    static void Open(const Nan::FunctionCallbackInfo<v8::Value>& info)
    {
        /* Create/open a queue with mq_open()
         * TODO: clean up this method & its reuse of `val`
         */
        Nan::HandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());
        bool doCreate = false;
        int flags = O_RDWR | O_NONBLOCK;
        mode_t mode;

        /* Close existing queue if already open */
        if (obj->mqueue != MQDES_INVALID) {
            obj->close();
        }

        if (info.Length() != 1) {
            Nan::ThrowTypeError("Expecting 1 argument");
            return;
        }
        if (!info[0]->IsObject()) {
            Nan::ThrowTypeError("Argument must be an object");
            return;
        }

        v8::Local<v8::Object> config = info[0].As<v8::Object>();
        v8::Local<v8::Value> val;

        /* Whether or not to create the queue when opening */
        if (!(val = Nan::Get(config, Nan::New("create").ToLocalChecked()).ToLocalChecked())
                 ->IsUndefined()) {
            if (!val->IsBoolean()) {
                Nan::ThrowTypeError("'create' property must be a boolean");
                return;
            }
            doCreate = Nan::To<bool>(val).FromJust();
        }

        /* Optional override for default O_RDWR | O_NONBLOCK flag */
        if (!(val = Nan::Get(config, Nan::New("flags").ToLocalChecked()).ToLocalChecked())
                 ->IsUndefined()) {
            if (val->IsUint32()) {
                flags = Nan::To<int32_t>(val).FromJust();
            }
            else {
                Nan::ThrowTypeError("'flags' property must be an int");
                return;
            }
        }

        /* Required name of queue to open */
        val = Nan::Get(config, Nan::New("name").ToLocalChecked()).ToLocalChecked();
        if (!val->IsString()) {
            Nan::ThrowTypeError("'name' property must be a string");
            return;
        }
        v8::Local<v8::String> namestr = Nan::To<v8::String>(val).ToLocalChecked();
        Nan::Utf8String name(namestr);

        val = Nan::Get(config, Nan::New("mode").ToLocalChecked()).ToLocalChecked();

        /* If creating, get params to use for creation */
        if (doCreate) {
            /* Mode specifies permissions */
            if (val->IsUint32()) {
                mode = (mode_t)Nan::To<int32_t>(val).FromJust();
            }
            else if (val->IsString()) {
                v8::Local<v8::String> modestr = Nan::To<v8::String>(val).ToLocalChecked();
                Nan::Utf8String mode_chars(modestr);
                mode = (mode_t)strtoul((const char*)(*mode_chars), NULL, 8);
            }
            else {
                Nan::ThrowTypeError("'mode' property must be a string or integer");
                return;
            }

            flags |= O_CREAT;
            val = Nan::Get(config, Nan::New("exclusive").ToLocalChecked()).ToLocalChecked();
            if (val->IsBoolean() && Nan::To<bool>(val).FromJust() == true) {
                flags |= O_EXCL;
            }

            /* Max number of messages allowed in queue */
            val = Nan::Get(config, Nan::New("maxmsgs").ToLocalChecked()).ToLocalChecked();
            if (val->IsUint32()) {
                obj->mqattrs.mq_maxmsg = Nan::To<int32_t>(val).FromJust();
            }
            else {
                obj->mqattrs.mq_maxmsg = 10;
            }
            /* Size of each message on the queue */
            val = Nan::Get(config, Nan::New("msgsize").ToLocalChecked()).ToLocalChecked();
            if (val->IsUint32()) {
                obj->mqattrs.mq_msgsize = Nan::To<int32_t>(val).FromJust();
            }
            else {
                obj->mqattrs.mq_msgsize = 8192;
            }

            /* Do the open */
            obj->mqueue = mq_open(*name, flags, mode, &(obj->mqattrs));
        }
        else {
            obj->mqueue = mq_open(*name, flags);
        }

        /* If opening failed, throw exception */
        if (obj->mqueue == MQDES_INVALID) {
            Nan::ThrowError(strerror(errno));
            return;
        }

        /* Open mq reference */
        int mq_rc = mq_getattr(obj->mqueue, &(obj->mqattrs));

        if (mq_rc == -1) {
            Nan::ThrowError(strerror(errno));
        }

        if (obj->mqname) {
            free(obj->mqname);
            obj->mqname = NULL;
        }
        else {
            obj->Emit.Reset(Nan::Persistent<v8::Function>(v8::Local<v8::Function>::Cast(
                Nan::Get(obj->handle(), Nan::New<v8::String>("emit").ToLocalChecked())
                    .ToLocalChecked())));
        }

        /* Set attrs for reference */
        obj->mqname = strdup(*name);
        obj->canread = !(obj->mqattrs.mq_curmsgs > 0);
        obj->canwrite = !(obj->mqattrs.mq_curmsgs < obj->mqattrs.mq_maxmsg);
        if (!obj->mqpollhandle) {
            obj->mqpollhandle = new uv_poll_t;
        }
        obj->mqpollhandle->data = obj;
        obj->eventmask = UV_READABLE | UV_WRITABLE;

        uv_poll_init(uv_default_loop(), obj->mqpollhandle, MQDES_TO_FD(obj->mqueue));
        uv_poll_start(obj->mqpollhandle, obj->eventmask, poll_cb);

        info.GetReturnValue().SetUndefined();
    }

    static void Close(const Nan::FunctionCallbackInfo<v8::Value>& info)
    {
        Nan::HandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());

        if (obj->mqueue == MQDES_INVALID) {
            Nan::ThrowError("Queue already closed");
            return;
        }

        int r = obj->close();

        if (r < 0) {
            Nan::ThrowError(GET_UV_ERROR_STR(r));
            return;
        }
        info.GetReturnValue().SetUndefined();
    }

    static void Unlink(const Nan::FunctionCallbackInfo<v8::Value>& info)
    {
        Nan::HandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());

        if (!obj->mqname) {
            Nan::ThrowError("Nothing to unlink");
            return;
        }

        int mq_rc = mq_unlink((const char*)obj->mqname);
        if (mq_rc == -1) {
            Nan::ThrowError(strerror(errno));
            return;
        }

        if (obj->mqname) {
            free(obj->mqname);
            obj->mqname = NULL;
        }
        info.GetReturnValue().SetUndefined();
    }

    /* Push data onto the queue using mq_send() */
    static void Send(const Nan::FunctionCallbackInfo<v8::Value>& info)
    {
        Nan::EscapableHandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());
        uint32_t priority = 0;
        int send_result;
        bool ret = true;

        if (info.Length() < 1) {
            Nan::ThrowTypeError("Expected at least 1 argument");
            return;
        }
        else if ((!node::Buffer::HasInstance(info[0])) and (!info[0]->IsString())) {
            Nan::ThrowTypeError("First argument must be a node::Buffer or v8::String");
            return;
        }
        else if (info.Length() >= 2) {
            if (info[1]->IsUint32() && Nan::To<int32_t>(info[1]).FromJust() < 32) {
                priority = Nan::To<int32_t>(info[1]).FromJust();
            }
            else {
                Nan::ThrowTypeError("Second argument must be an integer 0 <= n < 32");
                return;
            }
        }

        if (node::Buffer::HasInstance(info[0])) {
            /* v8::Object passed in is a node::Buffer object */
            send_result = mq_send(obj->mqueue, node::Buffer::Data(info[0].As<v8::Object>()),
                node::Buffer::Length(info[0].As<v8::Object>()), priority);
        }
        else if (info[0]->IsString()) {
            /* v8::Object passed in is a javascript string */
            v8::Local<v8::String> msgstr = Nan::To<v8::String>(info[0]).ToLocalChecked();
            Nan::Utf8String message(msgstr);
            send_result = mq_send(obj->mqueue, *message, strlen(*message), priority);
        }
        else {
            /* Shouldn't ever actually get here since we checked above.
             * Just keeping the compiler warning-free. */
            Nan::ThrowTypeError("First argument wasn't a node::Buffer or v8::String!");
            return;
        }

        if (send_result == -1) {
            if (errno != EAGAIN) {
                Nan::ThrowError(strerror(errno));
                return;
            }
            ret = false;
        }

        mq_getattr(obj->mqueue, &(obj->mqattrs));

        info.GetReturnValue().Set(scope.Escape(Nan::New<v8::Boolean>(ret)));
    }

    static void Receive(const Nan::FunctionCallbackInfo<v8::Value>& info)
    {
        Nan::EscapableHandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());
        ssize_t nBytes;
        uint32_t priority;
        bool retTuple = false;
        v8::Local<v8::Value> ret;

        if (info.Length() < 1) {
            Nan::ThrowTypeError("Expected at least 1 argument");
            return;
        }
        else if (!node::Buffer::HasInstance(info[0])) {
            Nan::ThrowTypeError("First argument must be a node::Buffer");
            return;
        }
        else if (info.Length() > 1) {
            retTuple = Nan::To<bool>(info[1]).FromJust();
        }

        v8::Local<v8::Object> buf = info[0].As<v8::Object>();
        if ((nBytes = mq_receive(
                 obj->mqueue, node::Buffer::Data(buf), node::Buffer::Length(buf), &priority))
            == -1) {
            if (errno != EAGAIN) {
                Nan::ThrowError(strerror(errno));
                return;
            }
            ret = Nan::New<v8::Boolean>(false);
        }
        else if (!retTuple) {
            ret = Nan::New<v8::Integer>(static_cast<uint32_t>(nBytes));
        }
        else {
            v8::Local<v8::Array> tuple = Nan::New<v8::Array>(2);
            Nan::Set(tuple, 0, Nan::New<v8::Integer>(static_cast<uint32_t>(nBytes)));
            Nan::Set(tuple, 1, Nan::New<v8::Integer>(static_cast<uint32_t>(priority)));
            ret = tuple;
        }

        mq_getattr(obj->mqueue, &(obj->mqattrs));

        info.GetReturnValue().Set(scope.Escape(ret));
    }

    static void MsgsizeGetter(
        v8::Local<v8::String> property, const Nan::PropertyCallbackInfo<v8::Value>& info)
    {
        Nan::EscapableHandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());

        mq_getattr(obj->mqueue, &(obj->mqattrs));

        info.GetReturnValue().Set(
            scope.Escape(Nan::New<v8::Integer>(static_cast<uint32_t>(obj->mqattrs.mq_msgsize))));
    }

    static void MaxmsgsGetter(
        v8::Local<v8::String> property, const Nan::PropertyCallbackInfo<v8::Value>& info)
    {
        Nan::EscapableHandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());

        mq_getattr(obj->mqueue, &(obj->mqattrs));

        info.GetReturnValue().Set(
            scope.Escape(Nan::New<v8::Integer>(static_cast<uint32_t>(obj->mqattrs.mq_maxmsg))));
    }

    static void CurmsgsGetter(
        v8::Local<v8::String> property, const Nan::PropertyCallbackInfo<v8::Value>& info)
    {
        Nan::EscapableHandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());

        mq_getattr(obj->mqueue, &(obj->mqattrs));

        info.GetReturnValue().Set(
            scope.Escape(Nan::New<v8::Integer>(static_cast<uint32_t>(obj->mqattrs.mq_curmsgs))));
    }

    static void IsfullGetter(
        v8::Local<v8::String> property, const Nan::PropertyCallbackInfo<v8::Value>& info)
    {
        Nan::EscapableHandleScope scope;
        PosixMQ* obj = Nan::ObjectWrap::Unwrap<PosixMQ>(info.This());

        mq_getattr(obj->mqueue, &(obj->mqattrs));

        info.GetReturnValue().Set(
            scope.Escape(Nan::New<v8::Boolean>(obj->mqattrs.mq_curmsgs == obj->mqattrs.mq_maxmsg)));
    }

    static void Initialize(Nan::ADDON_REGISTER_FUNCTION_ARGS_TYPE target)
    {
        Nan::HandleScope scope;
        /* Init FunctionTemplate */
        v8::Local<v8::String> name = Nan::New<v8::String>("PosixMQ").ToLocalChecked();
        v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);

        tpl->SetClassName(name);
        tpl->InstanceTemplate()->SetInternalFieldCount(1);

        /* Init methods */
        Nan::SetPrototypeMethod(tpl, "open", Open);
        Nan::SetPrototypeMethod(tpl, "close", Close);
        Nan::SetPrototypeMethod(tpl, "push", Send);
        Nan::SetPrototypeMethod(tpl, "shift", Receive);
        Nan::SetPrototypeMethod(tpl, "unlink", Unlink);

        /* Init properties */
        Nan::SetAccessor(tpl->PrototypeTemplate(), Nan::New("msgsize").ToLocalChecked(), MsgsizeGetter);
        Nan::SetAccessor(tpl->PrototypeTemplate(), Nan::New("maxmsgs").ToLocalChecked(), MaxmsgsGetter);
        Nan::SetAccessor(tpl->PrototypeTemplate(), Nan::New("curmsgs").ToLocalChecked(), CurmsgsGetter);
        Nan::SetAccessor(tpl->PrototypeTemplate(), Nan::New("isFull").ToLocalChecked(), IsfullGetter);

        /* Hook it up */
        constructor.Reset(tpl);
        Nan::Set(target, name, Nan::GetFunction(tpl).ToLocalChecked());
    }
};

extern "C" {
NAN_MODULE_INIT(init)
{
    Nan::HandleScope scope;
    PosixMQ::Initialize(target);
}

NODE_MODULE(posixmq, init)
}
