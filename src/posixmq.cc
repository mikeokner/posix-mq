/* Forked from the `pmq' project by Brian White (https://github.com/mscdex/pmq)
 *
 * Added additional features to allow for user-specified flags and pushing
 * strings directly rather than requiring they be converted to a node::Buffer object.
 *
 * 2014-09 by Michael Okner (https://github.com/mikeokner)
 */


#include <node.h>
#include <node_buffer.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <mqueue.h>

#if defined(__linux__)
#  define MQDES_TO_FD(mqdes) (int)(mqdes)
#elif defined(__FreeBSD__)
#  define MQDES_TO_FD(mqdes) __mq_oshandle(mqdes)
#endif

static v8::Persistent<v8::FunctionTemplate> constructor;
static v8::Persistent<v8::String> emit_symbol;
static v8::Persistent<v8::Value> read_emit_argv[1];
static v8::Persistent<v8::Value> write_emit_argv[1];
static const mqd_t MQDES_INVALID = (mqd_t)-1;


class PosixMQ : public node::ObjectWrap {
  public:
    mqd_t mqdes;
    struct mq_attr mqattrs;
    uv_poll_t* mqpollhandle;
    char* mqname;
    v8::Persistent<v8::Function> Emit;
    bool canread;
    bool canwrite;
    int eventmask;

    PosixMQ() : mqpollhandle(NULL), mqdes(MQDES_INVALID), mqname(NULL),
                canread(false), canwrite(false), eventmask(0) {};

    ~PosixMQ() {
      close();
      if (mqname) {
        free(mqname);
        mqname = NULL;
      }
      Emit.Dispose();
      Emit.Clear();
    }

    int close() {
      /* Cleanup and call mq_close() */
      int r = 0;
      if (mqdes != MQDES_INVALID) {
        uv_poll_stop(mqpollhandle);
        uv_close((uv_handle_t *)mqpollhandle, on_close);
        r = mq_close(mqdes);
        mqdes = MQDES_INVALID;
      }
      return r;
    }

    static void on_close (uv_handle_t *handle) {
      PosixMQ* obj = (PosixMQ*)handle->data;
      delete obj->mqpollhandle;
      obj->mqpollhandle = NULL;
    }

    static v8::Handle<v8::Value> New(const v8::Arguments& args) {
      /* Create a new instance of this class */
      v8::HandleScope scope;

      if (!args.IsConstructCall()) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("Use `new` to create instances of this object.")));
      }

      PosixMQ* obj = new PosixMQ();
      obj->Wrap(args.This());

      return args.This();
    }

    static v8::Handle<v8::Value> Open(const v8::Arguments& args) {
      /* Create/open a queue with mq_open() */
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(args.This());
      bool doCreate = false;
      int flags = O_RDWR | O_NONBLOCK;
      mode_t mode;
      const char* name;

      if (args.Length() != 1) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("Expecting 1 argument")));
      }
      if (!args[0]->IsObject()) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("Argument must be an object")));
      }

      v8::Local<v8::Object> config = args[0]->ToObject();
      v8::Local<v8::Value> val;

      /* Whether or not to create the queue when opening */
      if (!(val = config->Get(v8::String::New("create")))->IsUndefined()) {
        if (!val->IsBoolean()) {
          return v8::ThrowException(v8::Exception::TypeError(
              v8::String::New("'create' property must be a boolean")));
        }
        doCreate = val->BooleanValue();
      }

      /* Optional override for default O_RDWR | O_NONBLOCK flag */
      if (!(val = config->Get(v8::String::New("flags")))->IsUndefined()) {
        if (val->IsUint32())
            flags = val->Uint32Value();
        else {
            return v8::ThrowException(v8::Exception::TypeError(
                v8::String::New("'flags' property must be an int")));
        }
      }

      /* Required name of queue to open */
      val = config->Get(v8::String::New("name"));
      if (!val->IsString()) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("'name' property must be a string")));
      }
      v8::String::AsciiValue namestr(val->ToString());
      name = (const char*)(*namestr);

      val = config->Get(v8::String::New("mode"));

      /* If creating, get params to use for creation */
      if (doCreate) {
        /* Mode specifies permissions */
        if (val->IsUint32())
          mode = (mode_t)val->Uint32Value();
        else if (val->IsString()) {
          v8::String::AsciiValue modestr(val->ToString());
          mode = (mode_t)strtoul((const char*)(*modestr), NULL, 8);
        } else {
          return v8::ThrowException(v8::Exception::TypeError(
              v8::String::New("'mode' property must be a string or integer")));
        }
        flags |= O_CREAT;

        val = config->Get(v8::String::New("exclusive"));
        if (val->IsBoolean() && val->BooleanValue() == true)
          flags |= O_EXCL;

        /* Max number of messages allowed in queue */
        val = config->Get(v8::String::New("maxmsgs"));
        if (val->IsUint32())
          obj->mqattrs.mq_maxmsg = val->Uint32Value();
        else
          obj->mqattrs.mq_maxmsg = 10;
        /* Size of each message on the queue */
        val = config->Get(v8::String::New("msgsize"));
        if (val->IsUint32())
          obj->mqattrs.mq_msgsize = val->Uint32Value();
        else
          obj->mqattrs.mq_msgsize = 8192;
      }

      /* Close existing queue if already open */
      if (obj->mqdes != MQDES_INVALID)
        obj->close();

      /* Open mq reference */
      if (doCreate)
        obj->mqdes = mq_open(name, flags, mode, &(obj->mqattrs));
      else
        obj->mqdes = mq_open(name, flags);

      /* If opening failed, throw exception */
      if (obj->mqdes == MQDES_INVALID ||
          mq_getattr(obj->mqdes, &(obj->mqattrs)) == -1) {
        return v8::ThrowException(v8::Exception::Error(
            v8::String::New(uv_strerror(uv_last_error(uv_default_loop())))));
      }

      if (obj->mqname) {
        free(obj->mqname);
        obj->mqname = NULL;
      } else {
        obj->Emit = v8::Persistent<v8::Function>::New(v8::Local<v8::Function>::Cast(
                                               obj->handle_->Get(emit_symbol)));
      }

      /* Set attrs for reference */
      obj->mqname = strdup(name);
      obj->canread = !(obj->mqattrs.mq_curmsgs > 0);
      obj->canwrite = !(obj->mqattrs.mq_curmsgs < obj->mqattrs.mq_maxmsg);
      if (!obj->mqpollhandle)
        obj->mqpollhandle = new uv_poll_t;
      obj->mqpollhandle->data = obj;
      obj->eventmask = UV_READABLE | UV_WRITABLE;

      uv_poll_init(uv_default_loop(), obj->mqpollhandle, MQDES_TO_FD(obj->mqdes));
      uv_poll_start(obj->mqpollhandle, obj->eventmask, poll_cb);

      return v8::Undefined();
    }

    static void poll_cb(uv_poll_t *handle, int status, int events) {
      v8::HandleScope scope;
      assert(status == 0);

      PosixMQ* obj = (PosixMQ*)handle->data;

      //mq_getattr(obj->mqdes, &(obj->mqattrs));

      if ((events & UV_READABLE) && !obj->canread) {
        obj->eventmask &= ~UV_READABLE;
        obj->canread = true;

        v8::TryCatch try_catch;
        obj->Emit->Call(obj->handle_, 1, read_emit_argv);
        if (try_catch.HasCaught())
          node::FatalException(try_catch);
      } else if (!(events & UV_READABLE)) {
        obj->eventmask |= UV_READABLE;
        obj->canread = false;
      }

      if ((events & UV_WRITABLE) && !obj->canwrite) {
        obj->eventmask &= ~UV_WRITABLE;
        obj->canwrite = true;

        v8::TryCatch try_catch;
        obj->Emit->Call(obj->handle_, 1, write_emit_argv);
        if (try_catch.HasCaught())
          node::FatalException(try_catch);
      } else if (!(events & UV_WRITABLE)) {
        obj->eventmask |= UV_WRITABLE;
        obj->canwrite = false;
      }

      if (obj->mqdes != MQDES_INVALID)
        uv_poll_start(obj->mqpollhandle, obj->eventmask, poll_cb);
    }

    static v8::Handle<v8::Value> Close(const v8::Arguments& args) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(args.This());

      if (obj->mqdes == MQDES_INVALID) {
        return v8::ThrowException(v8::Exception::Error(
            v8::String::New("Queue already closed")));
      }

      int r = obj->close();

      if (r == -1) {
        return v8::ThrowException(v8::Exception::Error(
            v8::String::New(uv_strerror(uv_last_error(uv_default_loop())))));
      }

      return v8::Undefined();
    }

    static v8::Handle<v8::Value> Unlink(const v8::Arguments& args) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(args.This());

      if (!obj->mqname) {
        return v8::ThrowException(v8::Exception::Error(
            v8::String::New("Nothing to unlink")));
      }

      if (mq_unlink((const char*)obj->mqname) == -1) {
        return v8::ThrowException(v8::Exception::Error(
            v8::String::New(uv_strerror(uv_last_error(uv_default_loop())))));
      }

      if (obj->mqname) {
        free(obj->mqname);
        obj->mqname = NULL;
      }

      return v8::Undefined();
    }

    static v8::Handle<v8::Value> Send(const v8::Arguments& args) {
      /* Push data onto the queue using mq_send() */
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(args.This());
      uint32_t priority = 0;
      int send_result;
      bool ret = true;

      if (args.Length() < 1) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("Expected at least 1 argument")));
      } else if ((!node::Buffer::HasInstance(args[0])) and (!args[0]->IsString())) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("First argument must be a node::Buffer or v8::String")));
      } else if (args.Length() >= 2) {
        if (args[1]->IsUint32() && args[1]->Uint32Value() < 32)
          priority = args[1]->Uint32Value();
        else {
          return v8::ThrowException(v8::Exception::TypeError(
              v8::String::New("Second argument must be an integer 0 <= n < 32")));
        }
      }

      if (node::Buffer::HasInstance(args[0])) {
          /* v8::Object passed in is a node::Buffer object */
          send_result = mq_send(obj->mqdes, node::Buffer::Data(args[0]->ToObject()),
                                node::Buffer::Length(args[0]->ToObject()), priority);
      }
      else if (args[0]->IsString()) {
          /* v8::Object passed in is a javascript string */
          const char* message;
          v8::String::AsciiValue msgstr(args[0]->ToString());
          message = (const char*)(*msgstr);
          send_result = mq_send(obj->mqdes, message, strlen(message), priority);
      }
      else {
        return v8::ThrowException(v8::Exception::TypeError(
          /* Shouldn't ever actually get here since we checked above */
          v8::String::New("First argument wasn't a node::Buffer or v8::String!")));
      }

      if (send_result == -1) {
        if (errno != EAGAIN) {
          return v8::ThrowException(v8::Exception::Error(
              v8::String::New(uv_strerror(uv_last_error(uv_default_loop())))));
        }
        ret = false;
      }

      mq_getattr(obj->mqdes, &(obj->mqattrs));

      return scope.Close(v8::Boolean::New(ret));
    }

    static v8::Handle<v8::Value> Receive(const v8::Arguments& args) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(args.This());
      ssize_t nBytes;
      uint32_t priority;
      bool retTuple = false;
      v8::Local<v8::Value> ret;

      if (args.Length() < 1) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("Expected at least 1 argument")));
      } else if (!node::Buffer::HasInstance(args[0])) {
        return v8::ThrowException(v8::Exception::TypeError(
            v8::String::New("First argument must be a node::Buffer")));
      } else if (args.Length() > 1)
        retTuple = args[1]->BooleanValue();

      v8::Local<v8::Object> buf = args[0]->ToObject();
      if ((nBytes = mq_receive(obj->mqdes, node::Buffer::Data(buf),
                               node::Buffer::Length(buf), &priority)) == -1) {
        if (errno != EAGAIN) {
          return v8::ThrowException(v8::Exception::Error(
              v8::String::New(uv_strerror(uv_last_error(uv_default_loop())))));
        }
        ret = v8::Local<v8::Value>::New(v8::Boolean::New(false));
      } else if (!retTuple)
        ret = v8::Integer::New(nBytes);
      else {
        v8::Local<v8::Array> tuple = v8::Array::New(2);
        tuple->Set(0, v8::Integer::New(nBytes));
        tuple->Set(1, v8::Integer::New(priority));
        ret = tuple;
      }

      mq_getattr(obj->mqdes, &(obj->mqattrs));

      return scope.Close(ret);
    }

    static v8::Handle<v8::Value> MsgsizeGetter (v8::Local<v8::String> property, const v8::AccessorInfo& info) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(info.This());

      mq_getattr(obj->mqdes, &(obj->mqattrs));

      return scope.Close(v8::Integer::New(obj->mqattrs.mq_msgsize));
    }

    static v8::Handle<v8::Value> MaxmsgsGetter (v8::Local<v8::String> property, const v8::AccessorInfo& info) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(info.This());

      mq_getattr(obj->mqdes, &(obj->mqattrs));

      return scope.Close(v8::Integer::New(obj->mqattrs.mq_maxmsg));
    }

    static v8::Handle<v8::Value> CurmsgsGetter (v8::Local<v8::String> property, const v8::AccessorInfo& info) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(info.This());

      mq_getattr(obj->mqdes, &(obj->mqattrs));

      return scope.Close(v8::Integer::New(obj->mqattrs.mq_curmsgs));
    }

    static v8::Handle<v8::Value> IsfullGetter (v8::Local<v8::String> property, const v8::AccessorInfo& info) {
      v8::HandleScope scope;
      PosixMQ* obj = node::ObjectWrap::Unwrap<PosixMQ>(info.This());

      mq_getattr(obj->mqdes, &(obj->mqattrs));

      return scope.Close(v8::Boolean::New(obj->mqattrs.mq_curmsgs == obj->mqattrs.mq_maxmsg));
    }

    static void Initialize(v8::Handle<v8::Object> target) {
      v8::HandleScope scope;

      v8::Local<v8::FunctionTemplate> tpl = v8::FunctionTemplate::New(New);
      v8::Local<v8::String> name = v8::String::NewSymbol("PosixMQ");

      constructor = v8::Persistent<v8::FunctionTemplate>::New(tpl);
      constructor->InstanceTemplate()->SetInternalFieldCount(1);
      constructor->SetClassName(name);

      NODE_SET_PROTOTYPE_METHOD(constructor, "open", Open);
      NODE_SET_PROTOTYPE_METHOD(constructor, "close", Close);
      NODE_SET_PROTOTYPE_METHOD(constructor, "push", Send);
      NODE_SET_PROTOTYPE_METHOD(constructor, "shift", Receive);
      NODE_SET_PROTOTYPE_METHOD(constructor, "unlink", Unlink);

      constructor->PrototypeTemplate()->SetAccessor(v8::String::New("msgsize"),
                                                    MsgsizeGetter);
      constructor->PrototypeTemplate()->SetAccessor(v8::String::New("maxmsgs"),
                                                    MaxmsgsGetter);
      constructor->PrototypeTemplate()->SetAccessor(v8::String::New("curmsgs"),
                                                    CurmsgsGetter);
      constructor->PrototypeTemplate()->SetAccessor(v8::String::New("isFull"),
                                                    IsfullGetter);
      emit_symbol = NODE_PSYMBOL("emit");
      read_emit_argv[0] = NODE_PSYMBOL("messages");
      write_emit_argv[0] = NODE_PSYMBOL("drain");
      target->Set(name, constructor->GetFunction());
    }
};

extern "C" {
  void init(v8::Handle<v8::Object> target) {
    v8::HandleScope scope;
    PosixMQ::Initialize(target);
  }

  NODE_MODULE(posixmq, init);
}
