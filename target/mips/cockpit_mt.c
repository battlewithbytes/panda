/*
 * Experimental MIPS MT startup subset, configured by the board.
 * One executing TC per VPE; additional TCs may be configured while halted.
 * No vendor addresses, firmware offsets, or CPUMIPSState/CFFI layout changes.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "qemu/osdep.h"
#include "cockpit_mt.h"
#include "sysemu/reset.h"
#include "exec/exec-all.h"
#include "migration/migration.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

typedef struct StartupCore {
    MIPSCPU **vpes;
    unsigned *owner;
    bool *runnable;
    unsigned exclusive_vpe;
    bool released;
} StartupCore;

typedef struct StartupMachine {
    CockpitMTTopology topology;
    StartupCore *cores;
    target_ulong entry;
    Error *migration_blocker;
} StartupMachine;

/* Configurable-machine lifetime, like its CPUs. This registry is outside the
 * pypanda CPU ABI. No hotplug, replay, migration, or time-sliced TC scheduler. */
static StartupMachine *machine;

static StartupCore *find_core(CPUMIPSState *env, unsigned *vpe)
{
    unsigned c, v;
    if (!machine) {
        return NULL;
    }
    for (c = 0; c < machine->topology.cores; c++) {
        for (v = 0; v < machine->topology.vpes_per_core; v++) {
            if (env == &machine->cores[c].vpes[v]->env) {
                if (vpe) {
                    *vpe = v;
                }
                return &machine->cores[c];
            }
        }
    }
    return NULL;
}

bool cockpit_mt_enabled(CPUMIPSState *env)
{
    return find_core(env, NULL) != NULL;
}

CPUMIPSState *cockpit_mt_map(CPUMIPSState *env, int tc)
{
    StartupCore *core = find_core(env, NULL);
    assert(core);
    if (tc < 0 || tc >= machine->topology.tcs_per_core) {
        cpu_abort(CPU(mips_env_get_cpu(env)),
                  "cockpit MT startup: TC index %d out of range", tc);
    }
    return &core->vpes[core->owner[tc]]->env;
}

static TCState *tc_state(CPUMIPSState *env, int tc)
{
    return tc == env->current_tc ? &env->active_tc : &env->tcs[tc];
}

bool cockpit_mt_active(CPUMIPSState *env)
{
    unsigned v;
    StartupCore *core = find_core(env, &v);
    return core && core->runnable[v];
}

void cockpit_mt_restart(CPUMIPSState *env, int tc, target_ulong pc)
{
    if (tc == env->current_tc) {
        CPUClass *cc = CPU_GET_CLASS(mips_env_get_cpu(env));
        cc->set_pc(CPU(mips_env_get_cpu(env)), pc);
    } else {
        /* Preserve ISA bit until this halted context is selected. */
        env->tcs[tc].PC = pc;
    }
}

void cockpit_mt_bind(CPUMIPSState *env, int tc, target_ulong value)
{
    StartupCore *core = find_core(env, NULL);
    TCState *state = tc_state(env, tc);
    unsigned dest = value & 15;
    assert(core);
    if (dest >= machine->topology.vpes_per_core ||
        !(state->CP0_TCHalt & 1)) {
        cpu_abort(CPU(mips_env_get_cpu(env)),
                  "cockpit MT startup: rebind requires halted TC and valid VPE");
    }
    state->CP0_TCBind = (tc << CP0TCBd_CurTC) |
                       (value & ((1 << CP0TCBd_TBE) | 15));
    if (dest != core->owner[tc]) {
        CPUMIPSState *other = &core->vpes[dest]->env;
        if (tc == env->current_tc) {
            cpu_abort(CPU(mips_env_get_cpu(env)),
                      "cockpit MT startup: moving selected TC unsupported");
        }
        other->tcs[tc] = *state;
        core->owner[tc] = dest;
    }
}

void cockpit_mt_refresh(CPUMIPSState *caller)
{
    StartupCore *core = find_core(caller, NULL);
    unsigned v;
    assert(core);
    for (v = 0; v < machine->topology.vpes_per_core; v++) {
        CPUMIPSState *env = &core->vpes[v]->env;
        CPUState *cs = CPU(core->vpes[v]);
        bool enabled = core->released &&
            (env->CP0_VPEConf0 & (1 << CP0VPEC0_VPA)) &&
            ((env->mvp->CP0_MVPControl & (1 << CP0MVPCo_EVP)) ||
             core->exclusive_vpe == v);
        int selected = (env->CP0_VPEConf0 >> CP0VPEC0_XTC) & 255;
        bool run = false;
        if (enabled) {
            int tc;
            unsigned ready = 0;
            for (tc = 0; tc < machine->topology.tcs_per_core; tc++) {
                TCState *state = tc_state(env, tc);
                if (core->owner[tc] == v &&
                    (state->CP0_TCStatus & (1 << CP0TCSt_A)) &&
                    !(state->CP0_TCHalt & 1)) {
                    ready++;
                }
            }
            if (ready > 1) {
                cpu_abort(cs, "cockpit MT startup: multiple runnable TCs per VPE unsupported");
            }
            if (selected >= machine->topology.tcs_per_core ||
                core->owner[selected] != v) {
                cpu_abort(cs, "cockpit MT startup: XTC must name a locally bound TC");
            }
            if (selected != env->current_tc) {
                if (core->runnable[v]) {
                    cpu_abort(cs, "cockpit MT startup: live TC switching unsupported");
                }
                if (env->current_tc < machine->topology.tcs_per_core) {
                    env->tcs[env->current_tc] = env->active_tc;
                }
                env->active_tc = env->tcs[selected];
                env->current_tc = selected;
                cockpit_mt_restart(env, selected, env->active_tc.PC);
                compute_hflags(env);
            }
            run = (env->active_tc.CP0_TCStatus & (1 << CP0TCSt_A)) &&
                  !(env->active_tc.CP0_TCHalt & 1);
        }
        if (run && !core->runnable[v]) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
            cpu_interrupt(cs, CPU_INTERRUPT_WAKE);
        } else if (!run && core->runnable[v]) {
            cs->halted = 1;
            cpu_reset_interrupt(cs, CPU_INTERRUPT_WAKE);
            if (env == caller) {
                cpu_interrupt(cs, CPU_INTERRUPT_HALT);
            }
        }
        core->runnable[v] = run;
    }
}

void cockpit_mt_control(CPUMIPSState *env, uint32_t previous)
{
    unsigned v;
    StartupCore *core = find_core(env, &v);
    assert(core);
    if ((previous & (1 << CP0MVPCo_EVP)) &&
        !(env->mvp->CP0_MVPControl & (1 << CP0MVPCo_EVP))) {
        core->exclusive_vpe = v;
    }
    cockpit_mt_refresh(env);
}

/* Device/platform adapter calls this only after a validated release protocol.
 * It does not manufacture a firmware barrier arrival or select a vendor PC. */
bool cockpit_mt_release_core(unsigned index, target_ulong entry)
{
    StartupCore *core;
    CPUMIPSState *env;
    if (!machine || index >= machine->topology.cores ||
        machine->cores[index].released) {
        return false;
    }
    core = &machine->cores[index];
    env = &core->vpes[0]->env;
    cockpit_mt_restart(env, env->current_tc, entry);
    core->released = true;
    cockpit_mt_refresh(env);
    return true;
}

static void startup_reset(void *opaque)
{
    unsigned c, v, tc;
    CockpitMTTopology *t = &machine->topology;
    for (c = 0; c < t->cores; c++) {
        StartupCore *core = &machine->cores[c];
        memset(core->owner, 0, t->tcs_per_core * sizeof(*core->owner));
        memset(core->runnable, 0, t->vpes_per_core * sizeof(*core->runnable));
        core->exclusive_vpe = 0;
        core->released = false;
        for (v = 0; v < t->vpes_per_core; v++) {
            CPUMIPSState *env = &core->vpes[v]->env;
            cpu_reset(CPU(core->vpes[v]));
            CPU(core->vpes[v])->halted = 1;
            env->CP0_VPEConf0 = v ? 0 : 3;
            env->current_tc = v ? MIPS_TC_MAX : 0;
            for (tc = 0; tc < t->tcs_per_core; tc++) {
                env->tcs[tc].CP0_TCBind = tc << CP0TCBd_CurTC;
                env->tcs[tc].CP0_TCHalt = 1;
            }
            if (!v) {
                env->active_tc.CP0_TCBind = 0;
                env->active_tc.CP0_TCHalt = 0;
                env->active_tc.CP0_TCStatus = 1 << CP0TCSt_A;
            }
        }
        core->vpes[0]->env.mvp->CP0_MVPControl = 1 << CP0MVPCo_EVP;
        core->vpes[0]->env.mvp->CP0_MVPConf0 =
            (1U << CP0MVPC0_M) | (1 << CP0MVPC0_TCA) |
            ((t->vpes_per_core - 1) << CP0MVPC0_PVPE) | (t->tcs_per_core - 1);
    }
    cockpit_mt_release_core(0, machine->entry);
}

bool cockpit_mt_topology_valid(const CockpitMTTopology *t)
{
    return t->cores >= 1 && t->cores <= 4 &&
           t->vpes_per_core >= 1 && t->vpes_per_core <= 4 &&
           t->tcs_per_core >= t->vpes_per_core &&
           t->tcs_per_core <= MIPS_TC_MAX;
}

void cockpit_mt_init(MIPSCPU **cpus, const CockpitMTTopology *t,
                     target_ulong entry)
{
    unsigned c, v;
    Error *err = NULL;
    assert(!machine && cockpit_mt_topology_valid(t));
    for (v = 0; v < t->cores * t->vpes_per_core; v++) {
        if (!(cpus[v]->env.CP0_Config3 & (1 << CP0C3_MT))) {
            cpu_abort(CPU(cpus[v]), "cockpit MT startup requires MT-capable CPUs");
        }
    }
    machine = g_new0(StartupMachine, 1);
    machine->topology = *t;
    machine->entry = entry;
    machine->cores = g_new0(StartupCore, t->cores);
    for (c = 0; c < t->cores; c++) {
        StartupCore *core = &machine->cores[c];
        core->vpes = g_new0(MIPSCPU *, t->vpes_per_core);
        core->owner = g_new0(unsigned, t->tcs_per_core);
        core->runnable = g_new0(bool, t->vpes_per_core);
        for (v = 0; v < t->vpes_per_core; v++) {
            core->vpes[v] = cpus[c * t->vpes_per_core + v];
            if (v) {
                g_free(core->vpes[v]->env.mvp);
                core->vpes[v]->env.mvp = core->vpes[0]->env.mvp;
            }
        }
    }
    error_setg(&machine->migration_blocker,
               "cockpit MT startup topology does not support migration/snapshots");
    if (migrate_add_blocker(machine->migration_blocker, &err) < 0) {
        error_report_err(err);
        exit(1);
    }
    qemu_register_reset(startup_reset, machine);
    startup_reset(machine);
}
