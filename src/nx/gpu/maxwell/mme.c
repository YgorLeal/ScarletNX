/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the macro engine (MME): uploading the game's macros and running them as method calls */
#include "gpu/maxwell/engines_int.h"
/* ============================================================ MME (macros) */
/* The macro's state is its channel's (GpuMme): a method it sends can make the stream wait (gpu_park), and the macro
   then stops after that method, as the 3D engine runs the macro's later methods only once the wait is over. It goes
   on (mme_resume) before the stream's next word. Delay slots run as steps of their own, so a stop can fall between a
   branch or an exit and its slot */
static void mme_send(GpuChannel* ch, u32 v) {
    GpuMme* m = &ch->mme;
    /* a send to the macro methods (0xE00+) only sets the register, as in Ryujinx, where macros start from the
       pushbuffer alone: going through maxwell3d_method would start a macro inside this one, over the parameters
       (ch->mme_params) this one is still reading */
    if (m->method >= 0xE00)
        ch->r3d[m->method] = v;
    else
        maxwell3d_method(ch, m->method, v, 1);
    m->method += m->incr;
    m->method &= 0xFFF;
}
static void mme_setmethod(GpuMme* m, u32 v) {
    m->method = v & 0xFFF;
    m->incr = (v >> 12) & 0x3F;
}
static u32 mme_fetch(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    return m->pi < m->np ? ch->mme_params[m->pi++] : 0;
}
static void mme_setreg(GpuMme* m, u32 r, u32 v) {
    if (r) m->regs[r] = v;
}
static void mme_result(GpuChannel* ch, u32 op, u32 dst, u32 res) {
    GpuMme* m = &ch->mme;
    switch (op) {
        case 0: mme_setreg(m, dst, mme_fetch(ch)); break;
        case 1: mme_setreg(m, dst, res); break;
        case 2:
            mme_setreg(m, dst, res);
            mme_setmethod(m, res);
            break;
        case 3:
            mme_setreg(m, dst, mme_fetch(ch));
            mme_send(ch, res);
            break;
        case 4:
            mme_setreg(m, dst, res);
            mme_send(ch, res);
            break;
        case 5:
            mme_setreg(m, dst, mme_fetch(ch));
            mme_setmethod(m, res);
            break;
        case 6:
            mme_setreg(m, dst, res);
            mme_setmethod(m, res);
            mme_send(ch, mme_fetch(ch));
            break;
        case 7:
            mme_setreg(m, dst, res);
            mme_setmethod(m, res);
            mme_send(ch, (res >> 12) & 0x3F);
            break;
    }
}
static u32 mme_alu(GpuChannel* ch, u32 op, u32 a, u32 b) {
    GpuMme* m = &ch->mme;
    u64 r;
    switch (op) {
        case 0:
            r = (u64)a + b;
            m->carry = (u32)(r >> 32);
            return (u32)r;
        case 1:
            r = (u64)a + b + m->carry;
            m->carry = (u32)(r >> 32);
            return (u32)r;
        case 2:
            r = (u64)a - b;
            m->carry = r < 0x100000000ull;
            return (u32)r;
        case 3:
            r = (u64)a - b - (m->carry ? 0 : 1);
            m->carry = r < 0x100000000ull;
            return (u32)r;
        case 8: return a ^ b;
        case 9: return a | b;
        case 10: return a & b;
        case 11: return a & ~b;
        case 12: return ~(a & b);
    }
    {
        WARN_LIMIT(8, "mme: alu %u (not a Maxwell ALU operation) in macro %u at pc %x\n", op, m->idx, m->pc - 4);
        static int dumped;
        if (!dumped) {
            dumped = 1;
            u32 st = ch->mme_start[m->idx & 0xFF];
            LOG("mme: macro %u starts at word %x (upload pointer %x); code from there:\n", m->idx, st,
                ch->mme_upload_ptr);
            for (u32 i = 0; i < 64; i += 8) {
                const u32* c = &ch->mme_code[(st + i) & 0x1FFF];
                LOG("  %04x: %08x %08x %08x %08x %08x %08x %08x %08x\n", st + i, c[0], c[1], c[2], c[3], c[4], c[5],
                    c[6], c[7]);
            }
            LOG("  macro start table:");
            for (u32 i = 0; i < 0x80; i++) {
                if (i % 16 == 0) LOG("\n   ");
                LOG(" %x", ch->mme_start[i]);
            }
            LOG("\n");
        }
    }
    return 0;
}
/* runs one instruction: 0 when the macro has ended */
static int mme_step(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    int in_slot = m->slot;
    m->slot = 0;
    u32 base = m->pc;
    u32 w = ch->mme_code[(m->pc / 4) & 0x1FFF];
    if (m->delayed) {
        m->pc = m->dpc;
        m->delayed = 0;
    } else
        m->pc += 4;
    u32 op = w & 7, rop = (w >> 4) & 7, dst = (w >> 8) & 7, sa = (w >> 11) & 7, sb = (w >> 14) & 7;
    i32 imm = (i32)(w >> 14); /* imm: 18 bits signed @14 */
    if (imm & 0x20000) imm |= ~0x3FFFF;
    u32 bf_src = (w >> 17) & 31, bf_size = (w >> 22) & 31, bf_dst = (w >> 27) & 31;
    u32 mask = (1u << bf_size) - 1;
    if (bf_size == 0) mask = 0;
    switch (op) {
        case 0: mme_result(ch, rop, dst, mme_alu(ch, (w >> 17) & 31, m->regs[sa], m->regs[sb])); break;
        case 1: mme_result(ch, rop, dst, m->regs[sa] + (u32)imm); break;
        case 2: {
            u32 d = m->regs[sa], s = m->regs[sb];
            s = (s >> bf_src) & mask;
            d &= ~(mask << bf_dst);
            d |= s << bf_dst;
            mme_result(ch, rop, dst, d);
            break;
        }
        case 3: {
            u32 d = m->regs[sa], s = m->regs[sb];
            mme_result(ch, rop, dst, ((s >> (d & 31)) & mask) << bf_dst); /* shift counts as Ryujinx's (mod 32) */
            break;
        }
        case 4: {
            u32 d = m->regs[sa], s = m->regs[sb];
            mme_result(ch, rop, dst, ((s >> bf_src) & mask) << (d & 31));
            break;
        }
        case 5: {
            u32 a = (m->regs[sa] + (u32)imm) & 0xFFF;
            mme_result(ch, rop, dst, ch->r3d[a]);
            break;
        }
        case 7: {
            u32 v = m->regs[sa];
            int nz = (w >> 4) & 1, annul = (w >> 5) & 1;
            int taken = nz ? v != 0 : v == 0;
            if (taken) { /* a taken branch's exit bit doesn't count */
                if (annul)
                    m->pc = base + (u32)(imm * 4);
                else { /* the next instruction is its delay slot */
                    m->delayed = 1;
                    m->dpc = base + (u32)(imm * 4);
                    m->slot = 1;
                }
                goto done;
            }
            break; /* not taken: the exit bit counts, as for any instruction (as in Ryujinx's interpreter) */
        }
        default: WARN("mme: op %u\n", op); break;
    }
    if (((w >> 7) & 1) && !in_slot) { /* exit, after the next instruction, its delay slot */
        m->slot = 1;
        m->exiting = 1;
        return 1;
    }
done:
    /* an exit's delay slot ends the macro, unless it is a taken branch: then that branch's own slot runs first */
    return !(in_slot && m->exiting && !m->slot);
}
/* runs the macro until it ends, or a method it sent makes the stream wait */
static void mme_go(GpuChannel* ch) {
    GpuMme* m = &ch->mme;
    while (m->running) {
        if (m->steps++ >= 5000000) { /* a macro that never ends: the stream goes on without it */
            WARN_LIMIT(4, "mme: macro %u ran 5000000 instructions without ending, stopped\n", m->idx);
            m->running = 0;
        } else if (!mme_step(ch))
            m->running = 0;
        else if (ch->park)
            return;
    }
}
void mme_run(GpuChannel* ch, u32 idx, u32 np) {
    GpuMme* m = &ch->mme;
    memset(m, 0, sizeof *m);
    m->idx = idx;
    m->np = np;
    m->pi = 1;
    m->regs[1] = np ? ch->mme_params[0] : 0;
    m->pc = ch->mme_start[idx & 0xFF] * 4;
    m->running = 1;
    mme_go(ch);
}
void mme_resume(GpuChannel* ch) { mme_go(ch); }
