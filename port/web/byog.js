// Open Halo 2 (web): bring your own game.
//
// The page never serves game data. The player picks their own disc image (a
// Redump .iso or an XISO) or the folder of an extracted copy; the files the
// game reads (default.xbe, maps/, media/, bink/, TDATA/, UDATA/) are copied
// into the origin's private file system under game/, where the program mounts
// them (main_web.c: /opfs/game). default.xbe must be the retail build the
// code was lifted from (same SHA-256 as the native build's check).
//
// The image is read with File.slice, so a 7 GB image never has to fit in memory.
// The XDVDFS reader follows tools/import/h2import.c (same partition offsets,
// same name rules).
export const XBE_SHA256 = '03215919bb7163259257d361f4c7bf802a7ab12aa85e2689436369b5c427935d';
const WANT = new Set(['default.xbe', 'maps', 'media', 'bink', 'tdata', 'udata']);
const SECTOR = 2048;
const PARTITIONS = [0, 0x18300000, 0xFD90000, 0x2080000];
const MAGIC = 'MICROSOFT*XBOX*MEDIA';

async function bytes(file, off, n) {
  return new Uint8Array(await file.slice(off, off + n).arrayBuffer());
}
const ascii = (b) => String.fromCharCode(...b);
function nameOk(n) {
  return n.length > 0 && !n.includes('..') && !/[\/\\:\x00-\x1f]/.test(n);
}

async function findPartition(file) {
  for (const p of PARTITIONS) {
    if (p + 33 * SECTOR > file.size) continue;
    const v = await bytes(file, p + 32 * SECTOR, SECTOR);
    if (ascii(v.subarray(0, 20)) === MAGIC && ascii(v.subarray(0x7EC, 0x7EC + 20)) === MAGIC) {
      const dv = new DataView(v.buffer);
      return { base: p, rootSector: dv.getUint32(20, true), rootSize: dv.getUint32(24, true) };
    }
  }
  return null;
}

// Every entry of one directory: [{name, sector, size, dir}]
async function readDir(file, base, sector, size) {
  if (!size) return [];
  if (size > 64 << 20) throw new Error('damaged image (directory too large)');
  const buf = await bytes(file, base + sector * SECTOR, size);
  const dv = new DataView(buf.buffer);
  const out = [], seen = new Set(), stack = [0];
  while (stack.length) {
    const off = stack.pop();
    if (off + 14 > size || seen.has(off)) continue;
    seen.add(off);
    const left = dv.getUint16(off, true), right = dv.getUint16(off + 2, true);
    if (left === 0xFFFF) continue;                       // sector padding
    const nlen = buf[off + 13];
    if (off + 14 + nlen > size) throw new Error('damaged image (entry past the end)');
    const name = ascii(buf.subarray(off + 14, off + 14 + nlen));
    if (!nameOk(name)) throw new Error('the image holds an unsafe file name: ' + JSON.stringify(name));
    out.push({ name, sector: dv.getUint32(off + 4, true), size: dv.getUint32(off + 8, true),
               dir: (buf[off + 12] & 0x10) !== 0 });
    if (left) stack.push(left * 4);
    if (right) stack.push(right * 4);
  }
  return out;
}

// [{path, size, read(off,n)->Promise<Uint8Array>|blob}] from an image
async function listImage(file) {
  const part = await findPartition(file);
  if (!part) throw new Error('this is not an Xbox disc image (no XDVDFS volume found)');
  const files = [];
  const walk = async (sector, size, prefix, depth) => {
    if (depth > 48) throw new Error('damaged image (folders nested too deep)');
    for (const e of await readDir(file, part.base, sector, size)) {
      if (!prefix && !WANT.has(e.name.toLowerCase())) continue;
      const path = prefix + e.name;
      if (e.dir) await walk(e.sector, e.size, path + '/', depth + 1);
      else {
        const off = part.base + e.sector * SECTOR;
        if (off + e.size > file.size) throw new Error(path + ' lies past the end of the image (truncated download?)');
        files.push({ path, size: e.size, blob: file.slice(off, off + e.size) });
      }
    }
  };
  await walk(part.rootSector, part.rootSize, '', 0);
  return files;
}

// From <input type=file webkitdirectory>: File objects with webkitRelativePath
function listFolder(fileList) {
  const files = [];
  for (const f of fileList) {
    const parts = f.webkitRelativePath.split('/').slice(1);       // drop the picked folder's name
    if (!parts.length || !WANT.has(parts[0].toLowerCase())) continue;
    if (!parts.every(nameOk)) throw new Error('unsafe file name in the folder: ' + f.webkitRelativePath);
    files.push({ path: parts.join('/'), size: f.size, blob: f });
  }
  return files;
}

async function sha256Hex(blob) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', await blob.arrayBuffer()));
  return [...d].map((x) => x.toString(16).padStart(2, '0')).join('');
}

// Is a good game already imported?
export async function haveGame() {
  try {
    const root = await navigator.storage.getDirectory();
    const g = await root.getDirectoryHandle('game');
    const x = await (await g.getFileHandle('default.xbe')).getFile();
    return (await sha256Hex(x)) === XBE_SHA256;
  } catch (e) { return false; }
}

// source: a File (image) or a FileList (folder). say(text): progress lines.
export async function importGame(source, say = () => {}) {
  const files = source instanceof File ? await listImage(source) : listFolder(source);
  const xbe = files.find((f) => f.path.toLowerCase() === 'default.xbe');
  if (!xbe) throw new Error('no default.xbe in what you picked');
  const sha = await sha256Hex(xbe.blob);
  if (sha !== XBE_SHA256)
    throw new Error('this default.xbe is not the original retail Halo 2 this build runs (SHA-256 ' + sha + ')');
  const root = await navigator.storage.getDirectory();
  const game = await root.getDirectoryHandle('game', { create: true });
  const total = files.reduce((a, f) => a + f.size, 0);
  let done = 0, copied = 0;
  for (const f of files) {
    const parts = f.path.split('/'); const name = parts.pop();
    let dir = game; for (const d of parts) dir = await dir.getDirectoryHandle(d, { create: true });
    const fh = await dir.getFileHandle(name, { create: true });
    if ((await fh.getFile()).size === f.size && f !== xbe) { done += f.size; continue; }   // resumed import
    const w = await fh.createWritable();       // nothing is kept unless close() is reached
    await f.blob.stream().pipeTo(w);
    done += f.size; copied++;
    say('copying your game: ' + (done / 1048576 | 0) + ' of ' + (total / 1048576 | 0) + ' MB (' + f.path + ')');
  }
  say('IMPORT_DONE ' + files.length + ' files (' + copied + ' copied), ' + (total / 1048576 | 0) + ' MB, SHA-256 OK');
  return { files: files.length, bytes: total };
}
