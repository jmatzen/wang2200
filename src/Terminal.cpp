// This file implements the Terminal class.
// It models either a dumb display controller or a smart terminal.
// Dumb controller:
//    poking character bytes in the right location
//    moving the cursor
//    clearing the screen
// Smart terminal:
//    same as dumb controller, plus
//    maintaining character attributes
//    doing box drawing
//    doing character stream parsing and decompression
//    remapping keyboard encoding to match 2236
//    modeling uart delay and rate limiting
//
// TODO:
//   - remote printer isn't modeled, though most of the plumbing is in place
//   - in a real 2336 terminal, after flow control is initiated, the terminal
//     send a CRT-GO byte every three seconds or so while the rx buffer is
//     empty. I guess this is a robustness feature to even if the first CRT-GO
//     gets dropped a later one will get things going again.  that is less
//     important in the emulator where line loss is not a possibility.
//   - I did a little experimenting with a real 2336, but what should happen
//     if an illegal escape sequence is received? it isn't documented and
//     in my limited testing, really unexpected things can happen (like the
//     parser swallowing all subsequent input, including printable chars).

#include "IoCardKeyboard.h"
#include "IoCardTermMux.h"
#include "Scheduler.h"
#include "Terminal.h"
#include "Ui.h"
#include "host.h"              // for dbglog()
#include "system2200.h"

bool do_debug = false;

// ----------------------------------------------------------------------------
// Terminal
// ----------------------------------------------------------------------------

static char id_string[] = "*2236DE R2016 19200BPS 8+O (USA)";

Terminal::Terminal(std::shared_ptr<Scheduler> scheduler,
                   IoCardTermMux *muxd,
                   int io_addr, int term_num, ui_screen_t screen_type,
                   bool vp_cpu) :
    m_scheduler(scheduler),
    m_muxd(muxd),
    m_vp_cpu(vp_cpu),
    m_io_addr(io_addr),
    m_term_num(term_num)
{
    m_disp.screen_type = screen_type;
    m_disp.chars_w  = (screen_type == UI_SCREEN_64x16)  ? 64 : 80;
    m_disp.chars_h  = (screen_type == UI_SCREEN_64x16)  ? 16 : 24;
    m_disp.chars_h2 = (screen_type == UI_SCREEN_2236DE) ? 25 : m_disp.chars_h;

    reset(true);

    m_wndhnd = UI_displayInit(screen_type, m_io_addr, m_term_num, &m_disp);
    assert(m_wndhnd);

    const bool smart_term = (screen_type == UI_SCREEN_2236DE);
    if (smart_term) {
        // in dumb systems, the IoCardKeyboard will establish the callback
        // we use 0x01 as that corresponds to the vp-mode keyboard offset
        system2200::registerKb(
            m_io_addr+0x01, m_term_num,
            std::bind(&Terminal::receiveKeystroke, this, std::placeholders::_1)
        );

        // A real 2336 sends the sequence E4 F8 about a second after
        // it powers up (the second is to run self tests).
        m_init_tmr = m_scheduler->createTimer(
                       TIMER_MS(700),
                       std::bind(&Terminal::sendInitSeq, this)
                     );
    }
}


// free resources on destruction
Terminal::~Terminal()
{
    const bool smart_term = (m_disp.screen_type == UI_SCREEN_2236DE);
    if (smart_term) {
        system2200::unregisterKb(m_io_addr+0x01, m_term_num);
    }

    m_init_tmr    = nullptr;
    m_tx_tmr      = nullptr;
    m_crt_tmr     = nullptr;
    m_prt_tmr     = nullptr;
    m_selectp_tmr = nullptr;

    UI_displayDestroy(m_wndhnd.get());
}


// ----------------------------------------------------------------------------
// public member functions
// ----------------------------------------------------------------------------

// reset the crt
// hard_reset=true means
//     power on reset
// hard_reset=false means
//     user pressed the SHIFT-RESET sequence or
//     the terminal received a programmatic reset sequence
void
Terminal::reset(bool hard_reset)
{
    const bool smart_term = (m_disp.screen_type == UI_SCREEN_2236DE);

    // dumb CRT controllers don't independently get reset;
    // the host CPU tells it to clear.  smart terminals
    // will independently clear the screen even if the serial
    // line is disconnected.
    if (hard_reset || smart_term) {
        // reset command stream immediate mode parsing
        m_escape_seen = false;
        m_crt_sink    = true;

        resetCrt();
        resetPrt();
    }

    // smart terminals echo the ID string to the CRT at power on
    if (smart_term && hard_reset) {
        char *idptr = &id_string[1];  // skip the leading asterisk
        while (*idptr != '\0') {
            processCrtChar3(*idptr);
            idptr++;
        }
        processCrtChar3(0x0D);
        processCrtChar3(0x0A);
    }
}


// reset just the crt part of the terminal state
// if attr_only is true, a subset of the clearing happens.
void
Terminal::resetCrt(bool attr_only)
{
    // dumb/smart terminal state:
    m_disp.curs_x    = 0;
    m_disp.curs_y    = 0;
    m_disp.curs_attr = cursor_attr_t::CURSOR_ON;
    m_disp.dirty     = true;  // must regenerate display
    clearScreen();

    if (!attr_only) {
        // smart terminal state:
        m_raw_cnt    = 0;
        m_input_cnt  = 0;
        for (auto &byte : m_raw_buf)   { byte = 0x00; }  // not strictly necessary
        for (auto &byte : m_input_buf) { byte = 0x00; }  // not strictly necessary

        // we have to make an exception: if the script issues CLEAR,
        // the terminal is sent a "reset crt" sequence, but actually wiping
        // these out would break the script processing.
        if (!m_script_active) {
            m_tx_tmr = nullptr;
            m_kb_buff = {};
            m_kb_recent = {};
        }

        // crt buffer and associated flow control
        m_crt_buff       = {};
        m_crt_flow_state = flow_state_t::START;
        m_crt_tmr        = nullptr;
        m_selectp_tmr    = nullptr;
    }

    // on reset, the unit defaults to all attributes off, but the saved
    // "previous attributes" state, activated by a lone HEX(0E) is "bright".
    m_attrs      = char_attr_t::CHAR_ATTR_BRIGHT;
    m_attr_on    = false;
    m_attr_temp  = false;
    m_attr_under = false;
    m_box_bottom = false;

}


// reset just the prt part of the terminal state
void
Terminal::resetPrt()
{
    m_prt_buff       = {};
    m_prt_flow_state = flow_state_t::START;
    m_prt_tmr        = nullptr;
}


// ----------------------------------------------------------------------------
// terminal to mxd communication channel
// ----------------------------------------------------------------------------

// The 2336 sends an init sequence of E4 F8 on power up.  The emulator
// doesn't send E4 because it sometimes shows up as a spurious "INIT"
// keyword. It isn't clear what the E4 is for.  The F8 is the "crt go"
// flow control byte.  In a real system the CRT might be turned on before
// the 2200 CPU is, and so the MXD wouldn't see the init sequence.
void
Terminal::sendInitSeq()
{
    m_init_tmr = nullptr;
//  m_kb_buff.push(static_cast<uint8>(0xE4));
    m_kb_buff.push(static_cast<uint8>(0xF8));
    checkKbBuffer();
}


// the received keyval is appropriate for the first generation keyboards,
// but the smart terminals:
//   (a) couldn't send an IB9 (9th bit) per key, and
//   (b) certain keys were coded differently, including as a pair of bytes
// as smart terminals were connected via 19200 baud serial lines, we
// model the transport delay by firing a timer for that long which disallows
// sending any more keys for that long.  if any key arrives while the timer is
// active, store it in a fifo and send the next one when the timer expires.
void
Terminal::receiveKeystroke(int keycode)
{
    if (m_kb_buff.size() >= KB_BUFF_MAX) {
        UI_warn("the terminal keyboard buffer dropped a character");
        return;
    }

    // remap certain keycodes from first-generation encoding to what is
    // send over the serial line
    if (keycode == IoCardKeyboard::KEYCODE_RESET) {
        reset(false);  // clear screen, home cursor, and empty fifos
        m_kb_buff.push(static_cast<uint8>(0x12));
    } else if (keycode == IoCardKeyboard::KEYCODE_HALT) {
        // halt/step
        m_kb_buff.push(static_cast<uint8>(0x13));
    } else if (keycode == (IoCardKeyboard::KEYCODE_SF | IoCardKeyboard::KEYCODE_EDIT)) {
        // edit
        // Note: logging what comes out of my 2336, Shift-EDIT produces FD 50.
        //       I tried adding it but its purpose escapes me, so I removed it.
        m_kb_buff.push(static_cast<uint8>(0xBD));
    } else if ((keycode & IoCardKeyboard::KEYCODE_SF) != 0) {
        // special function
        m_kb_buff.push(static_cast<uint8>(0xFD));
        m_kb_buff.push(static_cast<uint8>(keycode & 0xff));
    } else if (keycode == 0xE6) {
        // the pc TAB key maps to "STMT" in 2200T mode,
        // but it maps to "FN" (function) in 2336 mode
        m_kb_buff.push(static_cast<uint8>(0xFD));
        m_kb_buff.push(static_cast<uint8>(0x7E));
    } else if (keycode == 0xE5) {
        // erase
        m_kb_buff.push(static_cast<uint8>(0xE5));
    } else if (0x80 <= keycode && keycode < 0xE5) {
        // it is an atom; add prefix
        m_kb_buff.push(static_cast<uint8>(0xFD));
        m_kb_buff.push(static_cast<uint8>(keycode & 0xff));
    } else {
        // the mapping is unchanged
        assert(keycode == (keycode & 0xff));
        m_kb_buff.push(static_cast<uint8>(keycode));
    }

    checkKbBuffer();
}


// process the next entry in kb event queue and schedule a timer to check
// for the next one
void
Terminal::checkKbBuffer()
{
    if (m_tx_tmr) {
        // serial channel is in use
        return;
    }

    if (m_kb_buff.empty() && (m_crt_flow_state != flow_state_t::STOP_PEND)
                          && (m_crt_flow_state != flow_state_t::GO_PEND)) {
        // nothing to do
        return;
    }

    // if a flow control byte is pending, it cuts to the head of the line
    uint8 byte = 0x00;
    if (m_crt_flow_state == flow_state_t::GO_PEND) {
        byte = 0xF8;
        m_crt_flow_state = flow_state_t::GOING;
    } else if (m_crt_flow_state == flow_state_t::STOP_PEND) {
        byte = 0xFA;
        m_crt_flow_state = flow_state_t::STOPPED;
    } else {
        byte = m_kb_buff.front();
        m_kb_buff.pop();
    }

    // this is a hacky heuristic, but the 2200 system didn't have any
    // flow control to prevent a terminal from overrunning the host.
    // that is, the term->mxd path is 2000 chars/sec, but the MXD cannot
    // process characters that fast and the system depended on humans not
    // using anywhere near the line rate other than in bursts.
    //
    // running at 1/2 the rate for normal chars loses some; running at 1/50
    // on carriage returns isn't enough time for whatever bookkeeping BASIC
    // does at the end of line.  so instead we run at 1/4 rate for normal
    // chars, and 1/100 rate for <CR> and hope that is enough.
    int64 delay = serial_char_delay;
    if (m_script_active) {
        if (m_vp_cpu) {
            delay *= ((byte == 0x0D) ? 100 : 4);
        } else {
            // this is good enough for entering programs into empty memory,
            // but if the input is merging with an existing program, causing
            // a lot of end-of-line processing, things break down.  maybe a
            // better fix is to not invoke the script polling here but instead
            // from inside the IoCardTermMux code -- it knows when the CPU
            // is waiting for a byte.  Here we could make the serial port
            // delay tiny and just feed the bytes on demand.  TODO: think
            delay *= ((byte == 0x0D) ? 150 : 7);
        }
    }
    // another complication: if two terminals are doing script processing
    // at the same time, it slows down the MXD response time, and we again
    // get overruns.
    const int active_scripts = system2200::numActiveScripts(m_io_addr+0x01);
    if (active_scripts > 1) {
        delay *= active_scripts;
    }

    // unfortunately, the "CLEAR" command (which starts all the scripts
    // distributed with wangemu) takes a long time to execute, and the
    // previous hackery is not enough. instead, an artificial delay is
    // added if we are in script mode and the most recently received
    // characters were "C L E A R <CR> <something>".  We have to test
    // when <something> is received because we want the delay to occur
    // after the <CR> is seen.
    //
    // if this doesn't work, there is another more grungy solution.  the
    // MXD emulator could peek at the 8080's memory and figure out how deep
    // the RX fifo is and somehow throttle this code from sending any
    // characters if it was in danger of overrunning.  that would be a big
    // and ugly hammer.
    m_kb_recent.push_back(byte);
    const int fifo_size = m_kb_recent.size();
    if (fifo_size > 7) {
        m_kb_recent.pop_front();
        if (m_script_active) {
            std::vector<uint8> k{m_kb_recent.begin(), m_kb_recent.end()}; // convert deque to vector
            if (   (k[0] == 'C')
                && (k[1] == 'L')
                && (k[2] == 'E')
                && (k[3] == 'A')
                && (k[4] == 'R')
                && (k[5] == 0x0D)) {
                delay = TIMER_MS(1000);
            }
        }
    }

    m_tx_tmr = m_scheduler->createTimer(
                   delay,
                   std::bind(&Terminal::termToMxdCallback, this, byte)
               );
}


// callback after a character has finished transmission
void
Terminal::termToMxdCallback(int key)
{
    m_tx_tmr = nullptr;
    m_muxd->receiveKeystroke(m_term_num, key);

    // poll for script input, but don't let it overrun the key buffer
    if (m_kb_buff.size() < 5) {
        m_script_active = system2200::pollScriptInput(m_io_addr+0x01, m_term_num);
    }

    // see if any other chars are pending
    checkKbBuffer();
}

// ----------------------------------------------------------------------------
// crt character processing
// ----------------------------------------------------------------------------

// advance the cursor in y
void
Terminal::adjustCursorY(int delta) noexcept
{
    m_disp.curs_y += delta;
    if (m_disp.curs_y >= m_disp.chars_h) {
        m_disp.curs_y = m_disp.chars_h-1;
        if (m_disp.screen_type != UI_SCREEN_2236DE) {
            m_disp.curs_x = 0;   // yes, scrolling has this effect
        }
        scrollScreen();
    } else if (m_disp.curs_y < 0) {
        m_disp.curs_y = m_disp.chars_h-1;     // wrap around
    }
}


// move cursor left or right
void
Terminal::adjustCursorX(int delta) noexcept
{
    m_disp.curs_x += delta;
    if (m_disp.curs_x >= m_disp.chars_w) {
        m_disp.curs_x = 0;
    } else if (m_disp.curs_x < 0) {
        m_disp.curs_x = m_disp.chars_w - 1;
    }
}


// clear the display; home the cursor
void
Terminal::clearScreen() noexcept
{
    for (auto &byte : m_disp.display) { byte = static_cast<uint8>(0x20); }
    for (auto &byte : m_disp.attr)    { byte = static_cast<uint8>(0x00); }
    setCursorX(0);
    setCursorY(0);
}


// scroll the contents of the screen up one row, and fill the new
// row with blanks.
void
Terminal::scrollScreen() noexcept
{
    uint8 *d  = &m_disp.display[0];  // first char of row 0
    uint8 *s  = d + m_disp.chars_w;  // first char of row 1
    uint8 *d2 = d + m_disp.chars_w*(m_disp.chars_h2-1);  // first char of last row

    // scroll up the data
    memcpy(d, s, (m_disp.chars_h2-1)*m_disp.chars_w);
    // blank the last line
    memset(d2, ' ', m_disp.chars_w);

    // cake care of the attribute plane
    if (m_disp.screen_type == UI_SCREEN_2236DE) {
        d  = &m_disp.attr[0];     // first char of row 0
        s  = d + m_disp.chars_w;  // first char of row 1
        d2 = d + m_disp.chars_w*(m_disp.chars_h2-1);  // first char of last row
        const uint8 attr_fill = 0;

        // scroll up the data
        memcpy(d, s, (m_disp.chars_h2-1)*m_disp.chars_w);
        // blank the last line
        memset(d2, attr_fill, m_disp.chars_w);
    }
}

// ----------------------------------------------------------------------------
// host to terminal byte stream routing
// ----------------------------------------------------------------------------
// some of the escape sequence information was gleaned from this document:
//     2536DWTerminalAndTerminalControllerProtocol.pdf
//
// For dumb terminals, the byte is simply sent on to the lowest level
// character handling, which interprets single byte control codes (eg,
// backspace, carriage return, home) and literal characters.
//
// The 2236DE has a multi-level command stream interpreter.  At the top
// level, here, there are a few "immediate" escape sequences which are not
// queued and are handled immediately.  All the other bytes are simply
// routed to either the crt or prt receive FIFO.

void
Terminal::processChar(uint8 byte)
{
    if (m_disp.screen_type != UI_SCREEN_2236DE) {
        // dumb display: no fifo, no command parsing, and no delay
        processCrtChar3(byte);
        return;
    }

    if (do_debug) {
        const char ch = (0x20 <= byte && byte <= 0x7E) ? byte : '.';
        dbglog("Terminal::processChar(0x%02x/%c), m_raw_cnt=%d, esc_seen=%d\n",
               byte, ch, m_raw_cnt, m_escape_seen ? 1 : 0);
    }

    auto handler = (m_crt_sink) ? std::mem_fn(&Terminal::crtCharFifo)
                                : std::mem_fn(&Terminal::prtCharFifo);

    // we potentially have to stack FB escapes in the case that an immediate
    // sequence (eg, FB F0 = route to CRT) comes in the middle of, say,
    // the compression sequence FB <FB F0> 68
    if (byte == 0xFB) {
        if (m_escape_seen) {
            // two escapes in a row -- forward the first to the current sink
            handler(this, static_cast<uint8>(0xFB));
        }
        m_escape_seen = true;
        return;
    }

    if (!m_escape_seen) {
        // not part of a possible immediate command: pass it through
        handler(this, byte);
        return;
    }

    // <FB> <something> has been received and has not yet been forwarded.
    // Sequences FBF0, FBF1, FBF2, FBF6 are immediate, meaning they are not
    // put in the input FIFO and take effect as soon as they are received.
    switch (byte) {

        case 0xF0: // route to crt (FB F0)
            m_crt_sink = true;
            break;

        case 0xF1: // route to prt (FB F1)
            m_crt_sink = false;
            break;

        case 0xF2: // restart terminal (FB F2)
            // TODO: study LDMOD#AB UARTRX5 routine
            reset(false);  // soft reset
#if 0
            // a real 2336 sends E4 then F8 (crt go flow control)
            // TODO: LDMOD#40 says of E4 "SET RSD FLAG IN MXD"
            // then F8 every three seconds while not throttled.
            // if I include this, the emulator thinks it got the INIT atom (E4)
            system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0xE4);
#endif
            system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0xF8);
            // TODO: then F8 every 3 seconds if not throttled
            break;

        case 0xF6: // reset crt (FB F6)
            // TODO: study LDMOD#AB UARTRX5 routine
            resetCrt();
            // a real 2336 sends E9 (crt stop flow control),
            // then F8 (crt go flow control), then another F8, then E4,
            // then F8 every three seconds while not throttled.
            system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0xF9);
            system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0xF8);
            system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0xF8);
#if 0
            // if I include this, the emulator thinks it got the INIT atom (E4)
            system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0xE4);
#endif
            // TODO: then F8 every 3 seconds if not throttled
            break;

        default:
            // pass through to current sink the pending escape and current char
            handler(this, static_cast<uint8>(0xFB));
            handler(this, byte);
            break;
    }

    m_escape_seen = false;
}

// ----------------------------------------------------------------------------
// crt byte stream parsing
// ----------------------------------------------------------------------------

// input queue for CRT byte stream
void
Terminal::crtCharFifo(uint8 byte)
{
    int size = m_crt_buff.size();
    if (size == CRT_BUFF_MAX) {
        UI_warn("Terminal 0x%02x, term#%d had crt fifo overflow",
                m_io_addr, m_term_num+1);
        return;  // we dropped the new one
    }

    m_crt_buff.push(byte);
    size++;
    if (size == 96 || size == 113) {
        m_crt_flow_state = flow_state_t::STOP_PEND;
        checkKbBuffer();
    }

    // process the new traffic
    checkCrtFifo();
}


// drain the crt byte fifo until it is empty or we hit a delay
void
Terminal::checkCrtFifo()
{
    while (!m_crt_buff.empty()) {
        if (m_selectp_tmr) {
            return;  // waiting on SELECT Pn timeout
        }
        const uint8 byte = m_crt_buff.front();
        m_crt_buff.pop();
        int size = m_crt_buff.size();
        if ((size == 30) && (m_crt_flow_state == flow_state_t::STOPPED)) {
            // send a GO if we drop below the threshold and we're stopped
            m_crt_flow_state = flow_state_t::GO_PEND;
            checkKbBuffer();
        }
        processCrtChar1(byte);
    }
}


// decode FB ... escape sequences (eg, decompress runs)
void
Terminal::processCrtChar1(uint8 byte)
{
    if ((m_raw_cnt == 0) && (byte == 0xFB)) {
        // start a FB ... sequence
        m_raw_buf[0] = 0xFB;
        m_raw_cnt = 1;
        return;
    }

    if (m_raw_cnt == 0) {
        // pass it through
        processCrtChar2(byte);
        return;
    }

    // keep accumulating command bytes
    assert((0 <= m_raw_cnt) && (static_cast<unsigned long>(m_raw_cnt) < sizeof(m_raw_buf)));
    m_raw_buf[m_raw_cnt++] = byte;

    // it is a character run sequence: FB nn cc
    // where nn is the repetition count, and cc is the character
    if (m_raw_cnt == 3) {
        if (do_debug) {
            const char ch = (0x20 <= m_raw_buf[2] && m_raw_buf[2] <= 0x7E) ? m_raw_buf[2] : '.';
            dbglog("Decompress run: cnt=%d, chr=0x%02x/%c\n", m_raw_buf[1], m_raw_buf[2], ch);
        }
        for (int i=0; i < m_raw_buf[1]; i++) {
            processCrtChar2(m_raw_buf[2]);
        }
        m_raw_cnt = 0;
        return;
    }

    // this must be the case we are considering at this point
    assert(m_raw_cnt == 2);

    // start of FB nn cc, where 0x01 <= count < 0x60, is a three byte sequence
    if (m_raw_buf[1] < 0x60) {
        return;
    }

    // FB nn, where nn >= 0x60 represents (nn-0x60) spaces in a row
    if ((0x60 <= m_raw_buf[1]) && (m_raw_buf[1] <= 0xBF)) {
        if (do_debug) {
            dbglog("Decompress spaces: cnt=%d\n", m_raw_buf[1]-0x60);
        }
        for (int i=0x60; i < m_raw_buf[1]; i++) {
            processCrtChar2(static_cast<uint8>(0x20));
        }
        m_raw_cnt = 0;
        return;
    }

    // check for delay sequence: FB Cn
    if ((0xC1 <= m_raw_buf[1]) && (m_raw_buf[1] <= 0xC9)) {
        const int delay_ms = 1000 * (m_raw_buf[1] - 0xC0) / 6;
        assert(m_selectp_tmr == nullptr);
        if (delay_ms > 0) {
//UI_info("Got FB Cn, delay=%d ms", delay_ms);
            m_selectp_tmr = m_scheduler->createTimer(
                                   TIMER_MS(delay_ms),
                                   std::bind(&Terminal::selectPCallback, this)
                                 );
            assert(m_selectp_tmr != nullptr);
        }
        if (do_debug) {
            dbglog("Delay sequence: cnt=%d\n", m_raw_buf[1]);
        }
        m_raw_cnt = 0;
        return;
    }

    // check for literal 0xFB byte: FB D0
    if (m_raw_buf[1] == 0xD0) {
        if (do_debug) {
            dbglog("Literal 0xFB byte\n");
        }
        processCrtChar2(static_cast<uint8>(0xFB));
        m_raw_cnt = 0;
        return;
    }

    if (m_raw_buf[1] == 0xF8) {
        m_raw_cnt = 0;
        return;
    }

    // the source code for the 2436 terminal, file LDMOD#40, picks off
    // the various escape codes and if the value is > 0xD0, it simply
    // checks the 0x04 bit to decide blink or no blink.
    if ((m_raw_buf[1] & 0x04) == 0x04) {
        // enable cursor blink (FB FC)
        // if the cursor was off to begin with, it remains off
//FIXME: rethink this conflation of cursor on/off/blinnk -- it seems weird
//C is encoding as 3 states, but likely hardware has 4 states, with
//two orthogonal control bits: cursor enabled/not, blink enabled/not.
        if (m_disp.curs_attr == cursor_attr_t::CURSOR_ON) {
            m_disp.curs_attr = cursor_attr_t::CURSOR_BLINK;
        }
    } else {
        // disable cursor blink (FB F8)
        // turning off blink does not re-enable the cursor
        if (m_disp.curs_attr == cursor_attr_t::CURSOR_BLINK) {
            m_disp.curs_attr = cursor_attr_t::CURSOR_ON;
        }
    }
    m_raw_cnt = 0;
    return;
}


// second level command stream interpretation, for 2236DE type
void
Terminal::processCrtChar2(uint8 byte)
{
    assert(m_disp.screen_type == UI_SCREEN_2236DE);
    if (do_debug) {
        dbglog("processCrtChar2(0x%02x), m_input_cnt=%d\n", byte, m_input_cnt);
    }

    assert(static_cast<unsigned long>(m_input_cnt) < sizeof(m_input_buf));

    // these values terminate FB 02 ... command sequences
    bool terminator = (byte == 0x0E || byte == 0x0F);

    // it isn't documented, but some sequences ignore any extra bytes after the
    // main sequence up to the terminator:
    //     0205...{0E|0F}
    //     020809...{0E|0F}
    //     020D0C03...{0E|0F}
    // while others ignore extra so long as the bytes are less than 0x20. If a
    // byte greater than 0x20 is seen, the current sequence is abandoned and
    // the violating character is processed.
    if (m_input_cnt == 0) {
        m_ignore = ignore_t::NONE;
    }
    if ((m_ignore == ignore_t::SOME) && (m_input_buf[1] >= 0x20)) {
        // break out of the escape sequence and process the new character
        m_input_cnt = 0;
    }
    if ((m_ignore == ignore_t::ALL) && !terminator) {
        return;  // ignore chars after the defined part of the sequence
    }

    if (m_input_cnt == 0) {
        switch (byte) {
        case 0x02:  // character attribute, draw/erase box
            m_input_buf[0] = byte;
            m_input_cnt = 1;
            return;
        case 0x0D:  // carriage return
            // this has the side effect of disabling attributes if in temp 0E mode
            m_attr_temp = false;
            processCrtChar3(0x0D);
            return;
        case 0x0E:  // enable attributes
            m_attr_on   = false;  // after 04 xx yy 0E, 0E changes to temp mode?
            m_attr_temp = true;
            return;
        case 0x0F:  // disable attributes
            m_attr_on   = false;
            m_attr_temp = false;
            return;
        default:
            // pass through
            processCrtChar3(byte);
            return;
        }
    }

    // accumulate this byte on the current command string
    m_input_buf[m_input_cnt++] = byte;

    // look for completed 02 ... command sequences
    assert(m_input_cnt > 0 && m_input_buf[0] == 0x02);

    // normal|alternate character set: 02 02 {00|02} ... {0E|0F}
    // if the terminator is early, treat missing bytes as if 0x00 had been received.
    if (m_input_cnt >= 3 && m_input_buf[1] == 0x02) {
        m_ignore = ignore_t::SOME;
        if (terminator) {
            if (m_input_cnt < 4) {
                m_input_buf[2] = 0x00;
            }
            if (m_input_buf[2] == 0x00) {
                m_attrs &= ~(char_attr_t::CHAR_ATTR_ALT);
            } else {
                m_attrs |= (char_attr_t::CHAR_ATTR_ALT);
            }
            m_input_cnt = 0;
        }
        return;
    }

    // defined attributes, possibly enabling them: 02 04 xx yy {0E|0F}
    // it is possible to get the terminator early, in which case treat the
    // missing bytes as if 0x00 had been received.
    if (m_input_cnt >= 4 && m_input_buf[1] == 0x04) {
        m_ignore = ignore_t::SOME;
        if (terminator) {
            if (m_input_cnt < 4) {
                m_input_buf[2] = 0x00;
            }
            if (m_input_cnt < 5) {
                m_input_buf[3] = 0x00;
            }
            if (   (m_input_buf[2] == 0x00 || m_input_buf[2] == 0x02 ||
                    m_input_buf[2] == 0x04 || m_input_buf[2] == 0x0B)
                && (m_input_buf[3] == 0x00 || m_input_buf[3] == 0x02 ||
                    m_input_buf[3] == 0x04 || m_input_buf[3] == 0x0B)) {
                m_attrs &= ~(char_attr_t::CHAR_ATTR_BRIGHT |
                             char_attr_t::CHAR_ATTR_BLINK  |
                             char_attr_t::CHAR_ATTR_INV    );
                m_attr_under = false;
                if (m_input_buf[2] == 0x02 || m_input_buf[2] == 0x0B) {
                    m_attrs |= (char_attr_t::CHAR_ATTR_BRIGHT);
                }
                if (m_input_buf[2] == 0x04 || m_input_buf[2] == 0x0B) {
                    m_attrs |= (char_attr_t::CHAR_ATTR_BLINK);
                }
                if (m_input_buf[3] == 0x02 || m_input_buf[3] == 0x0B) {
                    m_attrs |= (char_attr_t::CHAR_ATTR_INV);
                }
                if (m_input_buf[3] == 0x04 || m_input_buf[3] == 0x0B) {
                    m_attr_under = true;
                }
                m_attr_on   = (m_input_buf[4] == 0x0E);
                m_attr_temp = false;
                // UI_info("attrs: 02 04 %02x %02x %02x --> %02x, %d", m_input_buf[2], m_input_buf[3], m_input_buf[4], m_attrs, m_attr_under);
            }
            m_input_cnt = 0;
        }
        return;
    }

    // cursor blink enable: 02 05 ... {0E|0F}
    if (m_input_cnt >= 2 && m_input_buf[1] == 0x05) {
        m_ignore = ignore_t::ALL;
        if (terminator) {
            assert(m_input_cnt == 3);
            m_disp.curs_attr = cursor_attr_t::CURSOR_BLINK;
            m_input_cnt = 0;
        }
        return;
    }

    // return self-ID string: 02 08 09 ... 0F
    if (m_input_cnt >= 3 && m_input_buf[1] == 0x08) {
        m_ignore = ignore_t::ALL;
        if (terminator && (m_input_cnt == 4)) {
            if  (m_input_buf[2] == 0x09 && m_input_buf[3] == 0x0F) {
                char *idptr = &id_string[1];  // skip the leading asterisk
                while (*idptr != '\0') {
                    system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, *idptr);
                    idptr++;
                }
                system2200::dispatchKeystroke(m_io_addr+0x01, m_term_num, 0x0D);
            }
            m_input_cnt = 0;
        }
        return;
    }

    // draw/erase box mode: 02 0B {02|0B} ... {0E|0F}
    // keep the box mode prefix then erase the most recent box mode "verb"
    // after performing it, since the total box command string can be very
    // long, and there is no need to keep all of it.
    if (m_input_cnt == 3 && m_input_buf[1] == 0x0B) {
        if (m_input_buf[2] != 0x02 && m_input_buf[2] != 0x0B) {
            // must start either 02 0B 02, or 02 0B 0B
            m_ignore = ignore_t::SOME;
            return;
        }
        // this flag is used during 08 sub-commands
        // we draw the bottom only if we've seen a previous 0B command
        m_box_bottom = false;
        return;
    }
    if (m_input_cnt == 4 && m_input_buf[1] == 0x0B) {
        const bool box_draw = (m_input_buf[2] == 0x02);
        m_input_cnt--;  // drop current command byte
        switch (byte) {
            case 0x08: // move left
                // draw top left  side of char below old position
                // draw top right side of char below new position
                if (m_box_bottom) {
                    setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_LEFT, +1);
                }
                adjustCursorX(-1);
                if (m_box_bottom) {
                    setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_RIGHT, +1);
                }
                return;
            case 0x09: // move right
                // draw top right side at old position
                // draw top left  side at new position
                setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_RIGHT);
                adjustCursorX(+1);
                setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_LEFT);
                return;
            case 0x0A: // move down one line and draw vertical line
                adjustCursorY(+1);
                setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_VERT);
                return;
            case 0x0B: // draw vertical line at current position
                setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_VERT);
                m_box_bottom = true;  // subsequent 08 must draw bottom
                return;
            case 0x0C: // draw vertical line at current position
                adjustCursorY(-1);
                setBoxAttr(box_draw, char_attr_t::CHAR_ATTR_VERT);
                return;
            case 0x0E: // end of box mode
            case 0x0F: // end of box mode
                m_input_cnt = 0;
                return;
            default:
                // just ignore error and drop out of box mode
                m_ignore = ignore_t::SOME;
                return;
        }
    }

    // reinitialize terminal: 02 0D 0C 03 ... {0E|0F}
    // 2336DW_InteractiveTerminalUserManual.700-7636.11-82.pdf says this:
    //   1. Clears the screen, homes the cursor and turns the cursor on
    //   2. Selects normal intensity characters
    //   3. Selects bright as attribute to be activated by HEX(0E)
    //   4. Select the default character set for that version of terminal
    if (m_input_cnt >= 4) {
        m_ignore = ignore_t::ALL;
        if (terminator && (m_input_cnt == 5)) {
            if (   (m_input_buf[1] == 0x0D)
                && (m_input_buf[2] == 0x0C)
                && (m_input_buf[3] == 0x03)) {
                // the 2436 terminal source code (LDMOD#C0) does this:
                //     send HEX(03) -- clear screen
                //     send HEX(05) -- enable cursor
                //     jp RESETATR  -- reset crt attribute to default
                //                     it doesn't touch flow control state
                // resetCrt() takes a bool indicating if just this subset of processing
                // should be done.
                resetCrt(true);
            }
            m_input_cnt = 0;
        }
        return;
    }

    // check for illegal sequence prefixes
    if (    (m_input_cnt == 2)
         && (m_input_buf[1] != 0x02)
         && (m_input_buf[1] != 0x04)
         && (m_input_buf[1] != 0x05)
         && (m_input_buf[1] != 0x08)
         && (m_input_buf[1] != 0x0B)
         && (m_input_buf[1] != 0x0D)) {
        // treat it as a noop
        m_ignore = ignore_t::SOME;
        if (terminator) {
            m_input_cnt = 0;
        }
        return;
    }

    if (terminator) {
        // if we are here, a sequence terminated early, eg 02 0D 0F
        m_input_cnt = 0;
    }

    assert(m_input_cnt < 5);
}


// lowest level command stream interpretation
void
Terminal::processCrtChar3(uint8 byte)
{
    // true of old 64x16 display controller, which had only 7b buffer
    // byte &= 0x7F;

    switch (byte) {

        case 0x00: case 0x02: case 0x04:
        case 0x0B: case 0x0E: case 0x0F:
            // ignored
            break;

        case 0x01:      // home cursor
            setCursorX(0);
            setCursorY(0);
            break;

        case 0x03:      // clear screen
            clearScreen();
            break;

        case 0x05:      // enable cursor
            m_disp.curs_attr = cursor_attr_t::CURSOR_ON;
            break;

        case 0x06:      // disable cursor
            m_disp.curs_attr = cursor_attr_t::CURSOR_OFF;
            break;

        case 0x07:      // bell
            UI_displayDing(m_wndhnd.get());
            break;

        case 0x08:      // cursor left
            adjustCursorX(-1);
            break;

        case 0x09:      // horizontal tab
            adjustCursorX(+1);
            break;

        case 0x0A:      // linefeed (go down one column)
            adjustCursorY(+1);
            break;

        case 0x0C:      // reverse index (cursor up)
            adjustCursorY(-1);
            break;

        case 0x0D:      // go to first column
            setCursorX(0);
            break;

        default:        // just a character
            assert(byte >= 0x10);
#if 0
            // this is true for the old 64x16 display generator, not 80x24
            if (0x10 <= byte && byte <= 0x1F) {
                byte = byte + 0x40;     // 0x10 aliases into 0x50, etc.
            }
#endif
            const bool use_alt_char = (byte >= 0x80)
                                   && ((m_attrs & char_attr_t::CHAR_ATTR_ALT) != 0);

            const bool use_underline = ((byte >= 0x90) && !use_alt_char)
                                    || (m_attr_under && (m_attr_on || m_attr_temp));

            byte = (byte & 0x7F)
                 | ((use_underline) ? 0x80 : 0x00);

            screenWriteChar(m_disp.curs_x, m_disp.curs_y, byte);

            // update char attributes in screen buffer
            const int old = m_disp.attr[m_disp.chars_w*m_disp.curs_y + m_disp.curs_x]
                          & (char_attr_t::CHAR_ATTR_LEFT  |
                             char_attr_t::CHAR_ATTR_RIGHT |
                             char_attr_t::CHAR_ATTR_VERT  );

            int attr_mask = 0;
            if (!m_attr_on && !m_attr_temp) {
                attr_mask |= char_attr_t::CHAR_ATTR_BLINK
                          |  char_attr_t::CHAR_ATTR_BRIGHT
                          |  char_attr_t::CHAR_ATTR_INV;
            }
            if (!use_alt_char) {
                attr_mask |= char_attr_t::CHAR_ATTR_ALT;
            }

            screenWriteAttr(m_disp.curs_x, m_disp.curs_y,
                            static_cast<uint8>(old | (m_attrs & ~attr_mask)));
            adjustCursorX(+1);
            break;
    }

    m_disp.dirty = true;
}


// callback after SELECT Pn timer expires
void
Terminal::selectPCallback()
{
    m_selectp_tmr = nullptr;
    checkCrtFifo();
}

// ----------------------------------------------------------------------------
// printer receive parsing
// ----------------------------------------------------------------------------

// input queue for PRT byte stream
void
Terminal::prtCharFifo(uint8 /*byte*/)
{
    // TODO: not modeling remote printer yet
}

// vim: ts=8:et:sw=4:smarttab
