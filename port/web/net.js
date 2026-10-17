// Open Halo 2 (web): system link.
//
// The program's network controller model puts every Ethernet frame the game
// sends into a ring in its own (shared) memory and reads received frames from
// a second ring: see the __EMSCRIPTEN__ branch of port/xr/src/nvnet.c for the
// layout. A worker started here waits on the transmit ring (Atomics.wait, so
// a frame leaves at once and a background tab's throttled timers do not
// matter) and carries the frames:
//   ?link=bc[:room]        BroadcastChannel: other tabs of this browser on this
//                          origin, no server at all (the default room is "lan")
//   ?link=ws://host/path   WebSocket, one binary message per frame; the relay
//                          forwards each message to everyone else in the room
//   ?link=room:<code>      online system link: peer to peer over WebRTC data
//                          channels with everyone in room <code>, native copies
//                          included (net/h2net.js; the skatemods relay only
//                          introduces the peers). RTCPeerConnection exists only
//                          on the page, so the worker hands frames across.
//   ?link=openlive         Open Live (with ?env=RECOMP_OPEN_LIVE=1~...): no room
//                          at first; the program joins and leaves session rooms
//                          as the title hosts and joins games (net/h2net_web.c
//                          calls window.h2rooms), one h2net.js mesh per room,
//                          all of them one segment.
// Without ?link the game has a cable with nothing on the other end.
// The game tells stations apart by Ethernet address only (its packets all
// carry the address 0.0.0.1), so a hub that floods is all it needs.
(function () {
  'use strict';
  const link = new URLSearchParams(location.search).get('link');
  if (!link) return;

  const WORKER = `
    let head, headI, tx, rx, size, mask, send = null, sent = 0, got = 0, dropped = 0;
    function deliver(frame) {
      const n = frame.length, w = Atomics.load(head, 2), r = Atomics.load(head, 3);
      if (n < 14 || n > 1600 || size - ((w - r) >>> 0) < n + 2) { dropped++; Atomics.add(head, 5, 1); return; }
      rx[w & mask] = n & 255; rx[(w + 1) & mask] = n >> 8;
      for (let i = 0; i < n; i++) rx[(w + 2 + i) & mask] = frame[i];
      Atomics.store(head, 2, (w + 2 + n) >>> 0);
      got++;
    }
    function pump() {
      for (;;) {
        const w = Atomics.load(head, 0); let r = Atomics.load(head, 1);
        while (r !== w) {
          const n = tx[r & mask] | tx[(r + 1) & mask] << 8;
          const f = new Uint8Array(n);
          for (let i = 0; i < n; i++) f[i] = tx[(r + 2 + i) & mask];
          r = (r + 2 + n) >>> 0;
          Atomics.store(head, 1, r);
          if (send) { send(f); sent++; }
        }
        // waitAsync keeps this worker's message loop alive for arrivals
        const res = Atomics.waitAsync(headI, 0, w | 0, 250);
        if (res.async) { res.value.then(pump); return; }
      }
    }
    onmessage = (e) => {
      const m = e.data;
      if (m.rx) { deliver(new Uint8Array(m.rx)); return; }   // from the page's WebRTC mesh
      head = new Uint32Array(m.sab, m.ptr, 8);
      headI = new Int32Array(m.sab, m.ptr, 8);    // Atomics.waitAsync takes signed views only
      size = head[4]; mask = size - 1;
      tx = new Uint8Array(m.sab, m.ptr + 32, size);
      rx = new Uint8Array(m.sab, m.ptr + 32 + size, size);
      if (m.link.startsWith('room:') || m.link === 'openlive') {
        send = (f) => postMessage({ tx: f.buffer }, [f.buffer]);
      } else if (m.link.startsWith('ws')) {
        let ws = null;
        const open = () => {
          ws = new WebSocket(m.link);
          ws.binaryType = 'arraybuffer';
          ws.onopen = () => postMessage({ log: 'link: relay connected' });
          ws.onmessage = (ev) => { if (ev.data instanceof ArrayBuffer) deliver(new Uint8Array(ev.data)); };
          ws.onclose = () => { postMessage({ log: 'link: relay closed, retrying' }); setTimeout(open, 2000); };
        };
        open();
        send = (f) => { if (ws && ws.readyState === 1) ws.send(f); };
      } else {
        const room = m.link.split(':')[1] || 'lan';
        const bc = new BroadcastChannel('openhalo2-link-' + room);
        bc.onmessage = (ev) => deliver(new Uint8Array(ev.data));
        send = (f) => bc.postMessage(f.buffer, [f.buffer]);
        postMessage({ log: 'link: tab-to-tab room "' + room + '"' });
      }
      setInterval(() => postMessage({ stats: { sent, got, dropped } }), 5000);
      pump();
    };`;

  let started = false;
  function start() {
    if (started) return;
    if (typeof Module === 'undefined' || !Module.h2ready || typeof HEAPU8 === 'undefined') { setTimeout(start, 200); return; }
    const ptr = Module._xbox_web_nic_buffer ? Module._xbox_web_nic_buffer() : 0;
    if (!ptr) { console.log('link: this build has no network ring'); return; }
    started = true;
    const w = new Worker(URL.createObjectURL(new Blob([WORKER], { type: 'text/javascript' })));
    let mesh = null;
    if (link === 'openlive') {
      // one mesh per room; a frame from the game goes to every room (each
      // mesh sends unicast to the peer that MAC was last seen behind)
      import('./h2net.js').then(({ H2Net }) => {
        const rooms = new Map();
        const toGame = (f) => { const c = f.slice(); w.postMessage({ rx: c.buffer }, [c.buffer]); };
        mesh = { send: (f) => { for (const r of rooms.values()) r.send(f); } };
        window.h2rooms = {
          join(code) {
            if (rooms.has(code)) return;
            const r = new H2Net();
            r.onframe = toGame;
            try { r.open(code); rooms.set(code, r); console.log('openlive: room ' + code + ' joined'); }
            catch (err) { console.log('openlive: room ' + code + ': ' + err); }
          },
          leave(code) {
            const r = rooms.get(code);
            if (r) { r.close(); rooms.delete(code); console.log('openlive: room ' + code + ' left'); }
          },
          full(code) { const r = rooms.get(code); return !!(r && r.err === 'room full'); },
          peers(code) {
            let n = 0;
            for (const [k, r] of rooms) if (!code || k === code) n += r.peers();
            return n;
          },
          status() { return [...rooms].map(([k, r]) => k + ': ' + r.status()).join(' | '); },
        };
        for (const [op, code] of (window.h2roomq || [])) window.h2rooms[op](code);
        window.h2roomq = [];
        console.log('link: Open Live, rooms joined per session');
      }).catch((err) => console.log('link: h2net.js failed: ' + err));
    } else if (link.startsWith('room:')) {
      import('./h2net.js').then(({ H2Net }) => {
        mesh = new H2Net();
        mesh.onframe = (f) => { const c = f.slice(); w.postMessage({ rx: c.buffer }, [c.buffer]); };
        mesh.open(link.slice(5));
        window.h2mesh = mesh;
        console.log('link: online room "' + link.slice(5) + '"');
      }).catch((err) => console.log('link: h2net.js failed: ' + err));
    }
    w.onmessage = (e) => {
      if (e.data.tx) { if (mesh) mesh.send(new Uint8Array(e.data.tx)); return; }
      if (e.data.log) console.log(e.data.log);
      if (e.data.stats) {
        window.h2link = e.data.stats;
        console.log('link: sent ' + e.data.stats.sent + ' received ' + e.data.stats.got + ' dropped ' + e.data.stats.dropped);
      }
    };
    w.postMessage({ sab: HEAPU8.buffer, ptr, link });
  }
  start();
})();
