# Plan: use the playing recomp to speed up the Halo 2 decompilation

Goal: Open Halo 2 exists to help [halo2-decompiled](https://github.com/kirklandsig/halo2-decompiled)
(CC0, matching decomp, MSVC 7.1). This plan turns the recomp, which already runs the retail
code in-game, into a source of runtime facts and a functional test bench for decompiled code.
It also defines an overnight harness that grinds through functions unattended.

## Where things stand (2026-10-17)

- **Decomp:** 11,321 game functions (0x12000-0x2cb4e2). Of these, 9,085 are matched, 74 near (at
  most 2 instructions differ) and 2,162 todo. 2,131 of the todo functions already have source
  written that does not match yet; 31 have none. Library code (DSOUND, D3D, XOnline, Havok, Bink)
  is out of scope there. Matching needs the XDK 5849 vc71 compiler. The maintainer has it; this
  machine does not.
- **What the decomp lacks:**
  - Semantic checking. A "match" is byte equality. A non-matching function has no test at all,
    so its source may be wrong in ways nobody sees.
  - Runtime testing. Their notes say "no in-game runtime tests were run". Their rules allow
    "the program's observable behaviour, and runtime testing and debugging of the retail game"
    (PROVENANCE.md).
- **What the recomp already has** (openhalo2 + xboxrecomp):
  - 20,226 lifted functions `sub_<VA>`, all reached through one dispatch table. The VAs are the
    decomp's addresses (seeded from its functions.csv).
  - The wrapper pattern from `h2lift/xo_hle_pass.py`: `sub_X` becomes a switch between
    `sub_X_lifted` and a replacement, with every caller routed through it.
  - `tools/snapdiff`: records the full guest state at a chosen function's entry during real play
    (`bsnap_hook.c`), then replays it in Unicorn against the lifted copy (`cdiff.py`).
    `havok_diff.py` does the same from fixed argument vectors.
  - Play rigs that reach the menus, Cairo, the cinematics and Ivory Tower multiplayer headless
    (`tools/combat/*.sh`, `cairo*.sh`), plus call tracing (`--trace-functions`,
    `RECOMP_CALL_PROFILE`, ICALL feedback).
- **On this machine:** VS2022 x86 `cl.exe` (14.44), Unicorn 2.1.4, and the retail XBE
  (SHA-256 matches).

## Four products, in order of value per hour

### P1. Runtime facts (cheap, helps every function)
A post-pass, in the style of `xo_hle_pass`, puts a `RECOMP_FN_ENTER(va)` macro at the top of
every game function. The macro is empty in release builds. In an instrumented build, the rigs
play every scenario we can drive, and per function we record:
- **call count and where it was reached:** menu / campaign level / MP / cinematic. This gives
  coverage and the list of dead code;
- **callers and callees seen,** including **resolved indirect-call targets**: vtable slots and
  function pointers. These are the hardest edges for a static decomp;
- **argument samples:** the first N calls' stack arguments and registers. Each value is
  classified as a pointer into a known region (tag data, data-array pools, heap, stack, .rdata
  string) or a scalar / float / enum, with the range seen;
- **struct access shape:** for pointer arguments, the offsets read and written relative to the
  pointer (needs MEM tracing inside the function; only for the hot todo set);
- **return values** (range, pointer class).

Output: `runtime/functions_runtime.csv` plus per-function JSON. It is shaped for the decomp, so
it can feed names "from behaviour", types and field layouts. Nothing in it is retail bytes. It
is addresses, counts and value shapes, so it is CC0-shareable (the owner decides).

### P2. Functional-equivalence bench, "fecheck" (the core of the overnight run)
For a decomp function F at retail address A:
1. **Snapshots.** Play rigs capture K real call states of A: full guest RAM, registers and
   x87/XMM, through `bsnap_hook` at random hits across scenarios. They are captured once and
   reused all night.
2. **Build F as 32-bit x86** with VS2022 `cl /O1 /Gr` (or `/O2` per the file's `// @flags`)
   from the decomp's own `.cpp` and `include/`.
   - The object is linked at a free guest address, with every external symbol resolved to its
     retail VA: `function_<va>`, `unknown_<va>`, `g_<addr>`, and the names from
     functions.csv / @retail markers.
   - The result is a small blob of machine code that calls the real retail callees.
3. **Run both in Unicorn from the same snapshot.**
   - Retail bytes at A, versus the blob, entered through a thunk that maps the calling
     convention recorded for A. This covers LTCG custom conventions: the thunk is generated
     from our ABI analysis, and F's declaration supplies the C signature.
   - Callees run as retail code inside the snapshot in both runs; kernel calls are stubbed as
     in `cdiff.py`.
4. **Compare** the return registers, x87 top, all guest memory writes outside the stack, and
   the sequence of calls made (target + arguments).
5. **Verdict per function:**
   - `equivalent` (all K states agree);
   - `differs` (first diverging state, address, expected vs got: this is exactly the bug
     report a decomper needs);
   - `unbuildable` (compile error / missing symbol);
   - `untestable` (never reached in play, or needs a state we can't drive).

This tests the 2,131 written-but-unmatched functions and the 74 near ones semantically, which
nothing does today. It also gives already-matched functions a regression net when they are
renamed or refactored (a match can't regress, but shared headers can mislead later work).

### P3. Writing and fixing code with the bench as the oracle (the "new decompiled code" part)
Overnight agents take the priority queue (below). For each function they:
- **For `differs`:** read the diff, fix the source until it is `equivalent`, and keep the
  decomp's style so a later matching pass can start from correct C.
- **For the 31 unwritten functions and any `unbuildable` ones:** write the source from the
  lifted C (`gen/`), the disassembly (`tools/disasm.py`) and the P1 facts, and iterate against
  fecheck.
- **Name** functions, globals and struct fields only from observed behaviour (P1 data + code),
  never from banned sources (decomp LEGAL.md). Names go in comments/notes, ready for the
  decomp's review.

Result per function: source that is proven equivalent on real game states, plus a note
(`docs/unknown_<va>.md` style) with the evidence. It will not be byte-matched, because that
needs vc71. Matching becomes a pure codegen puzzle for the maintainer's lanes: the semantics
are already settled.

### P4. In-game swap (proof by playing)
For `equivalent` functions, build the decomp version (P2's blob) into the recomp:
- lift the blob with our lifter, or compile F natively against guest memory;
- install it through the `xo_hle_pass` wrapper pattern as `sub_X_decomp`, with a runtime
  switch (`RECOMP_DECOMP=all|none|<list>`) and per-function fallback to `sub_X_lifted`.

Then run the play rigs with the decomp versions on, and compare frame hashes and game-state
checksums against an all-lifted run on the same pad script. A swapped set that plays
identically is the strongest evidence, and it is the first step of the long road to a source
port (the exe grows more decomp code over time).

## Priority queue (what the overnight run works on)
Score = (seen in play) x (todo, or near) x (call count, log-scaled) x (bytes) / (difficulty).
Difficulty comes from size, x87/SSE density and the number of unresolved callees. Order:
1. near (74): the fastest wins, likely just semantic confirmation;
2. todo with source, hot in play;
3. todo with source, reached but cold;
4. unwritten (31);
5. todo never reached: run static-only (`havok_diff`-style argument vectors), lower confidence.

## Overnight harness (one night = one batch)
- **Driver:** a workflow (≤10 agents) or a small fleet of Claude sessions, ≤6 at once (RAM),
  each with a lane = a range of the queue, so no two agents touch the same function.
- **Per lane loop:** take the next function → fecheck → fix/write → fecheck again → record the
  verdict in `runtime/fecheck.csv` + note → next. Hard time cap per function (e.g. 25 min);
  park it with the last diff if over.
- **Shared, read-only all night:**
  - the instrumented exe;
  - the snapshot store (`D:/n64work/halo2/snaps/<va>/*.bsnap`, capped per function; RAM
    images compress well);
  - a decomp checkout at a fixed commit plus one git worktree per lane.
- **Safety:**
  - nothing is pushed or sent;
  - the decomp checkout is a local branch per lane;
  - the game exe in `xr_play` and the owner's saves are untouched;
  - renamed rig exes only;
  - builds at -j2;
  - every lane writes a one-screen summary.
- **Morning report:** functions newly `equivalent`, fixed and written, plus the differing ones
  with their top diffs, P1 coverage, and a ready-to-review patch series per lane. Sending
  anything to halo2-decompiled (an issue, a PR, a runtime-facts drop) is the owner's call.

## Milestones
- **M0 (setup, ~1 h):**
  - pin the decomp commit; map symbols → VAs (functions.csv, @retail, `g_<addr>`);
  - VS2022 x86 build of one decomp `.cpp` into a VA-linked blob.
- **M1 (P1 runtime facts, ~3 h):**
  - FN_ENTER post-pass + counters + argument sampler;
  - instrumented build; rigs: boot→menus, Cairo cinematic + walk, Ivory Tower MP fight,
    the Armory;
  - `functions_runtime.csv`. Gate: game functions reached ≥ 30% (estimate; measure).
- **M2 (P2 fecheck, ~4 h):**
  - snapshot capture for the queue's top 200;
  - the Unicorn A/B runner with the convention thunk and call-sequence compare.
  - Gate: 10 already-matched functions all report `equivalent` (they must: same source, same
    semantics). Then 5 hand-broken copies must report `differs` with the right address.
- **M3 (overnight run):** lanes over the queue, P3 fixes/writes, morning report.
- **M4 (P4 in-game swap):** the wrapper pass + `RECOMP_DECOMP`; rig A/B of the `equivalent`
  set.

## Open questions for the owner
- Share the P1 runtime facts and P2/P3 results with halo2-decompiled (issue #104 coordination /
  a PR into their docs)? Nothing is sent without your word.
- Do you have access to the XDK 5849 vc71 compiler? With it, overnight lanes could also chase
  byte matches. Without it, our lanes deliver proven-correct source and the maintainer's lanes
  match it.
