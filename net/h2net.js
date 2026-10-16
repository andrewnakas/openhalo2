// h2net (browser): peer-to-peer Ethernet frame transport for Open Halo 2
// system link. Same wire protocol as the native h2net.c, so browser and
// native stations share one mesh.
//
//   import { H2Net } from './h2net.js';
//   const net = new H2Net();                 // or new H2Net({ relay, iceServers, rxMax })
//   net.open('h2-myroom');
//   net.send(frameUint8Array);               // one Ethernet frame
//   const f = net.recv();                    // Uint8Array or null
//   net.status(); net.peers(); await net.stats(); net.close();
//   net.onframe = (f) => ...                 // optional push instead of recv()
//
// Signaling goes through the room relay (wss://skatemods.com/rooms/room/<code>):
// binary messages are u32 LE member id + payload, payload = one type byte +
// UTF-8 ('o' offer SDP, 'a' answer SDP, 'c' "<mid>\n<candidate>"). The lower
// member id offers. Game data never touches the relay: one RTCDataChannel per
// peer, unordered with maxRetransmits 0, one message per frame. Unicast goes
// to the peer the destination MAC was last seen from; unknown and group
// destinations are flooded to all open peers.
//
// RTCPeerConnection exists only on the page (not in workers): run this on the
// main thread and move frames to/from the emulator's rings with postMessage
// or by draining recv() into the receive ring.

export const DEFAULT_RELAY = 'wss://skatemods.com/rooms';
export const DEFAULT_ICE = [
  { urls: 'stun:stun.l.google.com:19302' },
  { urls: 'stun:stun.cloudflare.com:3478' },
];
const FRAME_MAX = 2048;
const enc = new TextEncoder();
const dec = new TextDecoder();

export class H2Net {
  constructor(opts = {}) {
    this.relay = (opts.relay || DEFAULT_RELAY).replace(/\/+$/, '');
    this.iceServers = opts.iceServers || DEFAULT_ICE;
    this.rxMax = opts.rxMax || 1024;
    this.onframe = null;
    this._reset();
  }

  _reset() {
    this.ws = null;
    this.wsState = 'closed';
    this.self = 0;
    this.host = 0;
    this.peerMap = new Map();     // member id -> peer
    this.macs = new Map();        // 'xx:xx:..' -> member id
    this.rxq = [];
    this.counters = { tx: 0, rx: 0, drop: 0, flood: 0 };
    this.err = '';
  }

  open(room) {
    if (this.ws) throw new Error('h2net: already open');
    if (!/^[A-Za-z0-9_-]{4,24}$/.test(room)) throw new Error('h2net: bad room code');
    this._reset();
    const ws = new WebSocket(`${this.relay}/room/${room}`);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    this.wsState = 'connecting';
    ws.onopen = () => { this.wsState = 'open'; };
    ws.onclose = () => { if (this.ws === ws) this.wsState = 'closed'; };
    ws.onerror = () => { if (this.ws === ws) { this.wsState = 'error'; this.err = 'websocket error'; } };
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') this._notice(JSON.parse(e.data));
      else if (e.data.byteLength >= 5) {
        const from = new DataView(e.data).getUint32(0, true);
        const u = new Uint8Array(e.data);
        this._signal(from, String.fromCharCode(u[4]), dec.decode(u.subarray(5)));
      }
    };
    return this;
  }

  send(f) {
    if (!(f instanceof Uint8Array) || f.length < 14 || f.length > FRAME_MAX) return;
    const target = (f[0] & 1) ? 0 : (this.macs.get(macKey(f, 0)) || 0);
    let n = 0;
    for (const p of this.peerMap.values()) {
      if (!p.open || (target && p.id !== target)) continue;
      try { p.dc.send(f); p.tx++; n++; } catch { /* closing */ }
    }
    if (n) this.counters.tx++;
    if (n && !target) this.counters.flood++;
  }

  recv() { return this.rxq.length ? this.rxq.shift() : null; }

  peers() {
    let n = 0;
    for (const p of this.peerMap.values()) if (p.open) n++;
    return n;
  }

  status() {
    const ps = [...this.peerMap.values()];
    let s = `relay=${this.wsState} self=${this.self} host=${this.host} peers=${this.peers()}/${ps.length}`;
    for (const p of ps) s += ` [${p.id}:${p.open ? 'open' : p.pc.connectionState}${p.pair ? ' ' + p.pair : ''}]`;
    const c = this.counters;
    s += ` tx=${c.tx} rx=${c.rx} drop=${c.drop} flood=${c.flood}`;
    if (this.err) s += ` err="${this.err}"`;
    return s;
  }

  // Refreshes each peer's selected candidate pair ("host-srflx" etc., local-remote) and RTT.
  async stats() {
    const out = [];
    for (const p of this.peerMap.values()) {
      try {
        const r = await p.pc.getStats();
        let pairId = null;
        r.forEach((s) => { if (s.type === 'transport' && s.selectedCandidatePairId) pairId = s.selectedCandidatePairId; });
        let pair = null;
        r.forEach((s) => {
          if (s.type === 'candidate-pair' && (s.id === pairId || (!pairId && s.nominated && s.state === 'succeeded'))) pair = s;
        });
        if (pair) {
          const l = r.get(pair.localCandidateId), m = r.get(pair.remoteCandidateId);
          p.pair = `${l ? l.candidateType : '?'}-${m ? m.candidateType : '?'}`;
          p.rtt = pair.currentRoundTripTime;
        }
      } catch { /* closed */ }
      out.push({ id: p.id, open: p.open, pair: p.pair || null, rttMs: p.rtt != null ? p.rtt * 1000 : null });
    }
    return out;
  }

  close() {
    const ws = this.ws;
    this.ws = null;
    if (ws) try { ws.close(); } catch { /* */ }
    for (const id of [...this.peerMap.keys()]) this._drop(id);
    this.wsState = 'closed';
  }

  // ---- signaling ----------------------------------------------------------

  _sig(target, type, text) {
    const body = enc.encode(text);
    const m = new Uint8Array(5 + body.length);
    new DataView(m.buffer).setUint32(0, target, true);
    m[4] = type.charCodeAt(0);
    m.set(body, 5);
    if (this.ws && this.ws.readyState === 1) this.ws.send(m);
  }

  _notice(n) {
    if (n.type === 'welcome') { this.self = n.self; this.host = n.host; }
    else if (n.type === 'join') { if (this.self && this.self < n.id) this._offer(n.id); }
    else if (n.type === 'leave') this._drop(n.id);
    else if (n.type === 'host-left') {
      this._drop(this.host);   // the relay's session model ends; the mesh carries on
      const ids = [...this.peerMap.keys(), this.self].filter((x) => x);
      this.host = ids.length ? Math.min(...ids) : this.self;
    } else if (n.type === 'full') this.err = 'room full';
  }

  _newPeer(id) {
    this._drop(id);
    const pc = new RTCPeerConnection({ iceServers: this.iceServers });
    const p = { id, pc, dc: null, open: false, chain: Promise.resolve(), tx: 0, rx: 0, pair: null, rtt: null };
    pc.onicecandidate = (e) => {
      if (e.candidate && e.candidate.candidate) this._sig(id, 'c', `${e.candidate.sdpMid ?? '0'}\n${e.candidate.candidate}`);
    };
    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'failed' || pc.connectionState === 'closed') p.open = false;
    };
    pc.ondatachannel = (e) => this._attach(p, e.channel);
    this.peerMap.set(id, p);
    return p;
  }

  _attach(p, dc) {
    p.dc = dc;
    dc.binaryType = 'arraybuffer';
    dc.onopen = () => { p.open = true; this.stats(); };
    dc.onclose = () => { p.open = false; };
    dc.onmessage = (e) => {
      if (typeof e.data === 'string') return;
      const f = new Uint8Array(e.data);
      if (f.length < 14 || f.length > FRAME_MAX) return;
      p.rx++;
      if (!(f[6] & 1)) this.macs.set(macKey(f, 6), p.id);
      if (this.onframe) { this.counters.rx++; this.onframe(f); return; }
      if (this.rxq.length >= this.rxMax) { this.counters.drop++; return; }
      this.rxq.push(f);
      this.counters.rx++;
    };
    if (dc.readyState === 'open') p.open = true;
  }

  _offer(id) {
    const p = this._newPeer(id);
    this._attach(p, p.pc.createDataChannel('h2', { ordered: false, maxRetransmits: 0 }));
    p.chain = p.chain.then(async () => {
      await p.pc.setLocalDescription(await p.pc.createOffer());
      this._sig(id, 'o', p.pc.localDescription.sdp);
    }).catch((e) => { this.err = String(e); });
  }

  _signal(from, type, text) {
    let p = this.peerMap.get(from);
    if (type === 'o') {
      p = this._newPeer(from);
      p.chain = p.chain.then(async () => {
        await p.pc.setRemoteDescription({ type: 'offer', sdp: text });
        await p.pc.setLocalDescription(await p.pc.createAnswer());
        this._sig(from, 'a', p.pc.localDescription.sdp);
      }).catch((e) => { this.err = String(e); });
    } else if (type === 'a' && p) {
      p.chain = p.chain.then(() => p.pc.setRemoteDescription({ type: 'answer', sdp: text }))
        .catch((e) => { this.err = String(e); });
    } else if (type === 'c' && p) {
      const nl = text.indexOf('\n');
      if (nl < 0) return;
      const sdpMid = text.slice(0, nl), candidate = text.slice(nl + 1).replace(/^a=/, '');
      p.chain = p.chain.then(() => p.pc.addIceCandidate({ candidate, sdpMid }))
        .catch(() => { /* e.g. an mDNS name the browser cannot resolve: harmless */ });
    }
  }

  _drop(id) {
    const p = this.peerMap.get(id);
    if (!p) return;
    this.peerMap.delete(id);
    for (const [k, v] of this.macs) if (v === id) this.macs.delete(k);
    try { p.dc && p.dc.close(); } catch { /* */ }
    try { p.pc.close(); } catch { /* */ }
  }
}

function macKey(f, o) {
  let s = '';
  for (let i = 0; i < 6; i++) s += (f[o + i] < 16 ? '0' : '') + f[o + i].toString(16);
  return s;
}
