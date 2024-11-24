// this is a base class for the 80x24 and the 64x16 display controllers
// and implements all the real functionality of both.

#ifndef _INCLUDE_IOCARD_DISPLAY_H_
#define _INCLUDE_IOCARD_DISPLAY_H_

#include "IoCard.h"

class Cpu2200;
class Scheduler;
class Timer;
class Terminal;
enum ui_screen_t : int;

class IoCardDisplay : public IoCard
{
public:
    // see the base class for the definition of the public functions
    CANT_ASSIGN_OR_COPY_CLASS(IoCardDisplay);

    // ----- common IoCard functions -----
    IoCardDisplay(std::shared_ptr<Scheduler> scheduler,
                  std::shared_ptr<Cpu2200>   cpu,
                  int base_addr, int card_slot, ui_screen_t screen_type);
    ~IoCardDisplay() override;

    std::vector<int> getAddresses() const override;

    void  reset(bool hard_reset) override;
    void  select() override;
    void  deselect() override;
    void  strobeOBS(int val) override;
    void  strobeCBS(int val) override;
    int   getIB() const noexcept override;
    void  setCpuBusy(bool busy) override;

private:
    // ---- card properties ----
    std::string       getDescription() const override;
    std::string       getName() const override;
    std::vector<int>  getBaseAddresses() const override;

    std::shared_ptr<Scheduler> m_scheduler; // shared system event scheduler
    std::shared_ptr<Cpu2200>   m_cpu;       // associated CPU
    const int  m_base_addr;     // the address the card is mapped to
    const int  m_slot;          // which slot the card is plugged into
    const int  m_screen_type;   // display type

    bool       m_selected  = false;      // the card is currently selected
    bool       m_card_busy = false;      // the card is busy doing something

    std::unique_ptr<Terminal>  m_terminal;  // handle to display logic

    // model controller "busy" timing
    std::shared_ptr<Timer> m_tmr_hsync;  // horizontal sync timer
    int        m_hsync_count = 0;        // which horizontal line we are on
    enum class busy_state {
        IDLE,    // not busy
        CHAR,    // wait for next hsync then clear busy
        CLEAR1,  // wait for vsync, then advance to BUSY_CLEAR2
        CLEAR2,  // wait for vsync, then clear busy
        ROLL1,   // wait for hsync, then advance to BUSY_ROLL2
        ROLL2,   // wait for hsync, then clear busy
    } m_busy_state = busy_state::IDLE;

    void tcbHsync(int arg);     // timer callback
};

#endif // _INCLUDE_IOCARD_DISPLAY_H_

// vim: ts=8:et:sw=4:smarttab
