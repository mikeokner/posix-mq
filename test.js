/* Create a new queue accessible by all, fill it up, and then close it. */
var PosixMQ = require('./build/Release/posix-mq').PosixMQ;
var mq, writebuf, r;

mq = new PosixMQ();
mq.open({
    name: '/pmqtest',
    create: true,
    mode: '0777'
});
writebuf = new Buffer(1);
do {
    writebuf[0] = Math.floor(Math.random() * 93) + 33;
} while ((r = mq.push(writebuf)) !== false);
mq.close();

/* Open an existing queue, read messages, and then close. */
mq = new PosixMQ();
mq.on('messages', function() {
    var n;
    while ((n = this.shift(readbuf)) !== false) {
        console.log('Received message ('+ n +' bytes): '+ readbuf.toString('utf8', 0, n));
        console.log('Messages left: '+ this.curmsgs);
    }
    this.unlink();
    this.close();
});
mq.open({name: '/pmqtest'});
readbuf = new Buffer(mq.msgsize);
