/******************************************************************************
 *                                                                            *
 * Copyright (C) 2023 MachineWare GmbH                                        *
 * All Rights Reserved                                                        *
 *                                                                            *
 * This is work is licensed under the terms described in the LICENSE file     *
 * found in the root directory of this source tree.                           *
 *                                                                            *
 ******************************************************************************/

#include "testing.h"

static void emit_sev(u8*& buf, u32 ev_id) {
    buf[0] = 0b00110100;
    buf[1] = ev_id << 3;
    buf += 2;
}

static void emit_end(u8*& buf) {
    buf[0] = 0b00000000;
    buf += 1;
}

static void emit_ld(u8*& buf) {
    buf[0] = 0b00000100;
    buf += 1;
}

static void emit_st(u8*& buf) {
    buf[0] = 0b00001000;
    buf += 1;
}

static void emit_rw_loop(u8*& buf, u32 iterations) {
    buf[0] = 0b00100000;                  // DMALP
    buf[1] = static_cast<u8>(iterations); // DMALP arg
    buf += 2;

    emit_ld(buf);
    emit_st(buf);

    buf[0] = 0b00111000; // DMALPEND
    buf[1] = 0x2;        // DMALPEND arg
    buf += 2;
}

enum move_target {
    SAR = 0,
    CCR = 1,
    DAR = 2,
};

static void emit_mov(u8*& buf, move_target target, u32 val) {
    buf[0] = 0b10111100; // DMAMOV
    buf[1] = static_cast<u8>(target);
    buf[2] = static_cast<u8>(val >> 0);
    buf[3] = static_cast<u8>(val >> 8);
    buf[4] = static_cast<u8>(val >> 16);
    buf[5] = static_cast<u8>(val >> 24);
    buf += 6;
}

static void emit_configuration(u8*& buf, bool non_secure, u32 src_burst_size,
                               u32 src_burst_len, u32 src_address,
                               u32 src_increment, u32 dst_burst_size,
                               u32 dst_burst_len, u32 dst_address,
                               u32 dst_increment) {
    u32 ccr_val = 0;
    ccr_val |= ((non_secure & 0b1) << 9) | ((non_secure & 0b1) << 23) |
               ((src_burst_size & 0b111) << 1) |
               ((src_burst_len & 0b1111) << 4) | ((src_increment & 0b1) << 0) |
               ((dst_burst_size & 0b111) << 15) |
               ((dst_burst_len & 0b1111) << 18) |
               ((dst_increment & 0b1) << 14);

    emit_mov(buf, CCR, ccr_val);
    emit_mov(buf, SAR, src_address);
    emit_mov(buf, DAR, dst_address);
}

class pl330_bench : public test_base
{
public:
    tlm_initiator_socket out;

    gpio_initiator_socket reset_out;

    gpio_target_socket irq_in;

    generic::memory mem;

    dma::pl330 dma;

    pl330_bench(const sc_module_name& nm):
        test_base(nm),
        out("out"),
        reset_out("reset_out"),
        irq_in("irq_in"),
        mem("mem", 256 * mwr::MiB),
        dma("pl330") {
        out.bind(dma.in);
        dma.dma.bind(mem.in);

        dma.irq[0].bind(irq_in);
        dma.irq_abort.stub();

        reset_out.bind(dma.rst);
        reset_out.bind(mem.rst);

        clk.bind(mem.clk);
        clk.bind(dma.clk);
    }

    void execute_dbg_insn(u32 channel_nr, size_t start_address) {
        u32 channel_non_secure = (dma.channels[channel_nr].csr >> 21) & 0b1;
        u32 dbginst0_val = 0b00000001 | 0b00000000 << 8 |
                           (0b10100000 | channel_non_secure << 1) << 16 |
                           (channel_nr & 0xf) << 24;
        out.write(dma.dbginst0.get_address(), &dbginst0_val, 4);

        u32 dbginst1_val = start_address;
        out.write(dma.dbginst1.get_address(), &dbginst1_val, 4);

        u32 dbgctr_val = 0b00;
        out.write(dma.dbgcmd.get_address(), &dbgctr_val, 4);
    }

    void set_ev_to_irq(u32 ev_id) {
        u32 irq_enable_bit;
        out.read(dma.inten.get_address(), &irq_enable_bit, 4);
        irq_enable_bit |= 0b1 << ev_id;
        out.write(dma.inten.get_address(), &irq_enable_bit, 4);
    }

    virtual void run_test() override {
        dma.reset();
        auto* data_char_ptr = mem.data();
        u8* const channel_insn_buffer = &data_char_ptr[0x1000];
        const u32 src_buffer_addr = 0x2000;
        const u32 dst_buffer_addr = 0x3000;
        u8* insn_buf_tail = channel_insn_buffer;

        for (int i = 0; i < 16; i++)
            (&data_char_ptr[src_buffer_addr])[i] = i;

        emit_configuration(insn_buf_tail, !!(dma.channels[0].csr & (1 << 21)),
                           1u, 1u, src_buffer_addr, 1u, 1u, 1u,
                           dst_buffer_addr, 1u);
        emit_rw_loop(insn_buf_tail, 16);

        u32 ev_id = 0;
        set_ev_to_irq(ev_id);
        emit_sev(insn_buf_tail, ev_id);
        emit_end(insn_buf_tail);

        execute_dbg_insn(0, 0x1000);

        while (!irq_in)
            wait(1.0, sc_core::SC_SEC);

        for (int i = 0; i < 16; i++) {
            EXPECT_EQ(data_char_ptr[src_buffer_addr + i],
                      data_char_ptr[dst_buffer_addr + i]);
            EXPECT_EQ(i, data_char_ptr[dst_buffer_addr + i]);
        }
    }
};

TEST(arm_pl330, main) {
    pl330_bench bench("bench");
    sc_core::sc_start();
}
