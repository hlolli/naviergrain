; Include naviergrain.inc separately for the schema names/defaults.
; Init always prepares the instance. While kRun != 1, performance is skipped:
; no grains, simulation time, RNG, or reset state advance. Outputs are silence;
; stats retain their last snapshot (initially the prepared/ready snapshot).
; Pre-roll the enclosing persistent instrument BEFORE attaching audio callbacks.
; This is a host transport gate, not the naviergrain freeze control. Only change
; it at silent boundaries: pausing a sounding grain is an abrupt transport cut.
opcode NaviergrainPrepared, aak[], iii[]k[]k
  iTable, iSourceRate, iConfig[], kControl[], kRun xin
  if kRun == 1 then
    aLeft, aRight, kStats[] naviergrain iTable, iSourceRate, iConfig, kControl
  else
    aLeft = 0
    aRight = 0
  endif
  xout aLeft, aRight, kStats
endop


; Offline observation only; iView must be a dedicated 3344-element GEN02 table.
; kObserve=1 copies one bounded snapshot after rendering the current block.
; Keep the table alive and unchanged until this instance is destroyed.
opcode NaviergrainVisualPrepared, aak[], iii[]k[]kik
  iTable, iSourceRate, iConfig[], kControl[], kRun, iView, kObserve xin
  if kRun == 1 then
    aLeft, aRight, kStats[] naviergrain_visual iTable, iSourceRate, iConfig, kControl, iView, kObserve
  else
    aLeft = 0
    aRight = 0
  endif
  xout aLeft, aRight, kStats
endop
