'use strict';

const assert = require('assert');
const {
    Worker,
    isMainThread,
    parentPort,
    workerData
} = require('worker_threads');

const TEST_TIMEOUT_MS = 15000;

function queueName(suffix) {
    return '/posix-mq-worker-' + process.pid + '-' + suffix;
}

function waitForWorker(worker, expectedMessage) {
    return new Promise(function(resolve, reject) {
        let messageReceived = false;
        const timeout = setTimeout(function() {
            worker.terminate();
            reject(new Error('Worker timed out'));
        }, TEST_TIMEOUT_MS);

        worker.once('error', function(error) {
            clearTimeout(timeout);
            reject(error);
        });
        worker.once('message', function(message) {
            try {
                assert.deepStrictEqual(message, expectedMessage);
                messageReceived = true;
            }
            catch (error) {
                clearTimeout(timeout);
                reject(error);
            }
        });
        worker.once('exit', function(code) {
            clearTimeout(timeout);
            if (code !== 0) {
                reject(new Error('Worker exited with code ' + code));
            }
            else if (!messageReceived) {
                reject(new Error('Worker exited without sending its result'));
            }
            else {
                resolve();
            }
        });
    });
}

function runWorker(data, expectedMessage) {
    const worker = new Worker(__filename, { workerData: data });
    return waitForWorker(worker, expectedMessage);
}

function terminateOpenWorker(name) {
    return new Promise(function(resolve, reject) {
        const worker = new Worker(__filename, {
            workerData: { mode: 'terminate', name: name }
        });
        const timeout = setTimeout(function() {
            worker.terminate();
            reject(new Error('Termination worker timed out'));
        }, TEST_TIMEOUT_MS);

        worker.once('error', function(error) {
            clearTimeout(timeout);
            reject(error);
        });
        worker.once('message', function(message) {
            if (message !== 'ready') {
                clearTimeout(timeout);
                worker.terminate();
                reject(new Error('Unexpected termination worker message'));
                return;
            }
            worker.terminate();
        });
        worker.once('exit', function() {
            clearTimeout(timeout);
            resolve();
        });
    });
}

async function runMainThread() {
    const PosixMQ = require('../lib/index');
    assert.strictEqual(typeof PosixMQ, 'function');

    const prefix = Date.now().toString(36);
    await collectOpenQueues(PosixMQ, prefix + '-main-gc');

    const eventWorkers = [];
    for (let i = 0; i < 4; i++) {
        const payload = 'worker-' + i;
        eventWorkers.push(runWorker({
            mode: 'event',
            name: queueName(prefix + '-event-' + i),
            payload: payload
        }, payload));
    }
    await Promise.all(eventWorkers);

    await runWorker({
        mode: 'reopen',
        firstName: queueName(prefix + '-reopen-1'),
        secondName: queueName(prefix + '-reopen-2')
    }, 'reopened');

    await runWorker({
        mode: 'gc',
        prefix: prefix
    }, 'collected');

    for (let i = 0; i < 4; i++) {
        await terminateOpenWorker(queueName(prefix + '-terminate-' + i));
    }

    console.log('worker_threads tests passed');
}

function openQueue(PosixMQ, name) {
    const mq = new PosixMQ();
    mq.open({
        name: name,
        create: true,
        exclusive: true,
        mode: '0700',
        maxmsgs: 10,
        msgsize: 64
    });
    return mq;
}

async function collectOpenQueues(PosixMQ, prefix) {
    assert.strictEqual(typeof global.gc, 'function', 'Run this test with --expose-gc');

    for (let i = 0; i < 32; i++) {
        let mq = openQueue(PosixMQ, queueName(prefix + '-' + i));
        mq.unlink();
        mq = null;
    }

    for (let i = 0; i < 8; i++) {
        global.gc();
        await new Promise(function(resolve) {
            setImmediate(resolve);
        });
    }
}

function runEventWorker(PosixMQ) {
    const mq = openQueue(PosixMQ, workerData.name);
    const readBuffer = Buffer.alloc(mq.msgsize);

    mq.once('messages', function() {
        try {
            const bytesRead = mq.shift(readBuffer);
            const message = readBuffer.toString('utf8', 0, bytesRead);
            mq.unlink();
            mq.close();
            parentPort.postMessage(message);
        }
        catch (error) {
            throw error;
        }
    });

    setTimeout(function() {
        mq.push(workerData.payload);
    }, 10);
}

function runReopenWorker(PosixMQ) {
    const mq = openQueue(PosixMQ, workerData.firstName);
    mq.unlink();
    mq.close();

    mq.open({
        name: workerData.secondName,
        create: true,
        exclusive: true,
        mode: '0700',
        maxmsgs: 10,
        msgsize: 64
    });
    mq.unlink();
    mq.close();
    parentPort.postMessage('reopened');
}

async function runGcWorker(PosixMQ) {
    await collectOpenQueues(PosixMQ, workerData.prefix + '-worker-gc');
    parentPort.postMessage('collected');
}

function runTerminateWorker(PosixMQ) {
    const mq = openQueue(PosixMQ, workerData.name);
    mq.unlink();
    parentPort.postMessage('ready');
    setInterval(function() {
        mq.curmsgs;
    }, 1000);
}

async function runWorkerThread() {
    const PosixMQ = require('../lib/index');

    if (workerData.mode === 'event') {
        runEventWorker(PosixMQ);
    }
    else if (workerData.mode === 'reopen') {
        runReopenWorker(PosixMQ);
    }
    else if (workerData.mode === 'gc') {
        await runGcWorker(PosixMQ);
    }
    else if (workerData.mode === 'terminate') {
        runTerminateWorker(PosixMQ);
    }
    else {
        throw new Error('Unknown worker mode: ' + workerData.mode);
    }
}

const test = isMainThread ? runMainThread() : runWorkerThread();
test.catch(function(error) {
    setImmediate(function() {
        throw error;
    });
});
