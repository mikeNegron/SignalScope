// Integration test: confirms the backend exits cleanly with a stable
// "[ws] fatal:" line on stderr when its WS port is already in use.
// Runs without Electron — directly fork+execs the backend binary.

import { describe, it, expect } from 'vitest';
import { spawn } from 'child_process';
import net from 'net';
import path from 'path';

const BACKEND_BIN = path.resolve(
    process.cwd(),
    'build/backend-simple/signalscope-backend'
);
const TEST_PORT = 18763;  // distinct from the prod default 8765

function waitForExit(proc, timeoutMs) {
    return new Promise((resolve, reject) => {
        const t = setTimeout(() => {
            proc.kill('SIGKILL');
            reject(new Error('backend did not exit within ' + timeoutMs + 'ms'));
        }, timeoutMs);
        proc.on('exit', (code) => { clearTimeout(t); resolve(code); });
    });
}

describe('backend startup failures', () => {
    it('exits with [ws] fatal: on stderr when port is in use', async () => {
        // Occupy the port.
        const holder = net.createServer().listen(TEST_PORT);
        await new Promise((r) => holder.once('listening', r));

        try {
            const proc = spawn(BACKEND_BIN, ['--test', '--port=' + TEST_PORT]);
            let stderr = '';
            proc.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
            proc.stdout.on('data', () => {});  // drain stdout

            const code = await waitForExit(proc, 3000);
            expect(stderr).toMatch(/\[ws\] fatal:.*(bind|listen) failed on port 18763/);
            expect(typeof code).toBe('number');
        } finally {
            holder.close();
        }
    });
});
