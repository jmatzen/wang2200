// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

#include "IoCardDisk.h"
#include "Ui.h"                 // emulator interface
#include "UiCrtFrame.h"
#include "UiCrtStatusBar.h"
#include "UiSystem.h"
#include "Wvd.h"                // used only for DISKTYPE_ definition
#include "host.h"
#include "system2200.h"

#ifdef __WXMSW__
#define USE_SBM_EVENTS 1        // track mouse events on staticBitmap
#else
// for some reason on OSX, the staticbitmap doesn't generate mouse events when
// clicked on.  instead, we capture mouse clicks on the statusbar, and manually
// check if they coincide with a static bitmap.
#define USE_SBM_EVENTS 0
#endif

// ----------------------------------------------------------------------------
// constants
// ----------------------------------------------------------------------------

// ID's for status bar buttons
enum {  ID_Keyword_Mode = 100,          // ID for status bar button

        // for the disk icon popup menu
        Disk_Popup_Insert,
        Disk_Popup_Eject,
        Disk_Popup_Inspect,
        Disk_Popup_Format,

        // first disk controller:
        ID_Button_DiskCtrl0_FDrive,     // primary fixed disk
        ID_Button_DiskCtrl0_RDrive,     // primary removable disk
        ID_Button_DiskCtrl0_FDrive2,    // secondary fixed disk
        ID_Button_DiskCtrl0_RDrive2,    // secondary removable disk
        // ... up to four drives per controller
    };

// ----------------------------------------------------------------------------
// resources
// ----------------------------------------------------------------------------

// icon set for disk images in the statusbar
#include "disk_icon0.xpm"
#include "disk_icon1.xpm"
#include "disk_icon2.xpm"
#include "disk_icon3.xpm"
#include "disk_icon4.xpm"
#include "disk_icon5.xpm"

// ----------------------------------------------------------------------------
// implementation
// ----------------------------------------------------------------------------

#define DISK_ICON_WIDTH  22     // in pixels
#define DISK_ICON_HEIGHT 13     // in pixels
#define DISK_ICON_GAP    1      // space between icons in pair, in pixels

CrtStatusBar::CrtStatusBar(CrtFrame *parent,
                           bool smart_term, bool primary_crt) :
        wxStatusBar(parent, -1),
        m_parent(parent)
{
    // max config: alpha/keyword button, status area, disk drives, dummy area
    int pane_widths[2+MAX_DISK_DRIVES+1] = {0};
    int pane_styles[2+MAX_DISK_DRIVES+1] = {0};

    m_icon_set[0] = wxIcon(disk_icon0_xpm);
    m_icon_set[1] = wxIcon(disk_icon1_xpm);
    m_icon_set[2] = wxIcon(disk_icon2_xpm);
    m_icon_set[3] = wxIcon(disk_icon3_xpm);
    m_icon_set[4] = wxIcon(disk_icon4_xpm);
    m_icon_set[5] = wxIcon(disk_icon5_xpm);

    // determine how many disk controllers there are
    if (primary_crt) {
        int slt;  // unused
        while (system2200::findDiskController(m_num_disk_controllers, &slt)) {
            m_num_disk_controllers++;
        }
    }

    // layout for the primary crt:
    //     keyboard button, text message, N disk controllers, dead space
    // layout for non-primary crts:
    //     keyboard button, text message, dead space
    int panes = 0;
#ifdef __WXMSW__
    pane_widths[panes] = 80;        // 80 pixels wide
#else
    // TODO: this shouldn't have to be hard-coded if I just did a little
    //       more work figuring out the button size w/maximum text
    pane_widths[panes] = 90;        // 90 pixels wide
#endif
    pane_styles[panes] = wxSB_NORMAL;
    panes++;

    pane_widths[panes] = -1;        // text area soaks up the rest
    pane_styles[panes] = wxSB_NORMAL;
    panes++;

#ifdef __WXMSW__
    const wxSize icon_size(DISK_ICON_WIDTH, DISK_ICON_HEIGHT);
#else
    const wxSize icon_size(DISK_ICON_WIDTH+2, DISK_ICON_HEIGHT);
#endif
    wxBitmap dummy(1, 1, 1);

    for (int ctrl=0; ctrl < m_num_disk_controllers; ctrl++) {

        // gather info about this controller
        int slot = 0;
        system2200::findDiskController(ctrl, &slot); // where is it plugged in?
        int io = 0;
        const bool ok = system2200::getSlotInfo(slot, nullptr, &io);  // address it is mapped to
        assert(ok);

        // figure out how many drives are attached to this controller
        m_num_drives[ctrl] = 0;
        for (int d=0; d < 4; d++) {
            const int stat = IoCardDisk::wvdDriveStatus(slot, d);
            if ((stat & IoCardDisk::WVD_STAT_DRIVE_EXISTENT) != 0) {
                m_num_drives[ctrl]++;
            } else {
                break;
            }
        }

        // make a label to indicate the base address associated with drives
        wxString label;
        label.Printf("3%02X:", io & 0xff);
        m_disk_label[2*ctrl+0] = new wxStaticText(this, -1, label);
        const wxSize label_size0  = m_disk_label[2*ctrl+0]->GetSize();
        const int    label_width0 = label_size0.GetWidth();

        label.Printf("3%02X:", (io + 0x40) & 0xff);
        m_disk_label[2*ctrl+1] = new wxStaticText(this, -1, label);
        const wxSize label_size1 = m_disk_label[2*ctrl+1]->GetSize();
        int label_width1 = label_size1.GetWidth();
        if (m_num_drives[ctrl] <= 2) {
            // not needed
            m_disk_label[2*ctrl+1]->Hide();
            label_width1 = 0;
        }

        // position of items within the pane
        m_disk_label_xoff[2*ctrl+0] = 2;
        m_disk_label_xoff[2*ctrl+1] = m_disk_label_xoff[2*ctrl+0]
                                    + label_width0
                                    + 2*(DISK_ICON_WIDTH+4*DISK_ICON_GAP);

        // create the icons for the drives
        for (int drive=0; drive < m_num_drives[ctrl]; drive++) {
            int idx = 4*ctrl + drive;
            m_disk_icon[idx] = new wxStaticBitmap(
                                    this,
                                    ID_Button_DiskCtrl0_FDrive + idx, // wxWindowID
                                    dummy,              // gets overridden later
                                    wxDefaultPosition,  // gets overridden later
                                    icon_size,
                                    0                   // draw flat -- not wxBU_AUTODRAW
                                );
            m_disk_state[idx] = -1;      // force update
            if (drive < 2) {
                m_disk_icon_xoff[idx] = m_disk_label_xoff[2*ctrl+0]
                                      + label_width0
                                      + DISK_ICON_GAP
                                      + ((drive == 1) ? (DISK_ICON_WIDTH+DISK_ICON_GAP) : 0);
            } else {
                m_disk_icon_xoff[idx] = m_disk_label_xoff[2*ctrl+1]
                                      + label_width1
                                      + DISK_ICON_GAP
                                      + ((drive == 3) ? (DISK_ICON_WIDTH+DISK_ICON_GAP) : 0);
            }
            SetDiskIcon(slot, drive);   // establish appropriate bitmap
#if USE_SBM_EVENTS
            // mouse event routing
            m_disk_icon[idx]->Bind(wxEVT_LEFT_DOWN,  &CrtStatusBar::OnMouseBtnDown, this);
            m_disk_icon[idx]->Bind(wxEVT_RIGHT_DOWN, &CrtStatusBar::OnMouseBtnDown, this);
#endif
        } // drive

        // allocate space for up to four drives, and one label per pair of drives
        pane_widths[panes] = m_disk_icon_xoff[4*ctrl + m_num_drives[ctrl]-1]
                           + DISK_ICON_WIDTH
                           + DISK_ICON_GAP;
        pane_styles[panes] = wxSB_NORMAL;
        panes++;
    } // for ctrl

    // create a dummy field to overlap the resize grip
    pane_widths[panes] = 20;
    pane_styles[panes] = wxSB_FLAT;
    panes++;

    SetFieldsCount (panes, &pane_widths[0]);
    SetStatusStyles(panes, &pane_styles[0]);

    // create checkbox to display/control whether shifted keys produce
    // keywords or cap characters.  the OnSize handler will size it and
    // move it into position later.  this member should be set last as
    // some methods test it to know if initialization is done yet.
    std::string label = (smart_term) ? "A/a" : "Keyword";
    m_keyword_ctl = new wxCheckBox(this, ID_Keyword_Mode, label);

    Show(true);

    SetMinHeight(DISK_ICON_HEIGHT); // triggers an OnSize()

    // event routing table
    Bind(wxEVT_SIZE,       &CrtStatusBar::OnSize,         this);
    Bind(wxEVT_CHECKBOX,   &CrtStatusBar::OnKeywordCtl,   this, ID_Keyword_Mode);
    Bind(wxEVT_LEFT_DOWN,  &CrtStatusBar::OnDiskButton,   this);
    Bind(wxEVT_RIGHT_DOWN, &CrtStatusBar::OnDiskButton,   this);
#if !USE_SBM_EVENTS
    Bind(wxEVT_LEFT_DOWN,  &CrtStatusBar::OnMouseBtnDown, this);
    Bind(wxEVT_RIGHT_DOWN, &CrtStatusBar::OnMouseBtnDown, this);
#endif
    Bind(wxEVT_MENU,       &CrtStatusBar::OnDiskPopup,    this, Disk_Popup_Insert);
    Bind(wxEVT_MENU,       &CrtStatusBar::OnDiskPopup,    this, Disk_Popup_Eject);
    Bind(wxEVT_MENU,       &CrtStatusBar::OnDiskPopup,    this, Disk_Popup_Inspect);
    Bind(wxEVT_MENU,       &CrtStatusBar::OnDiskPopup,    this, Disk_Popup_Format);
}


// set the appropriate icon for the disk, and refresh the associated tooltip
void
CrtStatusBar::SetDiskIcon(const int slot, const int drive)
{
    int io_addr;        // address of this slot
    bool ok = system2200::getSlotInfo(slot, nullptr, &io_addr);
    assert(ok);

    // figure out if disk is empty, idle, or running
    const int stat = IoCardDisk::wvdDriveStatus(slot, drive);
    const bool empty    = (stat & IoCardDisk::WVD_STAT_DRIVE_OCCUPIED) == 0;  // disk is not present
//  const bool running  = (stat & IoCardDisk::WVD_STAT_DRIVE_RUNNING)  != 0;  // motor is active
    const bool selected = (stat & IoCardDisk::WVD_STAT_DRIVE_SELECTED) != 0;  // unit is being addressed

    // figure out which disk controller this maps to
    int controller = 0;
    while (true) {
        int thisslot;
        ok = system2200::findDiskController(controller, &thisslot);
        assert(ok);
        if (thisslot == slot) {
            break;
        }
        controller++;
    }
    int idx = 4*controller+drive;
    const int mod_addr = io_addr + ((drive >= 2) ? 0x40 : 0x00);

    wxString tip;
    bool hard_disk = false;  // until proven otherwise
    const char drive_ch = (drive == 0) ? 'F' : 'R';
    if (!empty) {
        int disk_type;
        ok = IoCardDisk::wvdGetDiskType(slot, drive, &disk_type);
        assert(ok);
        hard_disk = (disk_type == Wvd::DISKTYPE_HD60)
                 || (disk_type == Wvd::DISKTYPE_HD80);
        std::string filename;
        ok = IoCardDisk::wvdGetFilename(slot, drive, &filename);
        assert(ok);
        // assign a tooltip
        tip.Printf("Click to eject drive %c/%03X:\n%s",
                    drive_ch, mod_addr, filename.c_str());
    } else {
        tip.Printf("Click to load drive %c /%03X", drive_ch, mod_addr);
    }

    wxString old_tip = m_disk_icon[idx]->GetToolTipText();
    if (tip != old_tip) {
        m_disk_icon[idx]->SetToolTip(tip);
    }

    // 0: hard disk,       1: selected hard disk
    // 2: occupied floppy, 3: selected occupied floppy
    // 4: empty floppy,    5: selected empty floppy
    const int state = empty     ? ((selected) ? 5 : 4)
                    : hard_disk ? ((selected) ? 1 : 0)
                                : ((selected) ? 3 : 2);

    // reassign and redraw the icon, but only if needed -- reduces flashing
    if (m_disk_state[idx] != state) {
        const wxIcon icon = m_icon_set[state];
        m_disk_icon[idx]->SetIcon(icon);
        m_disk_icon[idx]->Refresh();
        m_disk_state[idx] = state;
    }
}


// called when the window size changes
void
CrtStatusBar::OnSize(wxSizeEvent &event)
{
    if (m_keyword_ctl == nullptr) {
        return;         // not initialized yet
    }

    // controls must be relocated
    wxRect rect;
    GetFieldRect(0, rect);      // get size of status bar pane 0
#ifdef __WXMSW__
    m_keyword_ctl->Move(rect.x + 2, rect.y + 2);
#else
    m_keyword_ctl->Move(rect.x + 2, rect.y - 1);
#endif

    // move disk icons
    for (int ctrl=0; ctrl < m_num_disk_controllers; ctrl++) {

        wxRect status_rect;
        GetFieldRect(2 + ctrl, status_rect);
        const int y_off = (status_rect.height - DISK_ICON_HEIGHT) / 2;

        // move the labels for the primary and optional secondary drives
        m_disk_label[2*ctrl+0]->Move(
                status_rect.GetX() + m_disk_label_xoff[2*ctrl+0],
                status_rect.GetY() + y_off
            );
        if (m_num_drives[ctrl] > 2) {
            m_disk_label[2*ctrl+1]->Move(
                    status_rect.GetX() + m_disk_label_xoff[2*ctrl+1],
                    status_rect.GetY() + y_off
                );
        }

        // move the disk icons
        for (int drive=0; drive < m_num_drives[ctrl]; drive++) {
            m_disk_icon[4*ctrl+drive]->Move(
                    status_rect.GetX() + m_disk_icon_xoff[4*ctrl+drive],
                    status_rect.GetY() + y_off
                );
        }
    }

    event.Skip();       // let the rest of the processing happen
}


// called when the keyword checkbox in the status bar is pressed
void
CrtStatusBar::OnKeywordCtl(wxCommandEvent &event)
{
    if (event.GetId() == ID_Keyword_Mode) {
        // return focus to frame, otherwise the control will eat keyed input
        m_parent->refocus();
    } else {
        event.Skip();
    }
}


// called when the button in the status bar is pressed
void
CrtStatusBar::OnDiskButton(wxMouseEvent &event)
{
    const int diff = event.GetId() - ID_Button_DiskCtrl0_FDrive;
    if (diff < 0) {
        return; // clicked somewhere else on the status bar
    }

    const int controller = diff >> 2;
    const int drive      = diff & 3;
    assert((controller >= 0) && (controller < (MAX_DISK_DRIVES/2)));

    const bool left_click  = event.LeftDown();
    const bool right_click = event.RightDown();
    const bool cmd_down    = event.CmdDown(); // control key on PC, CMD key on Mac

    int slot;
    bool ok = system2200::findDiskController(controller, &slot);
    assert(ok);
    const int stat = IoCardDisk::wvdDriveStatus(slot, drive);
    const bool drive_occupied = (stat & IoCardDisk::WVD_STAT_DRIVE_OCCUPIED) != 0;

    // which behavior are we after
    m_popup_action = unknown;
           if (left_click  && !drive_occupied) {
        m_popup_action = insert_disk;
    } else if (left_click  &&  drive_occupied && !cmd_down) {
        m_popup_action = eject_disk;
    } else if (left_click  &&  drive_occupied &&  cmd_down) {
        m_popup_action = inspect_disk;
    } else if (right_click && !drive_occupied) {
        wxMenu menu;
        menu.Append(Disk_Popup_Insert, "&Insert disk", "insert a virtual disk");
        PopupMenu(&menu);
    } else if (right_click &&  drive_occupied) {
        wxMenu menu;
        menu.Append(Disk_Popup_Eject,   "&Eject",   "eject the current disk");
        menu.Append(Disk_Popup_Inspect, "&Inspect", "inspect the disk contents");
        menu.Append(Disk_Popup_Format,  "&Format",  "reformat the disk");
        PopupMenu(&menu);
    } else {
        assert(false);
        return;
    }

    // carry out the requested action
    switch (m_popup_action) {

        case insert_disk: {
            std::string full_path;
            if (host::fileReq(host::FILEREQ_DISK, "Disk to load", true, &full_path) ==
                              host::FILEREQ_OK) {
                int drive2, io_addr;
                const bool b = system2200::findDisk(full_path, nullptr, &drive2, &io_addr);
                const int eff_addr = io_addr + ((drive2 < 2) ? 0x00 : 0x40);
                if (b) {
                    UI_warn("Disk already in drive %c /%03x", "FRFR"[drive2], eff_addr);
                    return;
                }
                ok = IoCardDisk::wvdInsertDisk(slot, drive, full_path);
            } }
            break;

        case eject_disk:
            ok = IoCardDisk::wvdRemoveDisk(slot, drive);
            break;

        case inspect_disk: {
            std::string filename;
            ok = IoCardDisk::wvdGetFilename(slot, drive, &filename);
            assert(ok);
            m_parent->doInspect(filename);
            }
            break;

        case format_disk: {
            std::string filename;
            ok = IoCardDisk::wvdGetFilename(slot, drive, &filename);
            assert(ok);
            m_parent->doFormat(filename);
            }
            break;

        case unknown:
            return;

        default:
            assert(false);
            return;
    }

    if (!ok) {
        UI_error("Error: operation failed");
    }

    // return focus to frame, otherwise the control will eat keyed input
    m_parent->refocus();
}


void
CrtStatusBar::OnMouseBtnDown(wxMouseEvent &event)
{
#if !USE_SBM_EVENTS
    // on OSX, clicking on the wxStaticBitmaps used on the statusbar to represent
    // disk drives does not generate a mouse event for some reason.  instead, the
    // mouse event is generated by the statusbar, which we catch here and convert
    // to the ID which would have been generated by the static bitmap.
    wxPoint client_pos = event.GetPosition();  // "physical mouse position"
    wxPoint pos = ClientToScreen(client_pos);
    for (int ctrl=0; ctrl < m_num_disk_controllers; ctrl++) {
        for (int drive=0; drive < m_num_drives[ctrl]; drive++) {
            int idx = 4*ctrl + drive;
            wxRect rect = m_disk_icon[idx]->GetScreenRect();
            if (rect.Contains(pos)) {
                wxWindowID id = ID_Button_DiskCtrl0_FDrive + idx;
                event.SetId(id);
            }
        }
    }
#endif
    event.ResumePropagation(1);         // event was flagged to not propagate
    event.Skip();
}


void
CrtStatusBar::OnDiskPopup(wxCommandEvent &event)
{
    switch (event.GetId()) {
        case Disk_Popup_Insert:  m_popup_action = insert_disk;  break;
        case Disk_Popup_Eject:   m_popup_action = eject_disk;   break;
        case Disk_Popup_Inspect: m_popup_action = inspect_disk; break;
        case Disk_Popup_Format:  m_popup_action = format_disk;  break;
        default:
            assert(false);
            m_popup_action = unknown;
            break;
    }
}


void
CrtStatusBar::SetStatusMessage(const std::string &text)
{
    wxStatusBar::SetStatusText(text, 1);        // let superclass handle it
}


// keyword control
void
CrtStatusBar::setKeywordMode(bool state)
{
    // set the checkbox
    m_keyword_ctl->SetValue(state);

    // return focus to frame, otherwise the control will eat keyed input
    m_parent->refocus();
}

bool
CrtStatusBar::getKeywordMode() const
{
    return m_keyword_ctl->GetValue();
}

// when there is some significant event, this lets us know what changed
void
CrtStatusBar::diskEvent(int slot, int drive)
{
    SetDiskIcon(slot, drive);
}

// vim: ts=8:et:sw=4:smarttab
