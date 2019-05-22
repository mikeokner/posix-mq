//var PosixMQ = require('posix-mq');
var PosixMQ = require('./lib/index');

/* Create a new queue accessible by all, fill it up, and then close it. */
var mq = new PosixMQ();
mq.open({
    name: '/pmqtest',
    create: true,
    mode: '0777',
    maxmsgs: 10,
    msgsize: 8
});
var writebuf = new Buffer.alloc(1);
var r;
do {
    writebuf[0] = Math.floor(Math.random() * 93) + 33;
    console.log("Writing "+ writebuf[0] +" ('"+ String.fromCharCode(writebuf[0]) +"') to the queue...");
} while ((r = mq.push(writebuf)) !== false);
mq.close();

/* Open an existing queue, read messages, and then close. */
var mq = new PosixMQ();
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
readbuf = new Buffer.alloc(mq.msgsize);
