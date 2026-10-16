// run_browser.mjs: one native station + one headless-browser station in a fresh room.
//   node run_browser.mjs [frames] [--mdns] [--native-first]  (env H2NET_TEST_EXE, H2NET_BROWSER)
// Serves net/ over http://127.0.0.1:8765, opens test/test.html in headless
// Edge/Chrome, reads window.h2result over the DevTools protocol.
// By default the browser is started with mDNS host-candidate hiding off
// (a page without camera permission otherwise hides its LAN address behind a
// .local name that libjuice cannot resolve); --mdns keeps Chrome's default.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import crypto from 'node:crypto';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const frames = +(process.argv.find((a) => /^\d+$/.test(a)) || 1000);
const mdns = process.argv.includes('--mdns');
const exe = process.env.H2NET_TEST_EXE || 'D:/n64work/halo2/build/net/h2net_test.exe';
const browser = process.env.H2NET_BROWSER || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const room = 'h2t-' + crypto.randomBytes(4).toString('hex');
const PORT = 8765, CDP = 9333;
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript' };

const server = http.createServer((req, res) => {
  const p = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!p.startsWith(root) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': types[path.extname(p)] || 'application/octet-stream' });
  fs.createReadStream(p).pipe(res);
}).listen(PORT, '127.0.0.1');

const profile = 'D:/n64work/halo2/build/net/browser-profile';
const url = `http://127.0.0.1:${PORT}/test/test.html?room=${room}&idx=9&n=2&frames=${frames}`;
const args = ['--headless=new', `--remote-debugging-port=${CDP}`, `--user-data-dir=${profile}`, '--no-first-run',
  '--no-default-browser-check', '--disable-background-timer-throttling', '--disable-renderer-backgrounding'];
if (!mdns) args.push('--disable-features=WebRtcHideLocalIpsWithMdns');
console.log(`room=${room} frames=${frames} mdns-hiding=${mdns ? 'on' : 'off'}`);
const br = spawn(browser, [...args, url], { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function evalPage(expr) {
  const list = await (await fetch(`http://127.0.0.1:${CDP}/json`)).json();
  const pg = list.find((t) => t.type === 'page' && t.url.includes('test.html'));
  if (!pg) return null;
  const ws = new WebSocket(pg.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const reply = new Promise((r) => { ws.onmessage = (e) => r(JSON.parse(e.data)); });
  ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: expr, returnByValue: true } }));
  const m = await reply;
  ws.close();
  return m.result && m.result.result ? m.result.result.value : null;
}

// --native-first: native joins first and offers; default: the page joins first and offers
if (!process.argv.includes('--native-first')) {
  for (let i = 0; i < 60; i++) {
    await sleep(500);
    try { if (await evalPage('window.h2net ? window.h2net.self : 0')) break; } catch { /* not up yet */ }
  }
}
const nat = spawn(exe, [room, '1', '2', String(frames)], { stdio: ['ignore', 'pipe', 'pipe'] });
let natOut = '';
nat.stdout.on('data', (d) => { natOut += d; });
nat.stderr.on('data', (d) => { natOut += d; });
const natDone = new Promise((r) => nat.on('exit', (c) => r(c)));

let page = null;
const t0 = Date.now();
while (Date.now() - t0 < 120000) {
  await sleep(1000);
  try { page = await evalPage('window.h2result && JSON.stringify(window.h2result)'); } catch { page = null; }
  if (page) break;
}
const code = await Promise.race([natDone, sleep(30000).then(() => 'timeout')]);
console.log(natOut.split('\n').filter((l) => l && !l.startsWith('[h2net')).join('\n'));
console.log(page ? JSON.parse(page).text : 'browser: no result (timeout)');
console.log(code === 0 && page && JSON.parse(page).ok ? 'ALL PASS' : 'SOME FAILED');
try { nat.kill(); } catch { /* */ }
br.kill();
server.close();
process.exit(0);
