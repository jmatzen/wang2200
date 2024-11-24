// this is class interface for the 2200 CPUs

#ifndef _INCLUDE_CPU2200_H_
#define _INCLUDE_CPU2200_H_

#include "w2200.h"

class Scheduler;
class Timer;

// ============================= base class =============================
class Cpu2200
{
public:
    Cpu2200() = default;
    virtual ~Cpu2200() = default;

    // report which type of CPU is in use
    enum { CPUTYPE_2200B, CPUTYPE_2200T,
           CPUTYPE_VP, CPUTYPE_MVP, CPUTYPE_MVPC, CPUTYPE_MICROVP };
    virtual int getCpuType() const noexcept = 0;

    // true=hard reset, false=soft reset
    virtual void reset(bool hard_reset) noexcept = 0;

    // indicates if cpu is running or halted
    enum { CPU_RUNNING=0, CPU_HALTED=1 };
    int status() const noexcept { return m_status; }

    // the disk controller is odd in that it uses the AB bus to signal some
    // information after the card has been selected.  this lets it peek into
    // that part of the cpu state.
    virtual uint8 getAB() const noexcept = 0;

    // when a card is selected, or its status changes, it uses this function
    // to notify the core emulator about the new status.
    // I'm not sure what the names should be in general, but these are based
    // on what the keyboard uses them for.  data_avail shows up at
    // haltstep:  st3 bit 2 and is used to indicate the halt/step key is pressed.
    //   (perhaps this is always connected and doesn't depend on device selection)
    // ready: st3 bit 0 and is used to indicate the device is ready to accept a
    //        new command (and perhaps has data ready from an earlier command)
    virtual void setDevRdy(bool ready) noexcept = 0;

    // when a card gets an IOhdlr_getbyte and it decides to return the data
    // request, this function is called to return that data.  it also takes
    // care of the necessary low-level handshake emulation.
    virtual void ioCardCbIbs(int data) = 0;

    // run for ticks*100ns
    virtual int execOneOp() = 0;

    // this is a signal that in theory any card could use to set a
    // particular status flag in a cpu register, but the only role
    // I know it is used for is when the keyboard HALT key is pressed.
    virtual void halt() noexcept = 0;

protected:
    int m_status = CPU_HALTED;  // whether the cpu is running or halted

private:
};


// ============================= 2200T cpu =============================
class Cpu2200t: public Cpu2200
{
public:
    CANT_ASSIGN_OR_COPY_CLASS(Cpu2200t);
    // ---- see base class for description of these members: ----
    Cpu2200t(std::shared_ptr<Scheduler> scheduler,
             int ramsize, int subtype);
    ~Cpu2200t() override;
    int   getCpuType() const noexcept override;
    void  reset(bool hard_reset) noexcept override;
    uint8 getAB() const noexcept override;
    void  setDevRdy(bool ready) noexcept override;
    void  ioCardCbIbs(int data) override;
    int   execOneOp() override;  // simulate one instruction
    void  halt() noexcept override;

private:
    // ---- member functions ----

    // store the microcode word to the given microstore address
    void writeUcode(int addr, uint32 uop) noexcept;

    // dump the most important contents of the uP state
    void dumpState(bool full_dump);

    // dump a floating point number (16 nibbles)
    void dump16n(int addr);

    // read from the specified address
    uint8 readMem(uint16 addr) const noexcept;

    // write to the specified address.
    void writeMem(uint16 addr, uint4 wr_value, int write2) noexcept;

    // reading ST3 is a subroutine because it must return state that wasn't
    // what was written
    uint4 readSt3() const;

    // setting ST1.1 can have more complicated side effects
    void setSt1(uint4 value);

    // store the 4b value to the place selected by the C field.
    // return a flag if the op is illegal.
    void storeOperandC(uint32 uop, uint4 value);

    // ---- data members ----

    std::shared_ptr<Scheduler>  m_scheduler;  // shared system event scheduler
    const int                   m_cpu_type;   // type cpu flavor, eg CPUTYPE_2200T

    // these shouldn't have to be changed; they are just symbolic defines
    // to make the code more readable.
    static const int MAX_RAM   = 32768; // max # bytes of main memory
    static const int MAX_UCODE = 32768; // max # words in ucode store
    static const int MAX_KROM  = 2048;  // max # words in constant rom store

    static const int ICSTACK_SIZE = 16;                // 16 words in return stack
    static const int ICSTACK_TOP  = (ICSTACK_SIZE-1);  // index of top of stack
    static const int ICSTACK_MASK = 0xF;

    struct ucode_t {
        uint32 ucode;       // 19:0 stores raw ucode word
                            // 24:20 stores the repacked B field specifier
                            // 31:30 stores flags about required operands
        uint8  op;          // predecode: specific instruction
        uint8  p8;          // unused
        uint16 p16;         // predecode: instruction specific
    };

    ucode_t   m_ucode[MAX_UCODE]; // microcode store
    const int m_ucode_size;       // size of ucode store, in words

    uint8     m_kROM[MAX_KROM];   // constant/keyword ROM
    const int m_krom_size;        // size of kROM, in bytes

    // The micromachine uses A[15:0] as a nibble address.
    // We pack RAM[addr][3:0] = {WANGRAM[2*addr+1],WANGRAM[2*addr+0]}
    // That is, each byte of this RAM holds consecutive WANG RAM nibbles,
    // with the lower addressed nibble in the lsbs of the RAM byte.
    const int m_mem_size;       // size, in bytes
    uint8     m_ram[MAX_RAM];

    // this contains the CPU state
    struct cpu2200_t {
        uint16  pc;             // working address ("pc register")
        uint16  aux[16];        // PC scratchpad
        uint8   reg[8];         // eight 4b file registers
        uint16  ic;             // microcode instruction counter
        uint16  icstack[ICSTACK_SIZE];  // microcode subroutine stack
        int     icsp;           // icstack pointer
        uint8   c;              // data memory read register
        uint8   k;              // i/o data register
        uint8   ab;             // i/o address bus latch
        uint8   ab_sel;         // ab at time of last ABS
        uint8   st1;            // status reg 1 state
        uint8   st2;            // status reg 2 state
        uint8   st3;            // status reg 3 state
        uint8   st4;            // status reg 4 state
        bool    prev_sr;        // previous instruction was SR
    } m_cpu;

    // debugging feature
    bool m_dbg = false;
};


// ============================= 2200VP cpu =============================
class Cpu2200vp: public Cpu2200
{
public:
    CANT_ASSIGN_OR_COPY_CLASS(Cpu2200vp);
    // ---- see base class for description of these members: ----
    Cpu2200vp(std::shared_ptr<Scheduler> scheduler,
              int ramsize, int subtype);
    ~Cpu2200vp() override;
    int   getCpuType() const noexcept override;
    void  reset(bool hard_reset) noexcept override;
    uint8 getAB() const noexcept override;
    void  setDevRdy(bool ready) noexcept override;
    void  ioCardCbIbs(int data) override;
    int   execOneOp() override;  // simulate one instruction
    void  halt() noexcept override;

    // ---- class-specific members: ----

private:
    // ---- member functions ----
    // predecode uinstruction and write it to store
    void writeUcode(uint16 addr, uint32 uop, bool force=false) noexcept;

    // dump the most important contents of the uP state
    void dumpState(bool full_dump);

    void setSH(uint8 value);
    void setSL(uint8 value) noexcept;
    void setBSR(uint8 value) noexcept;
    void updateBankOffset() noexcept;

    // return the chosen bits of B and A, returns with the bits
    // of b in [7:4] and the bits of A in [3:0]
    uint8 getHbHa(int HbHa, int a_op, int b_op) const noexcept;

    // this callback occurs when the 30 ms timeslicing one-shot times out.
    void oneShot30msCallback() noexcept;

#ifdef HAVE_FILE_DUMP
    void dumpRam(const std::string &filename);
#endif

    static const int MAX_RAM   = 8192*1024; // max # bytes of main memory
    static const int MAX_UCODE =   64*1024; // max # words in ucode store
    static const int STACKSIZE = 96; // number of entries in the return stack

    // ---- data members ----

    const int m_cpu_subtype;
    const int m_mem_size;       // size, in bytes

    bool                        m_has_oneshot = false; // this cpu supports timeslicing
    std::shared_ptr<Scheduler>  m_scheduler;   // shared system timing scheduler object
    std::shared_ptr<Timer>      m_tmr_30ms;    // time slice 30 ms one shot

    struct ucode_t {
        uint32 ucode;       // raw ucode word (really 24b)
                            // upper 8b are used to hold flags
        uint8  op;          // predecode: specific instruction
        uint8  p8;          // predecode: instruction specific
        uint16 p16;         // predecode: instruction specific
    } m_ucode[MAX_UCODE];
    int m_ucode_words;      // number of implemented words

    // main memory
    uint8     m_ram[MAX_RAM];

    // this contains the CPU state
    struct cpu2200vp_t {
        uint16  pc;             // working address ("pc register")
        uint16  orig_pc;        // copy of pc at start of instruction (not always valid)
        uint16  aux[32];        // PC scratchpad
        uint8   reg[8];         // eight 8b file registers
        uint16  ic;             // microcode instruction counter
        uint16  icstack[STACKSIZE]; // microcode subroutine stack
        int     icsp;           // icstack pointer
        uint8   ch;             // high data memory read register
        uint8   cl;             // low data memory read register
        uint8   k;              // i/o data register
        uint8   ab;             // i/o address bus latch
        uint8   ab_sel;         // ab at time of last ABS
        uint8   sh;             // high status reg
        uint8   sl;             // low  status reg
        bool    bsr_mode;       // true=bsr register is active
        uint8   bsr;            // bank select register (microvp-2 feature)
        int     bank_offset;    // predecoded from sl
    } m_cpu;

    // debugging feature
    bool m_dbg = false;
};

// microcode disassembly utilities
bool dasmOneOp  (char *buff, uint16 ic, uint32 ucode) noexcept;
bool dasmOneVpOp(char *buff, uint16 ic, uint32 ucode) noexcept;

#endif // _INCLUDE_CPU2200_H_

// vim: ts=8:et:sw=4:smarttab
