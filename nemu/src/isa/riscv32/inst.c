/***************************************************************************************
 * Copyright (c) 2014-2022 Zihao Yu, Nanjing University
 *
 * NEMU is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 *PSL v2. You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 *KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 ***************************************************************************************/

#include "local-include/reg.h"
#include <cpu/cpu.h>
#include <cpu/decode.h>
#include <cpu/ifetch.h>
#define CALL(a, b) MUXDEF(FTRACE, func_call(a, b), (void)0)
#define RET(a, b) MUXDEF(FTRACE, func_ret(a, b), (void)0)
#define INTR isa_raise_intr
#define R(i) gpr(i)
#define Mr vaddr_read
#define Mw vaddr_write

enum {
    TYPE_I,
    TYPE_U,
    TYPE_S,
    TYPE_N, // none
    TYPE_J,
    TYPE_B,
    TYPE_R,
};

#define src1R()                                                                \
    do {                                                                       \
        *src1 = R(rs1);                                                        \
    } while (0)
#define src2R()                                                                \
    do {                                                                       \
        *src2 = R(rs2);                                                        \
    } while (0)
#define immI()                                                                 \
    do {                                                                       \
        *imm = SEXT(BITS(i, 31, 20), 12);                                      \
    } while (0)
#define immU()                                                                 \
    do {                                                                       \
        *imm = SEXT(BITS(i, 31, 12), 20) << 12;                                \
    } while (0)
#define immS()                                                                 \
    do {                                                                       \
        *imm = (SEXT(BITS(i, 31, 25), 7) << 5) | BITS(i, 11, 7);               \
    } while (0)
#define immJ()                                                                 \
    do {                                                                       \
        *imm = ((((SEXT(BITS(i, 31, 30), 1) << 8) | BITS(i, 19, 12)) << 1 |    \
                 BITS(i, 21, 20))                                              \
                    << 10 |                                                    \
                BITS(i, 30, 21));                                              \
    } while (0)
#define immB()                                                                 \
    do {                                                                       \
        *imm = (SEXT(BITS(i, 31, 31), 1) << 12) | BITS(i, 7, 7) << 11 |        \
               BITS(i, 30, 25) << 5 | BITS(i, 11, 8) << 1;                     \
    } while (0)

static void decode_operand(Decode *s, int *rd, word_t *src1, word_t *src2,
                           word_t *imm, int type) {
    uint32_t i = s->isa.inst.val;
    int rs1 = BITS(i, 19, 15);
    int rs2 = BITS(i, 24, 20);
    *rd = BITS(i, 11, 7);
    switch (type) {
    case TYPE_I:
        src1R();
        immI();
        break;
    case TYPE_U:
        immU();
        break;
    case TYPE_S:
        src1R();
        src2R();
        immS();
        break;
    case TYPE_J:
        immJ();
        break;
    case TYPE_B:
        src1R();
        src2R();
        immB();
        break;
    case TYPE_R:
        src1R();
        src2R();
        break;
    }
}

static int decode_exec(Decode *s) {
    int rd = 0;
    word_t src1 = 0, src2 = 0, imm = 0;
    s->dnpc = s->snpc;

#define INSTPAT_INST(s) ((s)->isa.inst.val)
#define INSTPAT_MATCH(s, name, type, ... /* execute body */)                   \
    {                                                                          \
        decode_operand(s, &rd, &src1, &src2, &imm, concat(TYPE_, type));       \
        __VA_ARGS__;                                                           \
    }

    INSTPAT_START();
    // INSTPAT("??????? ????? ????? 000 ????? 00100 11", li, I,
    //          R(rd) = R(0) + imm);
    INSTPAT("??????? ????? ????? 010 ????? 00000 11", lw, I,
            R(rd) = Mr(src1 + imm, 4));
    INSTPAT("??????? ????? ????? 000 ????? 00100 11", addi, I,
            R(rd) = src1 + imm);

    INSTPAT("??????? ????? ????? 011 ????? 00100 11", seqz, I,
            R(rd) = src1 < 1 ? 1 : 0);
    // jal	ra,80000018   将 PC+4 的值保存到 rd 寄存器中，然后设置 PC = PC +
    // offset  拿到的imm要左移一位
    INSTPAT("??????? ????? ????? ??? ????? 11011 11", jal, J, imm = imm << 1,
            R(rd) = s->pc + 4, s->dnpc = s->pc + imm);

    //           rs2=ra  rs1=sp
    // 0000 000(0 0001) (0001 0)(010) (0110 0)(010 0011)
    // 00112623    sw	ra,12(sp) 的含义是将寄存器 ra 中的值存储到地址 sp+12
    // 的内存位置中。在这里，ra 是链接寄存器（link register），sp
    // 是栈指针寄存器（stack pointer register） 存一个字
    INSTPAT("??????? ????? ????? 010 ????? 01000 11", sw, S,
            Mw(src1 + imm, 4, src2));
    INSTPAT("??????? ????? ????? 101 ????? 01100 11", srl, R,
            R(rd) = src1 >> (src2 & 31));
    // ret # 函数返回，等效于 jr ra，等效于 jalr x0, ra, 0
    // 0000 0000 0000 (0000 1)(000) (0000 0)(110 0111)
    // 00008067          	ret
    INSTPAT("??????? ????? ????? 000 ????? 11001 11", jalr, I,
            R(rd) = s->pc + 4, s->dnpc = src1 + imm);
    INSTPAT("0000000 00000 00001 000 00000 11001 11", ret, I,
            s->dnpc = src1 - (src1 & 1), RET(s->pc, s->dnpc));
    INSTPAT("??????? ????? ????? 000 ????? 11000 11", beq, B,
            s->dnpc = (src1 == src2) ? s->pc + imm : s->dnpc);
    // ----上面是添加的指令--------------------------

    INSTPAT("??????? ????? ????? ??? ????? 00101 11", auipc, U,
            R(rd) = s->pc + imm);
    INSTPAT("??????? ????? ????? 100 ????? 00000 11", lbu, I,
            R(rd) = Mr(src1 + imm, 1));
    INSTPAT("??????? ????? ????? 000 ????? 01000 11", sb, S,
            Mw(src1 + imm, 1, src2));

    INSTPAT("0000000 00001 00000 000 00000 11100 11", ebreak, N,
            NEMUTRAP(s->pc, R(10))); // R(10) is $a0
    INSTPAT("??????? ????? ????? ??? ????? ????? ??", inv, N, INV(s->pc));
    INSTPAT_END();

    R(0) = 0; // reset $zero to 0

    return 0;
}

int isa_exec_once(Decode *s) {
    s->isa.inst.val = inst_fetch(&s->snpc, 4);
    return decode_exec(s);
}
