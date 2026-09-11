/* Opt-in, bounded MIPS MT startup model. No vendor addresses or CPU ABI changes. */
#ifndef MIPS_COCKPIT_MT_H
#define MIPS_COCKPIT_MT_H

#include "cpu.h"

#ifndef CONFIG_USER_ONLY
typedef struct CockpitMTTopology {
    unsigned cores;
    unsigned vpes_per_core;
    unsigned tcs_per_core;
} CockpitMTTopology;
bool cockpit_mt_topology_valid(const CockpitMTTopology *topology);
void cockpit_mt_init(MIPSCPU **cpus, const CockpitMTTopology *topology,
                     target_ulong entry);
bool cockpit_mt_release_core(unsigned core, target_ulong entry);
bool cockpit_mt_enabled(CPUMIPSState *env);
bool cockpit_mt_active(CPUMIPSState *env);
CPUMIPSState *cockpit_mt_map(CPUMIPSState *env, int tc);
void cockpit_mt_bind(CPUMIPSState *env, int tc, target_ulong value);
void cockpit_mt_control(CPUMIPSState *env, uint32_t previous);
void cockpit_mt_refresh(CPUMIPSState *env);
void cockpit_mt_restart(CPUMIPSState *env, int tc, target_ulong pc);
#else
static inline bool cockpit_mt_enabled(CPUMIPSState *env) { return false; }
static inline bool cockpit_mt_active(CPUMIPSState *env) { return false; }
#endif
#endif
