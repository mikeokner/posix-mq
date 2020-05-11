## Description

[![NPM](https://nodei.co/npm/posix-mq.png)](https://npmjs.org/package/posix-mq)
[![Build Status](https://travis-ci.org/mikeokner/posix-mq.svg)](https://travis-ci.org/mikeokner/posix-mq)

A [node.js](http://nodejs.org/) library for using POSIX message queues.
Originally forked from mscdex/pmq to provide additional customization of flags
passed to `mq_open()`. Subsequently re-written to support v0.12 and v4+ using
[Native Abstractions for Node.js](https://github.com/nodejs/nan).


## Requirements

* [node.js](http://nodejs.org/) -- Tested against v0.8, v0.10, v0.12, v4, v6, v8, v10, v12, v13, v14

* Linux 2.6.6+ or FreeBSD kernel with POSIX message queue support compiled in (`CONFIG_POSIX_MQUEUE`, which is enabled by default)

* See `man mq_overview` for how/where to modify global POSIX message queue resource limits

* Depends on [nan](https://www.npmjs.com/package/nan) which will be automatically installed when running `npm install posix-mq`.


## Install

```console
$ npm install posix-mq
```


## Examples

* Create a new queue accessible by all, fill it up, and then close it:

```javascript
const PosixMQ = require('posix-mq');
const mq = new PosixMQ();
mq.open({
    name: '/pmqtest',
    create: true,
    mode: '0777',
    maxmsgs: 10,
    msgsize: 8
});
var writebuf = Buffer.alloc(1);
var r;
do {
    writebuf[0] = Math.floor(Math.random() * 93) + 33;
    console.log("Writing "+ writebuf[0] +" ('"+ String.fromCharCode(writebuf[0]) +"') to the queue...");
} while ((r = mq.push(writebuf)) !== false);
mq.close();
```

* Open an existing queue, read all of its messages, and then remove it from the system and close it:

```javascript
const PosixMQ = require('posix-mq');
const mq = new PosixMQ();
mq.on('messages', function() {
    var n;
    while ((n = this.shift(readbuf)) !== false) {
        console.log("Received message ("+ n +" bytes): "+ readbuf.toString('utf8', 0, n));
        console.log("Messages left: "+ this.curmsgs);
    }
    this.unlink();
    this.close();
});
mq.open({name: '/pmqtest'});
readbuf = Buffer.alloc(mq.msgsize);
```

* Open an existing queue and continuously listen for new messages:

```javascript
const PosixMQ = require('./lib/index');
const mq = new PosixMQ();

// Open the queue and allocate the buffer to read messages into
mq.open({name: '/pmqtest'});
readbuf = Buffer.alloc(mq.msgsize);

// Define the handler function to read all messages currently in the queue
handleMsg = () => {
    let n;
    while ((n = mq.shift(readbuf)) !== false) {
        let msg = readbuf.toString('utf8', 0, n);
        console.log("Received message("+ n +" bytes): " + msg);
        console.log("Messages left: "+ mq.curmsgs);
    }
};

// Call the handler function once before binding the handler to ensure
// all existing messages are read
handleMsg();

// Bind the handler now that the queue has been emptied by the previous invocation
mq.on('messages', handleMsg);
```


## API

### Events

* **messages**() - Emitted every time the queue goes from empty to having at least one message.

_Note: According to [the man page for mq_notify](https://www.systutorials.com/docs/linux/man/3-mq_notify/):_

> Message notification occurs only when a new message arrives and the queue was
> previously empty. If the queue was not empty at the time mq_notify() was
> called, then a notification will occur only after the queue is emptied and a
> new message arrives.

_Therefore, the queue must be empty when assigning the `mq.on('messages',
func)` handler.  You should first read any available messages by calling
`mq.shift` before assigning the handler._

* **drain**() - Emitted when there is room for at least one message in the queue.

### Properties (read-only)

* **isFull** - _boolean_ - Convenience property that returns true if `curmsgs` === `maxmsgs`.

* **maxmsgs** - _integer_ - The maximum number of messages in the queue.

* **msgsize** - _integer_ - The maximum size of messages in the queue.

* **curmsgs** - _integer_ - The number of messages currently in the queue.

### Methods

* **(constructor)**() - Creates and returns a new PosixMQ instance.

* **open**(<_object_>config) - _(void)_ - Connects to a queue. Valid properties in `config` are:

    * name - _string_ - The name of the queue to open, it **MUST** start with a '/'.

    * create - _boolean_ - Set to `true` to create the queue if it doesn't already exist (default is `false`). The queue will be owned by the user and group of the current process.

    * exclusive - _boolean_ - If creating a queue, set to true if you want to ensure a queue with the given name does not already exist.

    * mode - _mixed_ - If creating a queue, this is the permissions to use. This can be an octal string (e.g. '0777') or an integer.

    * maxmsgs - _integer_ - If creating a queue, this is the maximum number of messages the queue can hold. This value is subject to the system limits in place and defaults to 10.

    * msgsize - _integer_ - If creating a queue, this is the maximum size of each message (in bytes) in the queue. This value is subject to the system limits in place and defaults to 8192 bytes.

    * flags - _integer_ - Default is `O_RDWR | O_NONBLOCK` (2050). If a different set of flags is required, its integer value may be provided here. See the man page for `mq_open` for more information.
    
* **close**() - _(void)_ - Disconnects from the queue.

* **unlink**() - _(void)_ - Removes the queue from the system.

* **push**(< _Buffer_ or _string_ >data[, < _integer_ >priority]) - _boolean_ - Pushes a message with the contents of `data` onto the queue with the optional `priority` (defaults to 0). `data` is either a string or Buffer object. `priority` is an integer between 0 and 31 inclusive.

* **shift**(< _Buffer_ >readbuf[, < _boolean_ >returnTuple]) - _mixed_ - Shifts the next message off the queue and stores it in `readbuf`. If `returnTuple` is set to true, an array containing the number of bytes in the shifted message and the message's priority are returned, otherwise just the number of bytes is returned (default). If there was nothing on the queue, false is returned.
