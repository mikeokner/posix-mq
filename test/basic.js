const PosixMQ = require('../lib/index');
const Buffer = require('safer-buffer').Buffer;

const queueName = '/pmqtest-' + process.pid;

// Open the queue, fill it up, and close it
let mq = new PosixMQ();
mq.open({
    name: queueName,
    create: true,
    exclusive: true,
    mode: '0777',
    maxmsgs: 10,
    msgsize: 8
});
const writebuf = Buffer.alloc(1);
let r;

// Fill up the queue
do {
    writebuf[0] = Math.floor(Math.random() * 93) + 33;
    console.log("Writing "+ writebuf[0] +" ('"+ String.fromCharCode(writebuf[0]) +"') to the queue...");
} while ((r = mq.push(writebuf)) !== false);
mq.close();

// Open an existing queue, read messages, and then remove & close.
mq = new PosixMQ();
let readbuf;
mq.on('messages', function() {
    let n;
    while ((n = this.shift(readbuf)) !== false) {
        const msg = readbuf.toString('utf8', 0, n);
        console.log("Received message ("+ n +" bytes): "+ msg);
        console.log("Messages left: "+ this.curmsgs);
    }
    this.unlink();
    this.close();
});
mq.open({name: queueName});
readbuf = Buffer.alloc(mq.msgsize);
