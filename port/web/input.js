// Open Halo 2 (web): keyboard, mouse and gamepads to the title's controller ports.
//
// The page writes one 24-byte record per port into the program's memory once a
// frame (address from _xbox_web_pad_buffer(); layout in xinput_device.c, the
// __EMSCRIPTEN__ branch): uint32 connected, then an XBOX_GAMEPAD: uint16
// buttons, 8 analog buttons (A B X Y black white left-trigger right-trigger),
// 4 int16 sticks (LX LY RX RY, up and right positive).
//
// Port 0 is the keyboard and mouse merged with the first gamepad; further
// gamepads take ports 1-3. Keys are the same as the Windows build:
//   W A S D move (Shift = half)      mouse look after a click on the picture
//   I J K L look                     left button fire, right button grenade
//   Space A   F/Backspace B   E/R X   Tab Y   Q white   G black
//   C/Ctrl crouch (left stick click)  Z/middle button zoom (right stick click)
//   Enter/Esc START   F2 BACK   arrows d-pad
(function () {
  'use strict';
  const keys = new Set();
  let mdx = 0, mdy = 0, mbuttons = 0, rx = 0, ry = 0;
  const DIG = { up: 1, down: 2, left: 4, right: 8, start: 16, back: 32, l3: 64, r3: 128 };
  const swallow = new Set(['Tab', 'Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Backspace', 'F2']);

  addEventListener('keydown', e => { keys.add(e.code); if (swallow.has(e.code)) e.preventDefault(); });
  addEventListener('keyup', e => keys.delete(e.code));
  addEventListener('blur', () => { keys.clear(); mbuttons = 0; });
  addEventListener('mousemove', e => { if (document.pointerLockElement) { mdx += e.movementX; mdy += e.movementY; } });
  addEventListener('mousedown', e => {
    const t = e.target;
    if (t && t.tagName === 'CANVAS' && !document.pointerLockElement && t.requestPointerLock) t.requestPointerLock();
    if (document.pointerLockElement) mbuttons |= 1 << e.button;
  });
  addEventListener('mouseup', e => { mbuttons &= ~(1 << e.button); });
  addEventListener('contextmenu', e => { if (document.pointerLockElement) e.preventDefault(); });

  const k = c => keys.has(c);
  const clamp16 = v => Math.max(-32768, Math.min(32767, Math.round(v)));

  function keyboardPad() {
    const p = { dig: 0, an: new Uint8Array(8), lx: 0, ly: 0, rx: 0, ry: 0 };
    const half = (k('ShiftLeft') || k('ShiftRight')) ? 0.5 : 1;
    p.lx = ((k('KeyD') ? 1 : 0) - (k('KeyA') ? 1 : 0)) * 32767 * half;
    p.ly = ((k('KeyW') ? 1 : 0) - (k('KeyS') ? 1 : 0)) * 32767 * half;
    // mouse: speed to stick deflection, smoothed, starting past the title's dead zone
    const sens = 900;
    rx = rx * 0.6 + mdx * sens * 0.4; ry = ry * 0.6 + mdy * sens * 0.4; mdx = 0; mdy = 0;
    const dead = v => (Math.abs(v) < 1 ? 0 : Math.sign(v) * (0.2 * 32767 + Math.abs(v)));
    p.rx = dead(rx) + ((k('KeyL') ? 1 : 0) - (k('KeyJ') ? 1 : 0)) * 32767;
    p.ry = -dead(ry) + ((k('KeyI') ? 1 : 0) - (k('KeyK') ? 1 : 0)) * 32767;
    if (k('ArrowUp')) p.dig |= DIG.up;
    if (k('ArrowDown')) p.dig |= DIG.down;
    if (k('ArrowLeft')) p.dig |= DIG.left;
    if (k('ArrowRight')) p.dig |= DIG.right;
    if (k('Enter') || k('Escape')) p.dig |= DIG.start;
    if (k('F2')) p.dig |= DIG.back;
    if (k('KeyC') || k('ControlLeft')) p.dig |= DIG.l3;
    if (k('KeyZ') || (mbuttons & 2)) p.dig |= DIG.r3;
    if (k('Space')) p.an[0] = 255;
    if (k('KeyF') || k('Backspace')) p.an[1] = 255;
    if (k('KeyE') || k('KeyR')) p.an[2] = 255;
    if (k('Tab')) p.an[3] = 255;
    if (k('KeyG')) p.an[4] = 255;
    if (k('KeyQ')) p.an[5] = 255;
    if (mbuttons & 4) p.an[6] = 255;      // right button: left trigger
    if (mbuttons & 1) p.an[7] = 255;      // left button: right trigger
    return p;
  }

  function gamepadPad(g) {       // the Gamepad API's "standard" mapping
    const b = i => (g.buttons[i] ? g.buttons[i].value : 0);
    const p = { dig: 0, an: new Uint8Array(8), lx: 0, ly: 0, rx: 0, ry: 0 };
    p.lx = (g.axes[0] || 0) * 32767; p.ly = -(g.axes[1] || 0) * 32767;
    p.rx = (g.axes[2] || 0) * 32767; p.ry = -(g.axes[3] || 0) * 32767;
    if (b(12) > 0.5) p.dig |= DIG.up;
    if (b(13) > 0.5) p.dig |= DIG.down;
    if (b(14) > 0.5) p.dig |= DIG.left;
    if (b(15) > 0.5) p.dig |= DIG.right;
    if (b(9) > 0.5) p.dig |= DIG.start;
    if (b(8) > 0.5) p.dig |= DIG.back;
    if (b(10) > 0.5) p.dig |= DIG.l3;
    if (b(11) > 0.5) p.dig |= DIG.r3;
    p.an[0] = b(0) * 255; p.an[1] = b(1) * 255; p.an[2] = b(2) * 255; p.an[3] = b(3) * 255;
    p.an[4] = b(5) * 255;        // right bumper: black
    p.an[5] = b(4) * 255;        // left bumper: white
    p.an[6] = b(6) * 255; p.an[7] = b(7) * 255;
    return p;
  }

  function merge(a, b) {
    const p = { dig: a.dig | b.dig, an: new Uint8Array(8) };
    for (let i = 0; i < 8; i++) p.an[i] = Math.max(a.an[i], b.an[i]);
    for (const ax of ['lx', 'ly', 'rx', 'ry']) p[ax] = Math.abs(a[ax]) > Math.abs(b[ax]) ? a[ax] : b[ax];
    return p;
  }

  let base = 0;
  function write(port, p) {
    const v = new DataView(HEAPU8.buffer, base + port * 24, 24);
    if (!p) { v.setUint32(0, 0, true); return; }
    v.setUint32(0, 1, true);
    v.setUint16(4, p.dig, true);
    for (let i = 0; i < 8; i++) v.setUint8(6 + i, p.an[i]);
    v.setInt16(14, clamp16(p.lx), true); v.setInt16(16, clamp16(p.ly), true);
    v.setInt16(18, clamp16(p.rx), true); v.setInt16(20, clamp16(p.ry), true);
  }

  function frame() {
    requestAnimationFrame(frame);
    if (!base) {
      // the page sets Module.h2ready in onRuntimeInitialized: the export aborts if called earlier
      if (typeof Module === 'undefined' || !Module.h2ready || typeof HEAPU8 === 'undefined') return;
      base = Module._xbox_web_pad_buffer();
      if (!base) return;
    }
    const pads = (navigator.getGamepads ? Array.from(navigator.getGamepads()) : []).filter(g => g && g.connected);
    let p0 = keyboardPad();
    if (pads[0]) p0 = merge(p0, gamepadPad(pads[0]));
    write(0, p0);
    for (let i = 1; i < 4; i++) write(i, pads[i] ? gamepadPad(pads[i]) : null);
  }
  requestAnimationFrame(frame);
})();
