# fecheck design (decomp-assist bench, P2)

Goal: tell whether a decomp function F (retail address A) does what retail A does, on real
game states, without the vc71 compiler. Verdicts: `equivalent`, `differs`, `unbuildable`,
`untestable`. See docs/DECOMP_ASSIST_PLAN.md for the wider plan.

## Inputs
- Snapshots of A's entry state taken in play (fecheck_rt.c, `RECOMP_FE_SNAP`): registers,
  x87/MMX/XMM, guest RAM and the contiguous window, in the page store.
- F compiled by VS2022 x86 (dbuild.py) into a COFF object, one COMDAT per function.
- The decomp symbol -> retail VA map (M0), so F's references to globals and callees
  resolve to retail addresses.

## 1. Mock-callee unit test
Run F alone. Every call F makes out of itself is a mock: it records (target, arguments)
and returns what retail's callee returned at the same point (see 2). F's own memory
reads/writes go to a copy of the snapshot. This isolates F's body: a wrong callee
convention or a callee that is itself wrong cannot blame F.

## 2. Call-sequence matching
Run retail A from the same snapshot (Unicorn, retail bytes overlaid) and record its
call sequence: target, entry registers, stack arguments, and the callee's return
registers / x87 top / memory writes. Pair F's calls with retail's in order by target.
Retail's recorded effects are replayed as the mock's result. Divergence in target
order, count or argument values is reported as the first difference.

## 3. Convention learning from argument values
LTCG gives internal callees custom conventions. For each callee, the declaration in our
object gives a standard-convention parameter list; retail's call sites carry the same
values in some registers/stack slots. Matching values across the K snapshots (each
parameter value of F's call vs each register/stack word of retail's call) learns the
mapping parameter -> location, and how many stack bytes the callee pops (from esp after
retail's call). Ambiguous mappings (equal values) are resolved by other snapshots.

## 4. Real callee from retail's entry state
Optionally, instead of replaying recorded effects, run the real retail callee from the
retail call's entry state (built from F's arguments through the learned mapping). That
tests F against callee behaviour on states retail never produced at that call.

## 5. Register-parameter search for F itself
A's own entry convention is also custom. The thunk that enters F from a snapshot needs
to know which register or stack slot carries each of F's parameters. Search: for each
parameter, candidate locations are eax/ecx/edx/ebx/esi/edi and the stack slots; pick
the assignment under which F's behaviour (calls + writes + return) agrees with retail on
all K snapshots; fall back to the calling-convention analysis in tools/abi_wrap.py and
the retail `ret N`.
Candidate registers are A's live-ins: registers read before written on some path from the entry.
A `push` of ebx/esi/edi is a callee-saved register's save, not a read, only when A restores it
(`pop`) and the push does not sit within the argument bytes of the call it leads to (the callee's
single `ret N`, or the caller's `add esp, N`); so an incoming edi/ebx handed on as a stack argument
is a live-in. When retail's `ret N` pops N > 0 bytes, maps whose stack parameters fill exactly N
bytes are tried first.

## 6. Frame-pointer pairing
Callee writes into retail's frame are replayed into ours through a pairing of retail's frame
pointers with ours. First try: by argument position (same pops and register set) or address
order, as before. When a run differs and a call site's pairing is not certain (retail's callee
reads other registers than our declaration passes, or pops differ), the snapshot is run again
under alternative pairings at those sites: injective maps of the retail pointers the callee's
writes are based on to the pointers ours passes, where the written extent fits before ours'
next pointer, retail stack arguments keep our parameter order, and no write goes to a
pointer-to-const parameter; capped at 24 per site and 48 combinations. A pairing (or flipping
the hidden result pointer of a class-returning callee) is accepted only when the whole run
agrees, and then must hold on every snapshot. Entry registers handed untouched to a callee are
live-in candidates too, tried after the usual ones.

### Known callee conventions
At an uncertain site whose callee's retail convention is known, the frame pointers pair parameter by
parameter (retail's value in the location of parameter i with ours for parameter i, ours from the
callee's declared signature), and only that way: an open search could hide a source that swaps two
outputs. The convention comes from `<WORK>/entry_maps.csv` (read off retail's disassembly, trusted when
it pops the bytes retail's callee popped), else it is learned: the states retail A reaches at its calls
to the callee become snapshots of the callee, its own source is checked on them under every mapping
consistent with the call sites seen (values that are not frame pointers sit where retail held the same
value, frame pointers where retail held one, stack parameters in order filling exactly retail's pops,
grouped by F's entry mapping), and the result is kept only when every agreeing mapping puts each
pointer parameter in the same place (a callee that only hands its outputs on to its own mocked callees
cannot tell them apart). Learned maps are cached in `<WORK>/callee_maps`. A merged callee write that
spans several paired pointers is split at them.

## Retail cache
`<WORK>/rcache_v6`: one pickle per snapshot holding ('fecheck-rcache', format, result). The format is
RCACHE_FORMAT plus a hash of the routines that run for real (never recorded as calls); an entry of
another format, or older than its snapshot, is rerun.
Each entry also records the step cap it ran under (FECHECK_CAP); a run that stopped at the cap is
rerun when the cap is larger, and the wall-clock limit grows with the cap. A retail run that yields
the thread 100000 times (NtYieldExecution / KeDelayExecutionThread) is waiting on another thread that
never runs in the emulator (an I/O completion flag): it stops at once as `untestable: retail waits on
another thread` naming the waiting function.

## Vtables
A vtable the object defines without a retail address (``??_7X``) links to retail's own copy when its
contents single it out: the slots whose functions have retail addresses match exactly one place in
retail's data (at least two known slots). Then an object's stored vtable pointer is retail's. A copy
that matches all but a few of at least eight known slots is the class's vtable with wrong slots in the
decomp: ours stays in the blob and a `differs` verdict names the slots first. A weak external (MSVC's
vector deleting destructor ``??_E``) links to its default, the scalar deleting destructor ``??_G``.

## Builtins
By-value structures smaller than a dword compare only their own bytes as call arguments (retail pushes
a register and stores the byte). VS2022's __libm_sse2_pow_precise runs retail's own _CIpow, so pow's
IEEE edge cases (pow(0,y>0)=0, ...) are retail's.

## Compare
Return registers (eax, edx; x87 top for float), all guest memory writes outside the
stack, and the call sequence. Equal on all K snapshots -> `equivalent`; otherwise
`differs` with the first diverging snapshot, address, expected vs got.

## Not handled (marked in the queue `reason` column)
- Sources calling Direct3D (the Xbox d3d8.h inlines push-buffer writes): `unbuildable: d3d`.
- XOnline structures not in the shim: `unbuildable: xonline`.
- Functions never reached in play: `untestable: unreached`.
