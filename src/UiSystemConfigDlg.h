// This is a model dialog box for configuring the emulated computer system.
// It keeps track of which CPU type is installed, how much RAM, whether the
// cpu and disk are to be emulated in realtime or as fast as possible, and
// it keeps track of the per-I/O card configuration state.
//
// Actually, the state is held elsewhere, but this carries out the UI to
// allow establishing state, and making or reverting state changes.

#ifndef _INCLUDE_UI_SYSTEM_CONFIG_DLG_H_
#define _INCLUDE_UI_SYSTEM_CONFIG_DLG_H_

#include "SysCfgState.h"
#include "wx/wx.h"

class SystemConfigDlg : public wxDialog
{
public:
    CANT_ASSIGN_OR_COPY_CLASS(SystemConfigDlg);
    explicit SystemConfigDlg(wxFrame *parent);

private:
    // ---- event handlers ----
    void OnCpuChoice(wxCommandEvent& WXUNUSED(event));
    void OnMemsizeChoice(wxCommandEvent& WXUNUSED(event));
    void OnCardChoice(wxCommandEvent &event);
    void OnAddrChoice(wxCommandEvent &event);
    void OnButton(wxCommandEvent &event);
    void OnWarnIo(wxCommandEvent& WXUNUSED(event));

    wxChoice *m_cpu_type = nullptr;                 // B/T/VP/MVP/VLSI
    wxChoice *m_mem_size = nullptr;                 // main memory size
    wxChoice *m_card_desc[NUM_IOSLOTS] = {nullptr}; // io slot card types
    wxChoice *m_card_addr[NUM_IOSLOTS] = {nullptr}; // io slot addresses
    wxButton *m_card_cfg[NUM_IOSLOTS]  = {nullptr}; // per-card configuration

    // whether to want on access to non-existing IO devices the real Wang
    // hardware ignored such actions the emulator has support to catch such
    // actions, which is useful for debugging:
    wxCheckBox *m_warn_io = nullptr;

    wxButton *m_btn_revert = nullptr;
    wxButton *m_btn_ok     = nullptr;
    wxButton *m_btn_cancel = nullptr;

    // system configuration state
    SysCfgState m_cfg;          // the one being modified
    SysCfgState m_old_cfg;      // the existing configuration

    // return true if it is OK to commit the state as it is
    bool configStateOk(bool warn);

    // helper routines
    void setValidIoChoices(int slot, int card_type_idx);
    void updateDlg();
    void updateButtons();
    void setMemsizeStrings();

    // save/get dialog options to the config file
    void saveDefaults();
    void getDefaults();
};

#endif // _INCLUDE_UI_SYSTEM_CONFIG_DLG_H_

// vim: ts=8:et:sw=4:smarttab
