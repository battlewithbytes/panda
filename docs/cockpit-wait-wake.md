# Optional pending-IRQ WAIT wake policy

`-global mips-cpu.wait-irq-wake=on` selects a board-level analysis policy for
MIPS CPUs. It defaults to **off**; existing CPU models and the existing R6 wake
rule are unchanged. Boards with heterogeneous CPUs should set the property on
the intended CPU instances rather than use the global option.

The property permits a halted CPU to resume when a hardware IRQ is pending and
passes the CPU's existing pin-mask/vector-priority test, even if global
exception acceptance is disabled. It does not change Status, Cause, ISA level,
guest memory, or exception entry. `mips_cpu_exec_interrupt` still decides
whether the IRQ can actually be taken. Existing MT/VPE active checks still
veto wakeup. A masked pin or no pending interrupt does not qualify. This is
not a new source of interrupts and does not repair missing device delivery.

Motivation: a reviewed MIPS32r2 modem calls WAIT with Status.IE clear and
expects to resume before restoring interrupt acceptance. The current pre-R6
`mips_cpu_has_work` gate cannot do that. This opt-in policy captures that
specific hardware expectation, not universal MIPS or MediaTek behavior. No
firmware address, image digest, source number, or CPU count enters the engine
implementation. Physical-silicon fidelity is not claimed.

The property is machine configuration outside resettable architectural state.
Use the same setting on restart. Cross-policy migration/snapshot restoration
has not been validated and is outside the current cold-boot experiment.

FirmWire's `tests/mips_wait_probe.py` is an independent native reproducer. It
records guest-written Status/Cause and progress words, packaged engine hash,
probe hash and exact child command. With the pin pending and IM enabled it
requires resume while IE stays clear and zero ISR visits, followed by explicit
guest EI, one ISR, guest acknowledgment and ERET. No-WAIT is the wiring
control; no-pending and pin-masked cases must stay blocked. A timeout without
guest evidence or with a dead child is a failure, not a successful control.
No monitor stop/resume, firmware image, or Python CPU-struct cast is used.

This single-CPU probe presents the level before WAIT. Asynchronous arrival
after all CPUs have halted and a real firmware boot remain separate tests.
