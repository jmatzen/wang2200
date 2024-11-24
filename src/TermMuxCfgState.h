// This class is derived from CardCfgState and holds some configuration state
// particular to this type of card.  For the terminal mux, currently there is
// only one bit of state: how many terminals are connected to it.
//
// TODO: Other possible configuration options (per terminal):
//    - link speed
//    - serial port instead of virtual terminal
//    - attached printer or not

#ifndef _INCLUDE_TERM_MUX_CFG_H_
#define _INCLUDE_TERM_MUX_CFG_H_

#include "CardCfgState.h"

class TermMuxCfgState : public CardCfgState
{
public:
    TermMuxCfgState() = default;
    TermMuxCfgState(const TermMuxCfgState &obj) noexcept;             // copy
    TermMuxCfgState &operator=(const TermMuxCfgState &rhs) noexcept;  // assign

    // ------------ common CardCfgState interface ------------
    // See CardCfgState.h for the use of these functions.

    void setDefaults() noexcept override;
    void loadIni(const std::string &subgroup) override;
    void saveIni(const std::string &subgroup) const override;
    bool operator==(const CardCfgState &rhs) const noexcept override;
    bool operator!=(const CardCfgState &rhs) const noexcept override;
    std::shared_ptr<CardCfgState> clone() const override;
    bool configOk(bool warn) const noexcept override;
    bool needsReboot(const CardCfgState &other) const noexcept override;

    // ------------ unique to TermMuxCfgState ------------

    // set/get the number of disk drives associated with the controller
    void setNumTerminals(int count) noexcept;
    int  getNumTerminals() const noexcept;

private:
    bool m_initialized = false;  // for debugging and sanity checking
    int  m_num_terms   = 0;      // number of terminasl connected to term mux
};

#endif // _INCLUDE_TERM_MUX_CFG_H_

// vim: ts=8:et:sw=4:smarttab
