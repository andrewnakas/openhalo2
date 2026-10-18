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

## Compare
Return registers (eax, edx; x87 top for float), all guest memory writes outside the
stack, and the call sequence. Equal on all K snapshots -> `equivalent`; otherwise
`differs` with the first diverging snapshot, address, expected vs got.

## Not handled (marked in the queue `reason` column)
- Sources calling Direct3D (the Xbox d3d8.h inlines push-buffer writes): `unbuildable: d3d`.
- XOnline structures not in the shim: `unbuildable: xonline`.
- Functions never reached in play: `untestable: unreached`.
