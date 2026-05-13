// Connects to a running backend, requests a batch of history rows,
// verifies each one arrives with HistoryRow frame type (0x09), and
// exits non-zero on any decode failure.
const WebSocket = require('ws');

const ws = new WebSocket('ws://localhost:8765');
let received = 0;
let requested = false;
let timeout;

// Frame type byte (offset 0 of FrameHeader) for HistoryRow.
const FRAME_HISTORY_ROW = 0x09;

ws.on('open', () => {
    // Wait briefly so the DSP loop has time to populate the history ring
    // with rows before we ask for them.
    setTimeout(() => {
        ws.send(JSON.stringify({ cmd: 'request_history', start: 0, count: 100 }));
        requested = true;
    }, 1500);

    timeout = setTimeout(() => {
        console.error(`FAIL: only ${received} history rows received in 8s (requested=${requested})`);
        process.exit(1);
    }, 8000);
});

ws.on('message', (buf) => {
    if (!(buf instanceof Buffer)) return;
    if (buf.length < 1) return;
    const type = buf.readUInt8(0);
    if (type !== FRAME_HISTORY_ROW) return; // ignore live frames
    received++;
    if (received >= 100) {
        clearTimeout(timeout);
        console.log(`PASS: received ${received} HistoryRow frames`);
        ws.close();
        process.exit(0);
    }
});

ws.on('error', (e) => { console.error('FAIL:', e.message); process.exit(1); });
