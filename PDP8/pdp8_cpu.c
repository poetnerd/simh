/* pdp8_cpu.c: PDP-8 CPU simulator

   Copyright (c) 1993-2021, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   ----------------------------------------------------------------------------

   Portions copyright © 2015 by Oscar Vermeulen
                      © 2016-2018 by Warren Young
                      © 2021 by HB Eggenstein
                      © 2021 by Steve Tockey

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS LISTED ABOVE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the names of the authors above shall
   not be used in advertising or otherwise to promote the sale, use or other
   dealings in this Software without prior written authorization from those
   authors.

   ----------------------------------------------------------------------------

   cpu          central processor

   21-Oct-21    RMS     Fixed bug in reporting device conflicts (Hans-Bernd Eggenstein)
   07-Sep-17    RMS     Fixed sim_eval declaration in history routine (COVERITY)
   09-Mar-17    RMS     Fixed PCQ_ENTRY for interrupts (COVERITY)
   13-Feb-17    RMS     RESET clear L'AC, per schematics
   28-Jan-17    RMS     Renamed switch register variable to SR, per request
   18-Sep-16    RMS     Added alternate dispatch table for non-contiguous devices
   17-Sep-13    RMS     Fixed boot in wrong field problem (Dave Gesswein)
   28-Apr-07    RMS     Removed clock initialization
   30-Oct-06    RMS     Added idle and infinite loop detection
   30-Sep-06    RMS     Fixed SC value after DVI overflow (Don North)
   22-Sep-05    RMS     Fixed declarations (Sterling Garwood)
   16-Aug-05    RMS     Fixed C++ declaration and cast problems
   06-Nov-04    RMS     Added =n to SHOW HISTORY
   31-Dec-03    RMS     Fixed bug in set_cpu_hist
   13-Oct-03    RMS     Added instruction history
                        Added TSC8-75 support (Bernhard Baehr)
   12-Mar-03    RMS     Added logical name support
   04-Oct-02    RMS     Revamped device dispatching, added device number support
   06-Jan-02    RMS     Added device enable/disable routines
   30-Dec-01    RMS     Added old PC queue
   16-Dec-01    RMS     Fixed bugs in EAE
   07-Dec-01    RMS     Revised to use new breakpoint package
   30-Nov-01    RMS     Added RL8A, extended SET/SHOW support
   16-Sep-01    RMS     Fixed bug in reset routine, added KL8A support
   10-Aug-01    RMS     Removed register from declarations
   17-Jul-01    RMS     Moved function prototype
   07-Jun-01    RMS     Fixed bug in JMS to non-existent memory
   25-Apr-01    RMS     Added device enable/disable support
   18-Mar-01    RMS     Added DF32 support
   05-Mar-01    RMS     Added clock calibration support
   15-Feb-01    RMS     Added DECtape support
   14-Apr-99    RMS     Changed t_addr to unsigned

   The register state for the PDP-8 is:

   AC<0:11>             accumulator
   MQ<0:11>             multiplier-quotient
   L                    link flag
   PC<0:11>             program counter
   MA<0:11>             memory address
   MB<0:11>             memory buffer
   Major_State<0:1>     major state register
   IF<0:2>              instruction field
   IB<0:2>              instruction buffer
   DF<0:2>              data field
   UF                   user flag
   UB                   user buffer
   SF<0:6>              interrupt save field

   The PDP-8 has three instruction formats: memory reference, I/O transfer,
   and operate.  The memory reference format is:

     0  1  2  3  4  5  6  7  8  9 10 11
   +--+--+--+--+--+--+--+--+--+--+--+--+
   |   op   |in|zr|    page offset     |        memory reference
   +--+--+--+--+--+--+--+--+--+--+--+--+

   <0:2>        mnemonic        action

    000         AND             AC = AC & M[MA]
    001         TAD             L'AC = AC + M[MA]
    010         DCA             M[MA] = AC, AC = 0
    011         ISZ             M[MA] = M[MA] + 1, skip if M[MA] == 0
    100         JMS             M[MA] = PC, PC = MA + 1
    101         JMP             PC = MA

   <3:4>        mode            action
    00  page zero               MA = IF'0'IR<5:11>
    01  current page            MA = IF'PC<0:4>'IR<5:11>
    10  indirect page zero      MA = xF'M[IF'0'IR<5:11>]
    11  indirect current page   MA = xF'M[IF'PC<0:4>'IR<5:11>]

   where x is D for AND, TAD, ISZ, DCA, and I for JMS, JMP.

   Memory reference instructions can access an address space of 32K words.
   The address space is divided into eight 4K word fields; each field is
   divided into thirty-two 128 word pages.  An instruction can directly
   address, via its 7b offset, locations 0-127 on page zero or on the current
   page.  All 32k words can be accessed via indirect addressing and the
   instruction and data field registers.  If an indirect address is in
   locations 0010-0017 of any field, the indirect address is incremented
   and rewritten to memory before use.

   The I/O transfer format is as follows:

     0  1  2  3  4  5  6  7  8  9 10 11
   +--+--+--+--+--+--+--+--+--+--+--+--+
   |   op   |      device     | pulse  |        I/O transfer
   +--+--+--+--+--+--+--+--+--+--+--+--+

   The IO transfer instruction sends the the specified pulse to the
   specified I/O device.  The I/O device may take data from the AC,
   return data to the AC, initiate or cancel operations, or skip on
   status.

   The operate format is as follows:

   +--+--+--+--+--+--+--+--+--+--+--+--+
   | 1| 1| 1| 0|  |  |  |  |  |  |  |  |        operate group 1
   +--+--+--+--+--+--+--+--+--+--+--+--+
                |  |  |  |  |  |  |  |
                |  |  |  |  |  |  |  +--- increment AC  3
                |  |  |  |  |  |  +--- rotate 1 or 2    4
                |  |  |  |  |  +--- rotate left         4
                |  |  |  |  +--- rotate right           4
                |  |  |  +--- complement L              2
                |  |  +--- complement AC                2
                |  +--- clear L                         1
                +-- clear AC                            1

   +--+--+--+--+--+--+--+--+--+--+--+--+
   | 1| 1| 1| 1|  |  |  |  |  |  |  | 0|        operate group 2
   +--+--+--+--+--+--+--+--+--+--+--+--+
                |  |  |  |  |  |  |
                |  |  |  |  |  |  +--- halt             3
                |  |  |  |  |  +--- or switch register  3
                |  |  |  |  +--- reverse skip sense     1
                |  |  |  +--- skip on L != 0            1
                |  |  +--- skip on AC == 0              1
                |  +--- skip on AC < 0                  1
                +-- clear AC                            2

   +--+--+--+--+--+--+--+--+--+--+--+--+
   | 1| 1| 1| 1|  |  |  |  |  |  |  | 1|        operate group 3
   +--+--+--+--+--+--+--+--+--+--+--+--+
                |  |  |  | \______/
                |  |  |  |     |
                |  |  +--|-----+--- EAE command         3
                |  |     +--- AC -> MQ, 0 -> AC         2
                |  +--- MQ v AC --> AC                  2
                +-- clear AC                            1

  The operate instruction can be microprogrammed to perform operations
  on the AC, MQ, and link.

  This routine is the instruction decode routine for the PDP-8.
   It is called from the simulator control program to execute
   instructions in simulated memory, starting at the simulated PC.
   It runs until 'reason' is set non-zero.

   General notes:

   1. Reasons to stop.  The simulator can be stopped by:

        HALT instruction
        breakpoint encountered
        unimplemented instruction and stop_inst flag set
        I/O error in I/O simulator

   2. Interrupts.  Interrupts are maintained by three parallel variables:

        dev_done        device done flags
        int_enable      interrupt enable flags
        int_req         interrupt requests

      In addition, int_req contains the interrupt enable flag, the
      CIF not pending flag, and the ION not pending flag.  If all
      three of these flags are set, and at least one interrupt request
      is set, then an interrupt occurs.

   3. Non-existent memory.  On the PDP-8, reads to non-existent memory
      return zero, and writes are ignored.  In the simulator, the
      largest possible memory is instantiated and initialized to zero.
      Thus, only writes outside the current field (indirect writes) need
      be checked against actual memory size.

   3. Adding I/O devices.  These modules must be modified:

        pdp8_defs.h     add device number and interrupt definitions
        pdp8_sys.c      add sim_devices table entry
*/

#include "pdp8_defs.h"

#define PCQ_SIZE        64                              /* must be 2**n */
#define PCQ_MASK        (PCQ_SIZE - 1)
#define PCQ_ENTRY(x)    pcq[pcq_p = (pcq_p - 1) & PCQ_MASK] = x
#define UNIT_V_NOEAE    (UNIT_V_UF)                     /* EAE absent */
#define UNIT_NOEAE      (1 << UNIT_V_NOEAE)
#define UNIT_V_MSIZE    (UNIT_V_UF + 1)                 /* dummy mask */
#define UNIT_MSIZE      (1 << UNIT_V_MSIZE)
#define OP_KSF          06031                           /* for idle */

#define HIST_PC         0x40000000
#define HIST_MIN        64
#define HIST_MAX        65536

typedef struct {
    int32               pc;
    int32               ea;
    int16               ir;
    int16               opnd;
    int16               lac;
    int16               mq;
    } InstHistory;

uint16 M[MAXMEMSIZE] = { 0 };                           /* main memory */
int32 saved_LAC = 0;                                    /* saved L'AC */
int32 saved_MQ = 0;                                     /* saved MQ */
int32 saved_PC = 0;                                     /* saved IF'PC */
int32 saved_MA = 0;                                     /* saved MA */
int32 saved_IR = 0;                                     /* saved IR */
int16 saved_Major_State = 1;                            /* saved Major State */
int32 saved_DF = 0;                                     /* saved Data Field */
int32 IB = -1;                                          /* Instruction Buffer */
int32 SF = 0;                                           /* Save Field */
int32 emode = 0;                                        /* EAE mode */
int32 gtf = 0;                                          /* EAE gtf flag */
int32 SC = 0;                                           /* EAE shift count */
int32 UB = 0;                                           /* User mode Buffer */
int32 UF = 0;                                           /* User mode Flag */
int32 SR = 0;                                           /* Switch Register */
int32 tsc_ir = 0;                                       /* TSC8-75 IR */
int32 tsc_pc = 0;                                       /* TSC8-75 PC */
int32 tsc_cdf = 0;                                      /* TSC8-75 CDF flag */
int32 tsc_enb = 0;                                      /* TSC8-75 enabled */
int32 cpu_astop = 0;                                    /* address stop */
int16 pcq[PCQ_SIZE] = { 0 };                            /* PC queue */
int32 pcq_p = 0;                                        /* PC queue ptr */
REG *pcq_r = NULL;                                      /* PC queue reg ptr */
int32 dev_done = 0;                                     /* dev done flags */
int32 int_enable = INT_INIT_ENABLE;                     /* intr enables */
int32 int_req = 0;                                      /* intr requests */
int32 stop_inst = 0;                                    /* trap on ill inst */
int32 (*dev_tab[DEV_MAX])(int32 IR, int32 dat);         /* device dispatch */
int32 hst_p = 0;                                        /* history pointer */
int32 hst_lnt = 0;                                      /* history length */
InstHistory *hst = NULL;                                /* instruction history */

t_stat cpu_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_reset (DEVICE *dptr);
t_stat cpu_set_size (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat cpu_set_hist (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat cpu_show_hist (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_bool build_dev_tab (void);

/* CPU data structures

   cpu_dev      CPU device descriptor
   cpu_unit     CPU unit descriptor
   cpu_reg      CPU register list
   cpu_mod      CPU modifier list
*/

UNIT cpu_unit = { UDATA (NULL, UNIT_FIX + UNIT_BINK, MAXMEMSIZE) };

REG cpu_reg[] = {
    { ORDATAD (PC, saved_PC, 15, "program counter") },
    { ORDATAD (MA, saved_MA, 12, "memory address") },
    { ORDATAD (next_Major_State, saved_Major_State, 2, "major state") },
    { ORDATAD (AC, saved_LAC, 12, "accumulator") },
    { FLDATAD (L, saved_LAC, 12, "link") },
    { ORDATAD (MQ, saved_MQ, 12, "multiplier-quotient") },
    { ORDATAD (SR, SR, 12, "front panel switches") },
    { GRDATAD (IF, saved_PC, 8, 3, 12, "instruction field") },
    { GRDATAD (DF, saved_DF, 8, 3, 12, "data field") },
    { GRDATAD (IB, IB, 8, 3, 12, "instruction field buffter") },
    { ORDATAD (SF, SF, 7, "save field") },
    { FLDATAD (UB, UB, 0, "user mode buffer") },
    { FLDATAD (UF, UF, 0, "user mode flag") },
    { ORDATAD (SC, SC, 5, "EAE shift counter") },
    { FLDATAD (GTF, gtf, 0, "EAE greater than flag") },
    { FLDATAD (EMODE, emode, 0, "EAE mode (0 = A, 1 = B)") },
    { FLDATAD (ION, int_req, INT_V_ION, "interrupt enable") },
    { FLDATAD (ION_DELAY, int_req, INT_V_NO_ION_PENDING, "interrupt enable delay for ION") },
    { FLDATAD (CIF_DELAY, int_req, INT_V_NO_CIF_PENDING, "interrupt enable delay for CIF") },
    { FLDATAD (PWR_INT, int_req, INT_V_PWR, "power fail interrupt") },
    { FLDATAD (UF_INT, int_req, INT_V_UF, "user mode violation interrupt") },
    { ORDATAD (INT, int_req, INT_V_ION+1, "interrupt pending flags"), REG_RO },
    { ORDATAD (DONE, dev_done, INT_V_DIRECT, "device done flags"), REG_RO },
    { ORDATAD (ENABLE, int_enable, INT_V_DIRECT, "device interrupt enable flags"), REG_RO },
    { BRDATAD (PCQ, pcq, 8, 15, PCQ_SIZE, "PC prior to last JMP, JMS, or interrupt;                                        most recent PC change first"), REG_RO+REG_CIRC },
    { ORDATA (PCQP, pcq_p, 6), REG_HRO },
    { FLDATAD (STOP_INST, stop_inst, 0, "stop on undefined instruction") },
    { ORDATAD (WRU, sim_int_char, 8, "interrupt character") },
    { NULL }
    };

MTAB cpu_mod[] = {
    { UNIT_NOEAE, UNIT_NOEAE, "no EAE", "NOEAE", NULL },
    { UNIT_NOEAE, 0, "EAE", "EAE", NULL },
    { MTAB_XTD|MTAB_VDV, 0, "IDLE", "IDLE", &sim_set_idle, &sim_show_idle },
    { MTAB_XTD|MTAB_VDV, 0, NULL, "NOIDLE", &sim_clr_idle, NULL },
    { UNIT_MSIZE, 4096, NULL, "4K", &cpu_set_size },
    { UNIT_MSIZE, 8192, NULL, "8K", &cpu_set_size },
    { UNIT_MSIZE, 12288, NULL, "12K", &cpu_set_size },
    { UNIT_MSIZE, 16384, NULL, "16K", &cpu_set_size },
    { UNIT_MSIZE, 20480, NULL, "20K", &cpu_set_size },
    { UNIT_MSIZE, 24576, NULL, "24K", &cpu_set_size },
    { UNIT_MSIZE, 28672, NULL, "28K", &cpu_set_size },
    { UNIT_MSIZE, 32768, NULL, "32K", &cpu_set_size },
    { MTAB_XTD|MTAB_VDV|MTAB_NMO|MTAB_SHP, 0, "HISTORY", "HISTORY",
      &cpu_set_hist, &cpu_show_hist },
    { 0 }
    };

DEVICE cpu_dev = {
    "CPU", &cpu_unit, cpu_reg, cpu_mod,
    1, 8, 15, 1, 8, 12,
    &cpu_ex, &cpu_dep, &cpu_reset,
    NULL, NULL, NULL,
    NULL, 0
    };

// These definitions support simulation of Fetch, Defer, and Execute cpu major states
// so that the Sing Step switch can behave as on a real pdp-8. The major states are
// detailed in, for example, the 1973 small computer handbook on pages 3-18 to 3-22.
// http://bitsavers.informatik.uni-stuttgart.de/pdf/dec/pdp8/handbooks/Small_Computer_Handbook_1973.pdf
#define FETCH_state     1
#define DEFER_state     2
#define EXECUTE_state   3


t_stat sim_instr (void)
{
int32 IR, MB, IF, DF, LAC, MQ;
uint32 PC, MA;
uint16 this_Major_State, next_Major_State;
int32 device, pulse, temp, iot_data;
t_stat reason;
int op_code = 0;

/* Restore register state */

if (build_dev_tab ())                                   /* build dev_tab */
    return SCPE_STOP;
PC = saved_PC & 007777;                                 /* load local copies */
MA = saved_MA & 007777;
IR = saved_IR & 007777;
this_Major_State = next_Major_State = saved_Major_State;
IF = saved_PC & 070000;
DF = saved_DF & 070000;
LAC = saved_LAC & 017777;
MQ = saved_MQ & 07777;
int_req = INT_UPDATE;
reason = 0;

////////////////////////////////////////////////////////////////////////////////////
// For some strange reason, there are times when IB can be essentially uninitialized.
// It seems harmless most of the time, but it's deadly on TSS-8 startup which is at
// address 24200. Then, at 24204 is a JMS 0060. The JMS code for the EXECUTE major
// state, below, necessarily uses IB to give correct behavior following a CIF. The
// killer problem is that when--without this if () test--that TSS-8 JMS executes,
// IB is 0 not 2 so it is interpreted as a JMS to 00060 instead of the necessary
// JMS to 20060. Having this test forces IB to be set to IF the first time through
// so it's no longer "uninitialized" with respect to the simulated execution. See
// the TBD FIX in the code for the Start switch in main.c.in. When that is fixed,
// this can be removed.
if (IB = -1)
    IB = IF;
////////////////////////////////////////////////////////////////////////////////////

/* Main instruction fetch/decode loop */

while (reason == 0) {                                   /* loop until halted */

    // Allow clean exit to SCP: https://github.com/simh/simh/issues/387
    if (cpu_astop != 0) {
        cpu_astop = 0;
        reason = SCPE_STOP;
        break;
        }

    if (sim_interval <= 0) {                            /* check clock queue */
        if ((reason = sim_process_event ())) {
            break;
            }
        }

    this_Major_State = next_Major_State;
    switch (this_Major_State) {

        case FETCH_state:
            // fetch state for all instructions, regardless of op code
            MA = IF | PC & 07777;                                   /* form PC */
            if (sim_brk_summ && 
                sim_brk_test (MA, (1u << SIM_BKPT_V_SPC) | SWMASK ('E'))) { /* breakpoint? */
                reason = STOP_IBKPT;                                /* stop simulation */
                break;
                }

            PC = (PC + 1) & 07777;                                  /* increment PC */
            int_req = int_req | INT_NO_ION_PENDING;                 /* clear ION delay */
            sim_interval = sim_interval - 1;

            IR = MB = M[MA];                                        /* fetch instruction */
            if (sim_brk_summ && 
                sim_brk_test (IR, (2u << SIM_BKPT_V_SPC) | SWMASK ('I'))) { /* breakpoint? */
                reason = STOP_OPBKPT;                               /* stop simulation */
                break;
                }

            if (hst_lnt) {                                      /* history enabled? */
                int32 ea;
                hst_p = (hst_p + 1);                            /* next entry */
                if (hst_p >= hst_lnt)
                    hst_p = 0;
                hst[hst_p].pc = MA | HIST_PC;                   /* save PC, IR, LAC, MQ */
                hst[hst_p].ir = IR;
                hst[hst_p].lac = LAC;
                hst[hst_p].mq = MQ;
                if (IR < 06000) {                               /* mem ref? */
                    if (IR & 0200)
                        ea = (MA & 077600) | (IR & 0177);
                    else ea = IF | (IR & 0177);                 /* direct addr */
                    if (IR & 0400) {                            /* indirect? */
                        if (IR < 04000) {                       /* mem operand? */
                            if ((ea & 07770) != 00010)
                                ea = DF | M[ea];
                            else ea = DF | ((M[ea] + 1) & 07777);
                            }
                        else {                                  /* no, jms/jmp */
                            if ((ea & 07770) != 00010)
                                ea = IB | M[ea];
                            else ea = IB | ((M[ea] + 1) & 07777);
                            }
                        }
                    hst[hst_p].ea = ea;                         /* save eff addr */
                    hst[hst_p].opnd = M[ea];                    /* save operand */
                    }
                }

            op_code = (IR >> 9) & 07;
            switch (op_code) {
                case 4: 
                    PCQ_ENTRY (MA);
                    // intentional fall-through for JMS
                case 0:case 1:case 2:case 3:
                    // Fetch state for MRIs: AND, TAD, ISZ, DCA, JMS
                    if (IR & 0200)                                  /* current page or zero page? */
                        MA = (MA & 007600) | (IR & 0177);           /* current page */
                    else
                        MA = IR & 0177;                             /* zero page */
                    if (IR & 0400)                                  /* indirect or direct? */
                        next_Major_State = DEFER_state;             /* indirect */
                    else
                        next_Major_State = EXECUTE_state;           /* direct */
                    break;
                    // end of case op_code 0..4: AND, TAD, ISZ, DCA, JMS
                case 5:
                    // Fetch state for JMP
/* Opcode 5, JMP.  From Bernhard Baehr's description of the TSC8-75:

   (In user mode) the current JMP opcode is moved to the ERIOT register, the ECDF
   flag is cleared. The address of the JMP instruction is loaded into the ERTB
   register and the TSC8-75 I/O flag is raised. Then the JMP is performed as usual
   (including the setting of IF, UF and clearing the interrupt inhibit flag). */

                    PCQ_ENTRY (MA);
                    if (IR & 0200)                                  /* current page or zero page? */
                        MA = (MA & 077600) | (IR & 0177);           /* current page */
                    else
                        MA = IF | (IR & 0177);                      /* zero page */
                    if (IR & 0400)                                  /* direct or indirect? */
                        next_Major_State = DEFER_state;             /* indirect JMP */
                    else {
                        if (UF) {                                   /* direct, user mode? */
                            tsc_ir = IR;                            /* save instruction */
                            tsc_cdf = 0;                            /* clear flag */
                            if (tsc_enb) {                          /* TSC8 enabled? */
                                tsc_pc = (PC - 1) & 07777;          /* save PC */
                                int_req = int_req | INT_TSC;        /* request intr */
                            }
                        }
                        if (((IR & 0200) == 0) &&  sim_idle_enab &&   /* current page? idling enabled? */
                            (IF == IB)) {                           /* to same bank? */
                            if (MA == ((PC - 2) & 07777)) {         /* 1) JMP *-1? */
                                if (!(int_req & (INT_ION|INT_TTI)) &&     /*    iof, TTI flag off? */
                                    (M[IB|((PC - 2) & 07777)] == OP_KSF)) /*  next is KSF? */
                                    sim_idle (TMR_CLK, FALSE);      /* we're idle */
                                }                                   /* end 1) JMP *-1 */
                            else if (MA == ((PC - 1) & 07777)) {    /* 2) JMP *? */
                                    if (!(int_req & INT_ION))       /*    iof? */
                                        reason = STOP_LOOP;         /* then infinite loop */
                                    else if (!(int_req & INT_ALL))  /*    ion, not intr? */
                                        sim_idle (TMR_CLK, FALSE);  /* we're idle */
                                     }                              /* end 2) JMP */
                            }                                       /* end current page, idle enabled, same bank */
                        IF = IB;                                    /* change IF */
                        UF = UB;                                    /* change UF */
                        int_req = int_req | INT_NO_CIF_PENDING;     /* clr intr inhibit */
                        PC = MA;
                        }                                           /* end direct JMP */
                    break;
                    // end of case op_code 5: JMP
                case 6:
                    // Fetch state for IOTs

/* From Bernhard Baehr's description of the TSC8-75:
   (In user mode) Additional to raising a user mode interrupt, the current IOT
   opcode is moved to the ERIOT register. When the IOT is a CDF instruction (62x1),
   the ECDF flag is set, otherwise it is cleared. */

                    if (UF) {                                       /* privileged? */
                        int_req = int_req | INT_UF;                 /* request intr */
                        tsc_ir = IR;                                /* save instruction */
                        if ((IR & 07707) == 06201)                  /* set/clear flag */
                            tsc_cdf = 1;
                        else tsc_cdf = 0;
                        break;
                        }
                    device = (IR >> 3) & 077;                       /* device = IR<3:8> */
                    pulse = IR & 07;                                /* pulse = IR<9:11> */
                    iot_data = LAC & 07777;                         /* AC unchanged */
                    switch (device) {                               /* decode IR<3:8> */
                    case 000:                                       /* CPU control */
                        switch (pulse) {                            /* decode IR<9:11> */

                        case 0:                                     /* SKON */
                            if (int_req & INT_ION)
                                PC = (PC + 1) & 07777;
                            int_req = int_req & ~INT_ION;
                            break;

                        case 1:                                     /* ION */
                            int_req = (int_req | INT_ION) & ~INT_NO_ION_PENDING;
                            break;

                        case 2:                                     /* IOF */
                            int_req = int_req & ~INT_ION;
                            break;

                        case 3:                                     /* SRQ */
                            if (int_req & INT_ALL)
                                PC = (PC + 1) & 07777;
                            break;

                        case 4:                                     /* GTF */
                            LAC = (LAC & 010000) |
                                  ((LAC & 010000) >> 1) | (gtf << 10) |
                                  (((int_req & INT_ALL) != 0) << 9) |
                                  (((int_req & INT_ION) != 0) << 7) | SF;
                            break;

                        case 5:                                     /* RTF */
                            gtf = ((LAC & 02000) >> 10);
                            UB = (LAC & 0100) >> 6;
                            IB = (LAC & 0070) << 9;
                            DF = (LAC & 0007) << 12;
                            LAC = ((LAC & 04000) << 1) | iot_data;
                            int_req = (int_req | INT_ION) & ~INT_NO_CIF_PENDING;
                            break;

                        case 6:                                     /* SGT */
                            if (gtf)
                                PC = (PC + 1) & 07777;
                            break;

                        case 7:                                     /* CAF */
                            gtf = 0;
                            emode = 0;
                            int_req = int_req & INT_NO_CIF_PENDING;
                            dev_done = 0;
                            int_enable = INT_INIT_ENABLE;
                            LAC = 0;
                            reset_all (1);                          /* reset all dev */
                            break;
                            }                                       /* end switch pulse */
                        break;

                    case 020:case 021:case 022:case 023:
                    case 024:case 025:case 026:case 027:            /* memory extension */
                        switch (pulse) {                            /* decode IR<9:11> */

                        case 1:                                     /* CDF */
                            DF = (IR & 0070) << 9;
                            break;

                        case 2:                                     /* CIF */
                            IB = (IR & 0070) << 9;
                            int_req = int_req & ~INT_NO_CIF_PENDING;
                            break;

                        case 3:                                     /* CDF CIF */
                            DF = IB = (IR & 0070) << 9;
                            int_req = int_req & ~INT_NO_CIF_PENDING;
                            break;

                        case 4:
                            switch (device & 07) {                  /* decode IR<6:8> */

                            case 0:                                 /* CINT */
                                int_req = int_req & ~INT_UF;
                                break;

                            case 1:                                 /* RDF */
                                LAC = LAC | (DF >> 9);
                                    break;

                            case 2:                                 /* RIF */
                                LAC = LAC | (IF >> 9);
                                break;

                            case 3:                                 /* RIB */
                                LAC = LAC | SF;
                                break;

                            case 4:                                 /* RMF */
                                UB = (SF & 0100) >> 6;
                                IB = (SF & 0070) << 9;
                                DF = (SF & 0007) << 12;
                                int_req = int_req & ~INT_NO_CIF_PENDING;
                                break;

                            case 5:                                 /* SINT */
                                if (int_req & INT_UF)
                                    PC = (PC + 1) & 07777;
                                break;

                            case 6:                                 /* CUF */
                                UB = 0;
                                int_req = int_req & ~INT_NO_CIF_PENDING;
                                break;

                            case 7:                                 /* SUF */
                                UB = 1;
                                int_req = int_req & ~INT_NO_CIF_PENDING;
                                break;
                                }                                   /* end switch device */
                            break;
            
                        default:
                            reason = stop_inst;
                            break;
                            }                                       /* end switch pulse */
                        break;                                      /* end case 20-27 */

                    case 010:                                       /* power fail */
                        switch (pulse) {                            /* decode IR<9:11> */

                        case 1:                                     /* SBE */
                            break;

                        case 2:                                     /* SPL */
                            if (int_req & INT_PWR)
                                PC = (PC + 1) & 07777;
                            break;

                        case 3:                                     /* CAL */
                            int_req = int_req & ~INT_PWR;
                            break;

                        default:
                            reason = stop_inst;
                            break;
                            }                                       /* end switch pulse */
                        break;                                      /* end case 10 */

                    default:                                        /* I/O device */
                        if (dev_tab[device]) {                      /* dev present? */
                            iot_data = dev_tab[device] (IR, iot_data);
                            LAC = (LAC & 010000) | (iot_data & 07777);
                            if (iot_data & IOT_SKP)
                                PC = (PC + 1) & 07777;
                            if (iot_data >= IOT_REASON)
                                reason = iot_data >> IOT_V_REASON;
                            }
                        else reason = stop_inst;                    /* stop on flag */
                        break;
                        }                                           /* end switch device */
                    break;
                    // end of case op_code 6: IOT
                case 7:
                    // Fetch state for OPRs
                    if (!(IR & 00400)) {
                        /* OPR group 1 */
                        if (IR & 0200)
                            LAC = LAC & 010000;                     /* CLA is sequence 1 */
                        if (IR & 0100)
                            LAC = LAC & 007777;                     /* CLL is sequence 1 */
                        if (IR & 0040)
                            LAC = LAC ^ 007777;                     /* CMA is sequence 2 */
                        if (IR & 0020)
                            LAC = LAC ^ 010000;                     /* CML is sequence 2 */
                        if (IR & 0001)
                            LAC = (LAC + 1) & 017777;               /* IAC is sequence 3 */
                        switch (IR & 00016) {                       /* rotates are sequence 4 */
                            case 0000:
                                break;
                            case 0002:                              /* BSW */
                                LAC = (LAC & 010000) | ((LAC >> 6) & 077) | ((LAC & 077) << 6);
                                break;
                            case 0004:                              /* RAL */
                                LAC = ((LAC << 1) | (LAC >> 12)) & 017777;
                                break;
                            case 0006:                              /* RTL */
                                LAC = ((LAC << 2) | (LAC >> 11)) & 017777;
                                break;
                            case 0010:                              /* RAR */
                                LAC = ((LAC >> 1) | (LAC << 12)) & 017777;
                                break;
                            case 0012:                              /* RTR */
                                LAC = ((LAC >> 2) | (LAC << 11)) & 017777;
                                break;
                            case 0014:                              /* RAL RAR - undef */
                                LAC = LAC & (IR | 010000);          /* uses AND path */
                                break;
                            case 0016:                              /* RTL RTR - undef */
                                LAC = (LAC & 010000) | (MA & 07600) | (IR & 0177); /* uses address path */
                                break;
                            }
                        }
                        /* end of OPR group 1 */
                    else if (IR & 00400 && !(IR & 00001)) {
                        /* OPR group 2 */
                        switch (IR & 00170) {
                            /* skips are sequence 1 */
                            case 0010:                               /* SKP */
                                PC = (PC + 1) & 07777;
                                break;
                            case 0020:                               /* SNL */
                                if (LAC >= 010000)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0030:                               /* SZL */
                                if (LAC < 010000)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0040:                               /* SZA */
                                if ((LAC & 07777) == 0)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0050:                               /* SNA */
                                if ((LAC & 07777) != 0 )
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0060:                               /* SZA SNL */
                                if ((LAC == 0) || (LAC >= 010000))
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0070:                               /* SNA SZL */
                                if ((LAC != 0) && (LAC < 010000))
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0100:                               /* SMA */
                                if ((LAC & 04000) != 0)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0110:                               /* SPA */
                                if ((LAC & 04000) == 0)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0120:                               /* SMA SNL */
                                if (LAC >= 04000)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0130:                               /* SPA SZL */
                                if (LAC < 04000)
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0140:                               /* SMA SZA */
                                if (((LAC & 04000) != 0) || ((LAC & 07777) == 0))
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0150:                               /* SPA SNA */
                                if (((LAC & 04000) == 0) && ((LAC & 07777) != 0))
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0160:                               /* SMA SZA SNL */
                                if ((LAC >= 04000) || (LAC == 0))
                                    PC = (PC + 1) & 07777;
                                break;
                            case 0170:                               /* SPA SNA SZL */
                                if ((LAC < 04000) && (LAC != 0))
                                    PC = (PC + 1) & 07777;
                                break;
                            } // end of switch (IR & 00176)
                            if (IR & 0200)
                                LAC = LAC & 010000;                 /* CLA is sequence 2 */
                            if (IR & 06) {                          /* HLT, OSR are sequence 3 */
                                if (UF) {                           /* user mode? */
                                    int_req = int_req | INT_UF;     /* request intr */
                                    tsc_ir = IR;                    /* save instruction */
                                    tsc_cdf = 0;                    /* clear flag */
                                    }
                                    else {
                                        if (IR & 02) {                   /* HLT */    
                                    reason = STOP_HALT;
                                }
                                    else {                           /* OSR */
                                        LAC = LAC | SR;
                                        }
                                    }
                                }
                            }
                        /* end of OPR group 2 */
                    else {
                        /* OPR group 3, standard

                               MQA!MQL exchanges AC and MQ, as follows:

                               temp = MQ;
                               MQ = LAC & 07777;
                               LAC = LAC & 010000 | temp;
                        */
                        temp = MQ;                                  /* group 3 */
                        if (IR & 0200)                              /* CLA */
                            LAC = LAC & 010000;
                        if (IR & 0020) {                            /* MQL */
                            MQ = LAC & 07777;
                            LAC = LAC & 010000;
                            }
                        if (IR & 0100)                              /* MQA */
                           LAC = LAC | temp;
                        if ((IR & 0056) && (cpu_unit.flags & UNIT_NOEAE)) {
                           reason = stop_inst;                      /* EAE not present */
                            }

/* xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

All EAE code below remains indented/formatted as in the original file as it fits better on the page

xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx */


/* OPR group 3 EAE

   The EAE operates in two modes:

        Mode A, PDP-8/I compatible
        Mode B, extended capability

   Mode B provides eight additional subfunctions; in addition, some
   of the Mode A functions operate differently in Mode B.

   The mode switch instructions are decoded explicitly and cannot be
   microprogrammed with other EAE functions (SWAB performs an MQL as
   part of standard group 3 decoding).  If mode switching is decoded,
   all other EAE timing is suppressed.
*/

        if (IR == 07431) {                              /* SWAB */
            emode = 1;                                  /* set mode flag */
            break;
            }
        if (IR == 07447) {                              /* SWBA */
            emode = gtf = 0;                            /* clear mode, gtf */
            break;
            }

/* If not switching modes, the EAE operation is determined by the mode
   and IR<6,8:10>:

   <6:10>       mode A          mode B          comments

   0x000        NOP             NOP
   0x001        SCL             ACS
   0x010        MUY             MUY             if mode B, next = address
   0x011        DVI             DVI             if mode B, next = address
   0x100        NMI             NMI             if mode B, clear AC if
                                                 result = 4000'0000
   0x101        SHL             SHL             if mode A, extra shift
   0x110        ASR             ASR             if mode A, extra shift
   0x111        LSR             LSR             if mode A, extra shift
   1x000        SCA             SCA
   1x001        SCA + SCL       DAD
   1x010        SCA + MUY       DST
   1x011        SCA + DVI       SWBA            NOP if not detected earlier
   1x100        SCA + NMI       DPSZ            
   1x101        SCA + SHL       DPIC            must be combined with MQA!MQL
   1x110        SCA + ASR       DCM             must be combined with MQA!MQL
   1x111        SCA + LSR       SAM

   EAE instructions which fetch memory operands use the CPU's DEFER
   state to read the first word; if the address operand is in locations
   x0010 - x0017, it is autoincremented.
*/

        if (emode == 0)                                 /* mode A? clr gtf */
            gtf = 0;
        switch ((IR >> 1) & 027) {                      /* decode IR<6,8:10> */

        case 020:                                       /* mode A, B: SCA */
            LAC = LAC | SC;
            break;
        case 000:                                       /* mode A, B: NOP */
            break;

        case 021:                                       /* mode B: DAD */
            if (emode) {
                MA = IF | PC;
                if ((MA & 07770) != 00010)              /* indirect; autoinc? */
                    MA = DF | M[MA];
                else MA = DF | (M[MA] = (M[MA] + 1) & 07777); /* incr before use */
                MQ = MQ + M[MA];
                MA = DF | ((MA + 1) & 07777);
                LAC = (LAC & 07777) + M[MA] + (MQ >> 12);
                MQ = MQ & 07777;
                PC = (PC + 1) & 07777;
                break;
                }
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 001:                                       /* mode B: ACS */
            if (emode) {
                SC = LAC & 037;
                LAC = LAC & 010000;
                }
            else {                                      /* mode A: SCL */
                SC = (~M[IF | PC]) & 037;
                PC = (PC + 1) & 07777;
                }
            break;

        case 022:                                       /* mode B: DST */
            if (emode) {
                MA = IF | PC;
                if ((MA & 07770) != 00010)              /* indirect; autoinc? */
                    MA = DF | M[MA];
                else MA = DF | (M[MA] = (M[MA] + 1) & 07777); /* incr before use */
                if (MEM_ADDR_OK (MA))
                    M[MA] = MQ & 07777;
                MA = DF | ((MA + 1) & 07777);
                if (MEM_ADDR_OK (MA))
                    M[MA] = LAC & 07777;
                PC = (PC + 1) & 07777;
                break;
                }
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 002:                                       /* MUY */
            MA = IF | PC;
            if (emode) {                                /* mode B: defer */
                if ((MA & 07770) != 00010)              /* indirect; autoinc? */
                    MA = DF | M[MA];
                else MA = DF | (M[MA] = (M[MA] + 1) & 07777); /* incr before use */
                }
            temp = (MQ * M[MA]) + (LAC & 07777);
            LAC = (temp >> 12) & 07777;
            MQ = temp & 07777;
            PC = (PC + 1) & 07777;
            SC = 014;                                   /* 12 shifts */
            break;

        case 023:                                       /* mode B: SWBA */
            if (emode)
                break;
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 003:                                       /* DVI */
            MA = IF | PC;
            if (emode) {                                /* mode B: defer */
                if ((MA & 07770) != 00010)              /* indirect; autoinc? */
                    MA = DF | M[MA];
                else MA = DF | (M[MA] = (M[MA] + 1) & 07777); /* incr before use */
                }
            if ((LAC & 07777) >= M[MA]) {               /* overflow? */
                LAC = LAC | 010000;                     /* set link */
                MQ = ((MQ << 1) + 1) & 07777;           /* rotate MQ */
                SC = 0;                                 /* no shifts */
                }
            else {
                temp = ((LAC & 07777) << 12) | MQ;
                MQ = temp / M[MA];
                LAC = temp % M[MA];
                SC = 015;                               /* 13 shifts */
                }
            PC = (PC + 1) & 07777;
            break;

        case 024:                                       /* mode B: DPSZ */
            if (emode) {
                if (((LAC | MQ) & 07777) == 0)
                    PC = (PC + 1) & 07777;
                break;
                }
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 004:                                       /* NMI */
            temp = (LAC << 12) | MQ;                    /* preserve link */
            for (SC = 0; ((temp & 017777777) != 0) &&
                (temp & 040000000) == ((temp << 1) & 040000000); SC++)
                temp = temp << 1;
            LAC = (temp >> 12) & 017777;
            MQ = temp & 07777;
            if (emode && ((LAC & 07777) == 04000) && (MQ == 0))
                LAC = LAC & 010000;                     /* clr if 4000'0000 */
            break;

        case 025:                                       /* mode B: DPIC */
            if (emode) {
                temp = (LAC + 1) & 07777;               /* SWP already done! */
                LAC = MQ + (temp == 0);
                MQ = temp;
                break;
                }
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 5:                                         /* SHL */
            SC = (M[IF | PC] & 037) + (emode ^ 1);      /* shift+1 if mode A */
            if (SC > 25)                                /* >25? result = 0 */
                temp = 0;
            else temp = ((LAC << 12) | MQ) << SC;       /* <=25? shift LAC:MQ */
            LAC = (temp >> 12) & 017777;
            MQ = temp & 07777;
            PC = (PC + 1) & 07777;
            SC = emode? 037: 0;                         /* SC = 0 if mode A */
            break;

        case 026:                                       /* mode B: DCM */
            if (emode) {
                temp = (-LAC) & 07777;                  /* SWP already done! */
                LAC = (MQ ^ 07777) + (temp == 0);
                MQ = temp;
                break;
                }
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 6:                                         /* ASR */
            SC = (M[IF | PC] & 037) + (emode ^ 1);      /* shift+1 if mode A */
            temp = ((LAC & 07777) << 12) | MQ;          /* sext from AC0 */
            if (LAC & 04000)
                temp = temp | ~037777777;
            if (emode && (SC != 0))
                gtf = (temp >> (SC - 1)) & 1;
            if (SC > 25)
                temp = (LAC & 04000)? -1: 0;
            else temp = temp >> SC;
            LAC = (temp >> 12) & 017777;
            MQ = temp & 07777;
            PC = (PC + 1) & 07777;
            SC = emode? 037: 0;                         /* SC = 0 if mode A */
            break;

        case 027:                                       /* mode B: SAM */
            if (emode) {
                temp = LAC & 07777;
                LAC = MQ + (temp ^ 07777) + 1;          /* L'AC = MQ - AC */
                gtf = (temp <= MQ) ^ ((temp ^ MQ) >> 11);
                break;
                }
            LAC = LAC | SC;                             /* mode A: SCA then */
        case 7:                                         /* LSR */
            SC = (M[IF | PC] & 037) + (emode ^ 1);      /* shift+1 if mode A */
            temp = ((LAC & 07777) << 12) | MQ;          /* clear link */
            if (emode && (SC != 0))
                gtf = (temp >> (SC - 1)) & 1;
            if (SC > 24)                                /* >24? result = 0 */
                temp = 0;
            else temp = temp >> SC;                     /* <=24? shift AC:MQ */
            LAC = (temp >> 12) & 07777;
            MQ = temp & 07777;
            PC = (PC + 1) & 07777;
            SC = emode? 037: 0;                         /* SC = 0 if mode A */
            break;

            }                                           /* end of OPR group 3 */

/* xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

All EAE code above remains indented/formatted as in the original file as it fits better on the page

xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx */

                        }
                        /* end of OPR group 3 */
                    break;
                    // end of case op_code 7

                } // end of switch (op_code)
                break;

            // end of case FETCH_state

        case DEFER_state:
            MA = IF | MA;                                   /* defer major state uses IF */
            MB = M[MA];
            if ((MA & 07770) == 00010)                      /* autoincrement needed? */
                M[MA] = ++MB & 07777;                       /* yes, do the autoincrement */
            MA =  MB;                                       /* get the target address */
            if (((IR >> 9) & 07) != 5)                      /* MRI or JMP? */
                next_Major_State = EXECUTE_state;           /* it's a MRI */
            else {
/* Opcode 5, JMP.  From Bernhard Baehr's description of the TSC8-75:

   (In user mode) the current JMP opcode is moved to the ERIOT register, the ECDF
   flag is cleared. The address of the JMP instruction is loaded into the ERTB
   register and the TSC8-75 I/O flag is raised. Then the JMP is performed as usual
   (including the setting of IF, UF and clearing the interrupt inhibit flag). */

                if (UF) {                                   /* it's a JMP, user mode? */
                    tsc_ir = IR;                            /* save instruction */
                    tsc_cdf = 0;                            /* clear flag */
                    if (tsc_enb) {                          /* TSC8 enabled? */
                        tsc_pc = (PC - 1) & 07777;          /* save PC */
                        int_req = int_req | INT_TSC;        /* request intr */
                    }
                }
                IF = IB;                                    /* change IF */
                UF = UB;                                    /* change UF */
                int_req = int_req | INT_NO_CIF_PENDING;     /* clr intr inhibit */
                PC = MA;
                next_Major_State = FETCH_state;
            }
            break;
            // end of case DEFER_state


        case EXECUTE_state:
            if (((IR >> 9) & 07) < 4) {                     /* AND .. DCA, or is it JMS? */
                if (IR & 00400)                             /* it is AND .. DCA, direct or indirect? */
                    MA = DF | (MA & 07777);                 /* indirect, use DF */
                else
                    MA = IF | (MA & 07777);                 /* direct, use IF */
                MB = M[MA];                                 /* get the data word */
                switch ((IR >> 9) & 07) {
                    case 0:                                 /* AND */
                        LAC = LAC & (MB | 010000);
                        break;
                    case 1:                                 /* TAD */
                        LAC = (LAC + MB) & 017777;
                        break;
                    case 2:                                 /* ISZ */
                        M[MA] = MB = (MB + 1) & 07777;
                        if (MB == 0)
                            PC = (PC + 1) & 07777;
                        break;
                    case 3:                                 /* DCA */
                        M[MA] = MB = LAC & 07777;
                        LAC = LAC & 010000;
                        break;
                    }  // end of switch ((IR >> 9) & 07)
                }
            else {

/* Opcode 4 JMS.  From Bernhard Baehr's description of the TSC8-75:

   (In user mode) the current JMS opcode is moved to the ERIOT register, the ECDF
   flag is cleared. The address of the JMS instruction is loaded into the ERTB
   register and the TSC8-75 I/O flag is raised. When the TSC8-75 is enabled, the
   target addess of the JMS is loaded into PC, but nothing else (loading of IF, UF,
   clearing the interrupt inhibit flag, storing of the return address in the first
   word of the subroutine) happens. When the TSC8-75 is disabled, the JMS is performed
   as usual. */


                if (UF) {                                   /* JMS, user mode? */
                    tsc_ir = IR;                            /* save instruction */
                    tsc_cdf = 0;                            /* clear flag */
                    }
                if (UF && tsc_enb) {                        /* user mode, TSC enab? */
                    tsc_pc = (PC - 1) & 07777;              /* save PC */
                    int_req = int_req | INT_TSC;            /* request intr */
                    }
                else {                                      /* normal JMS */
                    IF = IB;                                /* change IF */
                    UF = UB;                                /* change UF */
                    int_req = int_req | INT_NO_CIF_PENDING; /* clr intr inhibit */
                    MA = IF | (MA & 07777);
                    if (MEM_ADDR_OK (MA))
                        M[MA] = PC;                         /* write the return address */
                    }
                MB = MA & 07777;
                PC = (MA + 1) & 07777;                      /* set the PC to entry + 1 */
                }
            next_Major_State = FETCH_state;
            break;
            // end of case EXECUTE_state

        }  // end of switch (Major_State)

    // at the end of a complete instruction cycle (i.e., next major state is now Fetch)
    // check for an interrupt request and handle it if it occurred with ION
    if (next_Major_State == FETCH_state && int_req > INT_PENDING) {
        int_req = int_req & ~INT_ION;                   /* occurred, so interrupts off */
        SF = (UF << 6) | (IF >> 9) | (DF >> 12);        /* form save field */
        PCQ_ENTRY (IF | PC);                            /* save old PC with IF */
        IF = IB = DF = UF = UB = 0;                     /* clear mem ext */
        M[0] = PC;                                      /* save PC in 0 */
        PC = 1;                                         /* fetch next from 1 */
        }

    }                                                   /* end while (reason == 0) */

/* Simulation halted */

saved_PC = IF | (PC & 07777);                           /* save copies */
saved_MA = MA & 007777;
saved_IR = IR & 007777;
saved_Major_State = next_Major_State;
saved_DF = DF & 070000;
saved_LAC = LAC & 017777;
saved_MQ = MQ & 07777;
pcq_r->qptr = pcq_p;                                    /* update pc q ptr */
return reason;
}                                                       /* end sim_instr */

/*
 * This sequence of instructions is a mix that hopefully
 * represents a resonable instruction set that is a close 
 * estimate to the normal calibrated result.
 */

static const char *pdp8_clock_precalibrate_commands[] = {
    "106 100",
    "-m 100 MQL MQA",
    "-m 101 ISZ 112",
    "-m 102 JMP I 106",
    "-m 103 JMP I 106",
    "PC 100",
    NULL};

/* Reset routine */

t_stat cpu_reset (DEVICE *dptr)
{
saved_LAC = 0;
saved_Major_State = FETCH_state;
int_req = (int_req & ~INT_ION) | INT_NO_CIF_PENDING;
saved_DF = IB = saved_PC & 070000;
UF = UB = gtf = emode = 0;
pcq_r = find_reg ("PCQ", NULL, dptr);
if (pcq_r)
    pcq_r->qptr = 0;
else
    return SCPE_IERR;
sim_clock_precalibrate_commands = pdp8_clock_precalibrate_commands;
sim_vm_initial_ips = 10 * SIM_INITIAL_IPS;
sim_brk_types = SWMASK ('E') | SWMASK('I');
sim_brk_dflt = SWMASK ('E');
return SCPE_OK;
}

/* Set PC for boot (PC<14:12> will typically be 0) */

void cpu_set_bootpc (int32 PC)
{
saved_PC = PC;                                          /* set PC, IF */
saved_Major_State = FETCH_state;
saved_DF = IB = PC & 070000;                            /* set IB, DF */
return;
}

/* Memory examine */

t_stat cpu_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw)
{
if (addr >= MEMSIZE)
    return SCPE_NXM;
if (vptr != NULL)
    *vptr = M[addr] & 07777;
return SCPE_OK;
}

/* Memory deposit */

t_stat cpu_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw)
{
if (addr >= MEMSIZE)
    return SCPE_NXM;
M[addr] = val & 07777;
return SCPE_OK;
}

/* Memory size change */

t_stat cpu_set_size (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
int32 mc = 0;
uint32 i;

if ((val <= 0) || (val > MAXMEMSIZE) || ((val & 07777) != 0))
    return SCPE_ARG;
for (i = val; i < MEMSIZE; i++)
    mc = mc | M[i];
if ((mc != 0) && (!get_yn ("Really truncate memory [N]?", FALSE)))
    return SCPE_OK;
MEMSIZE = val;
for (i = MEMSIZE; i < MAXMEMSIZE; i++)
    M[i] = 0;
return SCPE_OK;
}

/* Change device number for a device */

t_stat set_dev (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
DEVICE *dptr;
DIB *dibp;
uint32 newdev;
t_stat r;

if (cptr == NULL)
    return SCPE_ARG;
if (uptr == NULL)
    return SCPE_IERR;
dptr = find_dev_from_unit (uptr);
if (dptr == NULL)
    return SCPE_IERR;
dibp = (DIB *) dptr->ctxt;
if (dibp == NULL)
    return SCPE_IERR;
newdev = get_uint (cptr, 8, DEV_MAX - 1, &r);           /* get new */
if ((r != SCPE_OK) || (newdev == dibp->dev))
    return r;
dibp->dev = newdev;                                     /* store */
return SCPE_OK;
}

/* Show device number for a device */

t_stat show_dev (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
DEVICE *dptr;
DIB *dibp;

if (uptr == NULL)
    return SCPE_IERR;
dptr = find_dev_from_unit (uptr);
if (dptr == NULL)
    return SCPE_IERR;
dibp = (DIB *) dptr->ctxt;
if (dibp == NULL)
    return SCPE_IERR;
fprintf (st, "devno=%02o", dibp->dev);
if (dibp->num > 1)
    fprintf (st, "-%2o", dibp->dev + dibp->num - 1);
return SCPE_OK;
}

/* CPU device handler - should never get here! */

int32 bad_dev (int32 IR, int32 AC)
{
return (SCPE_IERR << IOT_V_REASON) | AC;                /* broken! */
}

/* Build device dispatch table */

t_bool build_dev_tab (void)
{
DEVICE *dptr;
DIB *dibp;
uint32 i, j;
static const uint8 std_dev[] = {
    000, 010, 020, 021, 022, 023, 024, 025, 026, 027
    };

for (i = 0; i < DEV_MAX; i++)                           /* clr table */
    dev_tab[i] = NULL;
for (i = 0; i < ((uint32) sizeof (std_dev)); i++)       /* std entries */
    dev_tab[std_dev[i]] = &bad_dev;
for (i = 0; (dptr = sim_devices[i]) != NULL; i++) {     /* add devices */
    dibp = (DIB *) dptr->ctxt;                          /* get DIB */
    if (dibp && !(dptr->flags & DEV_DIS)) {             /* enabled? */
        if (dibp->dsp_tbl) {                            /* dispatch table? */
            DIB_DSP *dspp = dibp->dsp_tbl;              /* set ptr */
            for (j = 0; j < dibp->num; j++, dspp++) {   /* loop thru tbl */
                if (dspp->dsp) {                        /* any dispatch? */
                    if (dev_tab[dspp->dev]) {           /* already filled? */
                        sim_printf ("%s device number conflict at %02o\n",
                            sim_dname (dptr), dspp->dev);
                        return TRUE;
                        }
                    dev_tab[dspp->dev] = dspp->dsp;     /* fill */
                    }                                   /* end if dsp */
                }                                       /* end for j */
            }                                           /* end if dsp_tbl */
        else {                                          /* inline dispatches */
            for (j = 0; j < dibp->num; j++) {           /* loop thru disp */
                if (dibp->dsp[j]) {                     /* any dispatch? */
                    if (dev_tab[dibp->dev + j]) {       /* already filled? */
                        sim_printf ("%s device number conflict at %02o\n",
                            sim_dname (dptr), dibp->dev + j);
                        return TRUE;
                        }
                    dev_tab[dibp->dev + j] = dibp->dsp[j]; /* fill */
                    }                                   /* end if dsp */
                }                                       /* end for j */
            }                                           /* end else */
        }                                               /* end if enb */
    }                                                   /* end for i */
return FALSE;
}

/* Set history */

t_stat cpu_set_hist (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
int32 i, lnt;
t_stat r;

if (cptr == NULL) {
    for (i = 0; i < hst_lnt; i++)
        hst[i].pc = 0;
    hst_p = 0;
    return SCPE_OK;
    }
lnt = (int32) get_uint (cptr, 10, HIST_MAX, &r);
if ((r != SCPE_OK) || (lnt && (lnt < HIST_MIN)))
    return SCPE_ARG;
hst_p = 0;
if (hst_lnt) {
    free (hst);
    hst_lnt = 0;
    hst = NULL;
    }
if (lnt) {
    hst = (InstHistory *) calloc (lnt, sizeof (InstHistory));
    if (hst == NULL)
        return SCPE_MEM;
    hst_lnt = lnt;
    }
return SCPE_OK;
}

/* Show history */

t_stat cpu_show_hist (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
int32 l, k, di, lnt;
const char *cptr = (const char *) desc;
t_stat r;
InstHistory *h;

if (hst_lnt == 0)                                       /* enabled? */
    return SCPE_NOFNC;
if (cptr) {
    lnt = (int32) get_uint (cptr, 10, hst_lnt, &r);
    if ((r != SCPE_OK) || (lnt == 0))
        return SCPE_ARG;
    }
else lnt = hst_lnt;
di = hst_p - lnt;                                       /* work forward */
if (di < 0)
    di = di + hst_lnt;
fprintf (st, "PC     L AC    MQ    ea     IR\n\n");
for (k = 0; k < lnt; k++) {                             /* print specified */
    h = &hst[(++di) % hst_lnt];                         /* entry pointer */
    if (h->pc & HIST_PC) {                              /* instruction? */
        l = (h->lac >> 12) & 1;                         /* link */
        fprintf (st, "%05o  %o %04o  %04o  ", h->pc & ADDRMASK, l, h->lac & 07777, h->mq);
        if (h->ir < 06000)
            fprintf (st, "%05o  ", h->ea);
        else fprintf (st, "       ");
        sim_eval[0] = h->ir;
        if ((fprint_sym (st, h->pc & ADDRMASK, sim_eval, &cpu_unit, SWMASK ('M'))) > 0)
            fprintf (st, "(undefined) %04o", h->ir);
        if (h->ir < 04000)
            fprintf (st, "  [%04o]", h->opnd);
        fputc ('\n', st);                               /* end line */
        }                                               /* end else instruction */
    }                                                   /* end for */
return SCPE_OK;
}
