// this file implements the PrinterFrame class.
// it is the window that represents an instance of a view of a Printer,
// and handles all the user interaction.

// AREAS FOR IMPROVEMENT (in here and Printer)
// in roughly decreasing order of usefulness & sanity
// -- have a printer icon on the CrtFrame next to where the disk icons go?
//    It should have a few different states indicating:
//       (1) printer log is empty
//       (2) printer has something, but it hasn't changed since last viewed
//       (3) printer has something new since the last time it was viewed
//    Even if you aren't using the print and clear stuff, I think this is
//    useful information.
// -- currently, each formfeed character is replaced with the right number
//    of blank lines to advance to the top of the next page.  if the page
//    length is changed, then things are mucked up.  also, if we save the
//    print log to a file, it might be best if those original pagebreaks
//    were preserved.
//    one solution would be to leave the pagebreak character in the log file.
//    when the page length gets changed, the dummy empty lines would be
//    removed and then new ones generated based on the new pagelength.  when
//    the log gets saved to a file, the dummy lines aren't printed out.
// -- configuration is currently hardwired to be a generic printer.
//    should we have a list of printers that can be selected, each with
//    different attributes?  or is that overkill?
// -- real printers have linefeed, formfeed, and on-/off-line buttons.
//    is there any need to duplicate that here?
// -- double-wide character mode isn't supported.
//    I suppose we'd just keep the control characters in the strings and
//    as each line is generated, we'd check for those control characters
//    and break the text up into runs of normal and wide characters.
//    normal runs would go as currently they do (although hard tabs would
//    have to take into account that wide chars count double).  then for
//    the runs of wide characters, we'd print each run into a small memory
//    buffer that would then get converted to a wxImage, then we'd use the
//    Rescale() function to double its width, then we'd paste that back
//    into the output DC at the right position.
// -- overstrike isn't supported.
//    all that is required is to interpret the HEX(08) characters as
//    appropriate, and also to distinguish a carriage return as just
//    resetting the output coordinate instead of also forcing a line feed
//    (research a bit to see who does the injection of the line feed)
// -- the timing of the printer isn't emulated.  is there any reason why
//    someone would want that duplicated?  it would have to go into
//    IoCardPrinter.cpp.


// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

#include "Ui.h"                 // emulator interface
#include "UiPrinter.h"
#include "UiPrinterConfigDlg.h"
#include "UiPrinterFrame.h"
#include "UiSystem.h"           // sharing info between UI_wxgui modules
#include "host.h"
#include "system2200.h"

#include "wx/paper.h"           // pick up paper database
                                // (for translation of enums - see paperSize)
#include "wx/printdlg.h"        // print preview interfaces

#include <iomanip>
#include <sstream>

#define USE_MY_PAPER 0

// ----------------------------------------------------------------------------
// constants
// ----------------------------------------------------------------------------

// IDs for the controls and the menu commands
enum
{
    // menu items
    File_Close = wxID_EXIT,
    File_SaveAs = wxID_SAVEAS,
    File_PrintClear = 1,
    File_PrintPreview,
    File_Print,
    File_PrintSetup,
    File_PageSetup,

    Display_FontSize8,
    Display_FontSize10,
    Display_FontSize12,
    Display_FontSize14,
    Display_FontSize18,

    Display_Greenbar,

    Configure_Dialog,
};

#if USE_MY_PAPER
// Map of enums for some of the paper sizes (wxPaperSize). Used for load/save
// to config file not all are implemented. See exPaperSize in defs.h for a
// complete list.
static constexpr struct papersizemap_t {
    wxPaperSize  papersize_val;
    const char  *papersize_name;
} papersizemap[] = {
    { wxPAPER_NONE,       "NONE" },         // Use specific dimensions
    { wxPAPER_LETTER,     "LETTER" },       // Letter, 8 1/2 by 11 inches
    { wxPAPER_LEGAL,      "LEGAL" },        // Legal, 8 1/2 by 14 inches
    { wxPAPER_A4,         "A4" },           // A4 Sheet, 210 by 297 millimeters
    { wxPAPER_A3,         "A3" },           // A3 sheet, 297 by 420 millimeters
    { wxPAPER_10X14,      "10X14" },        // 10-by-14-inch sheet
    { wxPAPER_11X17,      "11x17" },        // 11-by-17-inch sheet
    { wxPAPER_FANFOLD_US, "US Std Fanfold"} // US Std Fanfold, 14 7/8 by 11 inches
};
const int PSMAX = sizeof(papersizemap)/sizeof(papersizemap_t);
#endif

// map of enums for some of the paper bins (wxPrintBin). Used for load/save to config file
static constexpr struct paperbinmap_t {
    wxPrintBin  paperbin_val;
    const char *paperbin_name;
} paperbinmap[] = {
    { wxPRINTBIN_DEFAULT,        "DEFAULT" },
    { wxPRINTBIN_ONLYONE,        "ONLYONE" },
    { wxPRINTBIN_LOWER,          "LOWER" },
    { wxPRINTBIN_MIDDLE,         "MIDDLE" },
    { wxPRINTBIN_MANUAL,         "MANUAL" },
    { wxPRINTBIN_ENVELOPE,       "ENVELOPE" },
    { wxPRINTBIN_ENVMANUAL,      "ENVMANUAL" },
    { wxPRINTBIN_AUTO,           "AUTO" },
    { wxPRINTBIN_TRACTOR,        "TRACTOR" },
    { wxPRINTBIN_SMALLFMT,       "SMALLFMT" },
    { wxPRINTBIN_LARGEFMT,       "LARGEFMT" },
    { wxPRINTBIN_LARGECAPACITY,  "LARGECAPACITY" },
    { wxPRINTBIN_CASSETTE,       "CASSETTE" },
    { wxPRINTBIN_FORMSOURCE,     "FORMSOURCE" },
    { wxPRINTBIN_USER,           "USER" }
};

// ----------------------------------------------------------------------------
// PrinterFrame
// ----------------------------------------------------------------------------

// constructor
PrinterFrame::PrinterFrame(const wxString& title, const int io_addr) :
        wxFrame(static_cast<wxFrame *>(nullptr), -1, title,
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_FRAME_STYLE | wxNO_FULL_REPAINT_ON_RESIZE),
    m_printer_addr(io_addr)   // used later during configuration
{
#ifndef __WXMAC__
    // set the frame icon
    SetIcon(wxICON(wang));
#endif

    makeMenubar();      // create menubar
    makeStatusbar();    // create status bar

    // Printer support
    m_printData     = std::make_unique<wxPrintData>();
    m_pageSetupData = std::make_unique<wxPageSetupDialogData>();

    m_printer = new Printer(this);
    getDefaults();      // get configuration options, or supply defaults
    setMenuChecks();    // might need to disable some menu items

    setupRealPrinter();

    system2200::freezeEmu(false);

    // event routing table
    Bind(wxEVT_MENU,         &PrinterFrame::OnFileClose,       this, File_Close);
    Bind(wxEVT_MENU,         &PrinterFrame::OnFileSaveAs,      this, File_SaveAs);
    Bind(wxEVT_MENU,         &PrinterFrame::OnPrintClear,      this, File_PrintClear);
    Bind(wxEVT_MENU,         &PrinterFrame::OnPrintPreview,    this, File_PrintPreview);
    Bind(wxEVT_MENU,         &PrinterFrame::OnPrint,           this, File_Print);
    Bind(wxEVT_MENU,         &PrinterFrame::OnPrintSetup,      this, File_PrintSetup);
    Bind(wxEVT_MENU,         &PrinterFrame::OnPageSetup,       this, File_PageSetup);
    Bind(wxEVT_MENU,         &PrinterFrame::OnFontSize,        this, Display_FontSize8);
    Bind(wxEVT_MENU,         &PrinterFrame::OnFontSize,        this, Display_FontSize10);
    Bind(wxEVT_MENU,         &PrinterFrame::OnFontSize,        this, Display_FontSize12);
    Bind(wxEVT_MENU,         &PrinterFrame::OnFontSize,        this, Display_FontSize14);
    Bind(wxEVT_MENU,         &PrinterFrame::OnFontSize,        this, Display_FontSize18);
    Bind(wxEVT_MENU,         &PrinterFrame::OnDisplayGreenbar, this, Display_Greenbar);
    Bind(wxEVT_MENU,         &PrinterFrame::OnConfigureDialog, this, Configure_Dialog);
    Bind(wxEVT_MENU_OPEN,    &PrinterFrame::OnMenuOpen,        this);
    Bind(wxEVT_CLOSE_WINDOW, &PrinterFrame::OnClose,           this);
}


// destructor
PrinterFrame::~PrinterFrame()
{
    if (m_printer->getPrintasgo()) {
        printAndClear();        // print anything left in the printer
    }
    m_pageSetupData = nullptr;
    m_printData     = nullptr;
}


// create menubar
void
PrinterFrame::makeMenubar()
{
    wxMenu *menuFile = new wxMenu;
    menuFile->Append(File_Close,         "Close\tCtrl+W",   "Close the printer view");
    menuFile->AppendSeparator();
    menuFile->Append(File_SaveAs,        "Save As\tCtrl+S", "Save the contents of the printer to a file");
    menuFile->AppendSeparator();
    menuFile->Append(File_PrintClear,    "Clear printer",   "Clear the printer");
    menuFile->Append(File_PrintPreview,  "Print preview",   "Preview the contents of the printer");
    menuFile->Append(File_Print,         "Print\tCtrl+P",   "Print the contents of the printer");  // PHETODO alt-p?

    wxMenu *menuDisplay = new wxMenu;
    menuDisplay->Append(Display_FontSize8,  "Font Size  8", "Set display font to  8 point", wxITEM_CHECK);
    menuDisplay->Append(Display_FontSize10, "Font Size 10", "Set display font to 10 point", wxITEM_CHECK);
    menuDisplay->Append(Display_FontSize12, "Font Size 12", "Set display font to 12 point", wxITEM_CHECK);
    menuDisplay->Append(Display_FontSize14, "Font Size 14", "Set display font to 14 point", wxITEM_CHECK);
    menuDisplay->Append(Display_FontSize18, "Font Size 18", "Set display font to 18 point", wxITEM_CHECK);
    menuDisplay->AppendSeparator();
    menuDisplay->Append(Display_Greenbar,   "&Greenbar", "Make greenbar virtual paper", wxITEM_CHECK);

    wxMenu *menuConfig = new wxMenu;
    menuConfig->Append(Configure_Dialog, "&Emulated printer setup...", "Change settings of emulated Wang printer");
    menuConfig->Append(File_PageSetup,   "&Real printer setup...",     "Change settings of real printer");

    // make the help menu (as if it isn't obvious below!)
    wxMenu *menuHelp = TheApp::makeHelpMenu(this);

    // now append the freshly created menu to the menu bar...
    m_menubar = new wxMenuBar;
    m_menubar->Append(menuFile,    "&File");
    m_menubar->Append(menuDisplay, "&Display");
    m_menubar->Append(menuConfig,  "&Configure");
    m_menubar->Append(menuHelp,    "&Help");

    // ... and attach this menu bar to the frame
    SetMenuBar(m_menubar);
}


// create statusbar
void
PrinterFrame::makeStatusbar()
{
    m_statusbar = CreateStatusBar();
}


// set up various properties of the real printer
void
PrinterFrame::setupRealPrinter()
{
#if 0
    // trigger a load of default information
    m_pageSetupData->SetDefaultInfo(false);
#endif

    // now set up specifics

    // margins
    int left, right, top, bottom;
    m_printer->getMargins(left, right, top, bottom);
    const wxPoint point(left, top);
    const wxPoint point2(right, bottom);
    m_pageSetupData->SetMarginTopLeft(point);
    m_pageSetupData->SetMarginBottomRight(point2);

    // real printer name
    m_printData->SetPrinterName(m_printer->getRealPrinterName());

    // page orientation
    m_printData->SetOrientation(m_printer->getOrientation());

    // paper id
    m_printData->SetPaperId(m_printer->getPaperId());

    // paper bin
    m_printData->SetBin(m_printer->getBin());
}


// this is called just before a menu is displayed.
// set the check status for each of the menu items.
// also dynamically disables/enables menu items,
void
PrinterFrame::setMenuChecks()
{
    // ----- display -----------------------------------
    const bool greenbar = m_printer->getGreenbar();
    m_menubar->Check(Display_Greenbar, greenbar);

    m_menubar->Check(Display_FontSize8,  m_font_size ==  8);
    m_menubar->Check(Display_FontSize10, m_font_size == 10);
    m_menubar->Check(Display_FontSize12, m_font_size == 12);
    m_menubar->Check(Display_FontSize14, m_font_size == 14);
    m_menubar->Check(Display_FontSize18, m_font_size == 18);
}


// save Printer options to the config file
void
PrinterFrame::saveDefaults()
{
    std::ostringstream sg;
    sg << "ui/Printer-" << std::setw(2) << std::setfill('0') << std::hex << m_printer_addr;
    std::string subgroup(sg.str());

    int left, right, top, bottom;
    m_printer->getMargins(left, right, top, bottom);

    int plen, llen;
    m_printer->getPageAttributes(llen, plen);

    // save position and size
    host::configWriteWinGeom(this, subgroup);

    // save page attributes
    host::configWriteInt(subgroup,  "pagelength",       plen);
    host::configWriteInt(subgroup,  "linelength",       llen);
    host::configWriteInt(subgroup,  "fontsize",         m_font_size);
    host::configWriteBool(subgroup, "greenbar",         m_printer->getGreenbar());
    host::configWriteBool(subgroup, "autoshow",         m_printer->getAutoshow());
    host::configWriteBool(subgroup, "printasgo",        m_printer->getPrintasgo());
    host::configWriteBool(subgroup, "portdirect",       m_printer->getPortdirect());
    host::configWriteStr(subgroup,  "portstring",       m_printer->getPortstring());
    host::configWriteInt(subgroup,  "orientation",      static_cast<int>(m_printer->getOrientation()));
    host::configWriteStr(subgroup,  "papername",        m_printer->getPaperName());
    host::configWriteInt(subgroup,  "paperid",          m_printer->getPaperId());
    host::configWriteInt(subgroup,  "paperbin",         m_printer->getBin());
    host::configWriteInt(subgroup,  "marginleft",       left);
    host::configWriteInt(subgroup,  "marginright",      right);
    host::configWriteInt(subgroup,  "margintop",        top);
    host::configWriteInt(subgroup,  "marginbottom",     bottom);
    host::configWriteInt(subgroup,  "previewzoom",      m_preview_zoom);
    host::configWriteStr(subgroup,  "realprintername",  m_printer->getRealPrinterName());
}


// get Printer options from the config file, supplying reasonable defaults
void
PrinterFrame::getDefaults()
{
    std::string valstr;
    int v;
    bool b;

    // pick up screen color scheme
    std::ostringstream sg;
    sg << "ui/Printer-" << std::setw(2) << std::setfill('0') << std::hex << m_printer_addr;
    std::string subgroup(sg.str());

    // pick up screen location and size
    wxRect default_geom(50, 50, 690, 380);
    host::configReadWinGeom(this, subgroup, &default_geom);

    // pick up screen font size
    b = host::configReadInt(subgroup, "fontsize", &v);
    m_font_size = (b && (v >= 8) && (v <= 28)) ? v : 12;
    m_printer->setFontSize(m_font_size);

    host::configReadBool(subgroup, "greenbar", &b, true);
    m_printer->setGreenbar(b);

    // pick up page attributes
    int plen, llen;
    host::configReadInt(subgroup, "pagelength", &plen, 66);
    host::configReadInt(subgroup, "linelength", &llen, 80);
    m_printer->setPageAttributes(llen, plen);

    // pick up autoshow attribute
    host::configReadBool(subgroup, "autoshow", &b, true);
    m_printer->setAutoshow(b);

    // pick up printasgo attribute
    host::configReadBool(subgroup, "printasgo", &b, false);
    m_printer->setPrintasgo(b);

    // pick up portdirect attribute
    host::configReadBool(subgroup, "portdirect", &b, false);
    m_printer->setPortdirect(b);

    // pick up portstring attribute
    std::string portstring("LPT1");
    host::configReadStr(subgroup, "portstring", &portstring);
    m_printer->setPortstring(portstring);

    // pick up page margins
    int left, right, top, bottom;
    host::configReadInt(subgroup, "margintop",    &top,    50);
    host::configReadInt(subgroup, "marginbottom", &bottom, 50);
    host::configReadInt(subgroup, "marginleft",   &left,   50);
    host::configReadInt(subgroup, "marginright",  &right,  50);
    m_printer->setMargins(left, right, top, bottom);

    // pick up page preview zoom factor
    host::configReadInt(subgroup, "previewzoom", &m_preview_zoom, 70);

    // pick up orientation
    int orientation;
    host::configReadInt(subgroup, "orientation", &orientation, wxPORTRAIT);
    m_printer->setOrientation(static_cast<wxPrintOrientation>(orientation));

    // pick up paper id
    // we don't actually use the paperid that was saved to the config file but
    // instead we recalc the paperid from the papername. The papername is what
    // is important the paperid is an enum that might change at some point.
    wxPaperSize paperid = wxPAPER_NONE;
    b = host::configReadStr(subgroup, "papername", &valstr);
    if (b) {
        paperid = paperSize(valstr);
    }
    std::string papername(paperSize(paperid));
    if (papername.empty()) {
        // we did not find a match. use none
        paperid = wxPAPER_NONE;
        papername = paperSize(paperid);
    }
    m_printer->setPaperId(paperid);
    m_printer->setPaperName(papername);

    // pick up paper bin
    wxPrintBin paperbin = wxPRINTBIN_DEFAULT;
    b = host::configReadStr(subgroup, "paperbin", &valstr);
    if (b) {
        paperbin = paperBin(valstr);
    }
    m_printer->setBin(paperbin);

    // pick up printer name
    std::string printername;
    b = host::configReadStr(subgroup, "realprintername", &printername);
    m_printer->setRealPrinterName(printername);
}


// translate a character pagesize to the appropriate enum value for wxPaperSize
wxPaperSize
PrinterFrame::paperSize(const std::string &papersize_name)
{
    // translate char to wxPaperSize
#if USE_MY_PAPER
    for (int i=0; i < PSMAX; i++) {
        if (papersizemap[i].papersize_name == papersize_name) {
            return papersizemap[i].papersize_val;
        }
    }
    return wxPAPER_NONE;  // default
#else
    return wxThePrintPaperDatabase->ConvertNameToId(papersize_name);
#endif
}


// translate an enum pagesize to the appropriate string name
std::string
PrinterFrame::paperSize(wxPaperSize papersize_val) const
{
    // translate char to wxPaperSize
#if USE_MY_PAPER
    for (int i=0; i < PSMAX; i++) {
        if (papersizemap[i].papersize_val == papersize_val) {
            return papersizemap[i].papersize_name;
        }
    }
    return "LETTER"; // default
#else
    return std::string(wxThePrintPaperDatabase->ConvertIdToName(papersize_val));
#endif
}


// translate a character pagesize to the appropriate enum value for wxPaperSize
wxPrintBin
PrinterFrame::paperBin(const std::string &paperbin_name) const noexcept
{
    // translate char to wxPrintBin
    for (auto const &pb : paperbinmap) {
        if (pb.paperbin_name == paperbin_name) {
            return pb.paperbin_val;
        }
    }
    return wxPRINTBIN_DEFAULT;  // default
}


// translate an enum pagesize to the appropriate string name
std::string
PrinterFrame::paperBin(wxPrintBin paperbin_val) const
{
    // translate char to wxPaperSize
    for (auto const &pb : paperbinmap) {
        if (pb.paperbin_val == paperbin_val) {
            return pb.paperbin_name;
        }
    }

    return "DEFAULT"; // default
}

// ---- event handlers ----

// called when File/Close is selected
void
PrinterFrame::OnFileClose(wxCommandEvent& WXUNUSED(event))
{
    Show(false);  // hide the window for now
}


// save the contents of the virtual printer
void
PrinterFrame::OnFileSaveAs(wxCommandEvent& WXUNUSED(event))
{
    system2200::freezeEmu(true);
    m_printer->saveToFile();
    system2200::freezeEmu(false);
}


void
PrinterFrame::OnPrintClear(wxCommandEvent& WXUNUSED(event))
{
    system2200::freezeEmu(true);
    m_printer->printClear();
    system2200::freezeEmu(false);
}


// called when the window is manually closed ("X" button, or sys menu)
void
PrinterFrame::PP_OnClose(wxCloseEvent &event)
{
    const std::string subgroup("ui/printpreview");
    host::configWriteWinGeom(this, subgroup, false);

    wxPreviewControlBar *controlBar = m_preview->GetControlBar();
    assert(controlBar != nullptr);
    m_preview_zoom = controlBar->GetZoomControl();

    event.Skip();
}


void
PrinterFrame::OnPrintPreview(wxCommandEvent& WXUNUSED(event))
{
    // Pass two printout objects: for preview, and possible printing
    wxPrintDialogData printDialogData(*m_printData);
    // NB: wx takes care of reclaiming the preview object and associated objs
    wxPrintPreview *preview = new wxPrintPreview(
                                new Printout ("", m_printer),   // preview
                                new Printout ("", m_printer),   // printout
                                &printDialogData);
    if (!preview->IsOk()) {
        delete preview;
        preview = nullptr;
        wxMessageBox("There was a problem previewing.\n"
                     "Perhaps your current printer is not set correctly?",
                     "Previewing", wxOK);
        return;
    }

    std::string preview_title("Print Preview");
    // NB: wx framework takes care of reclaiming the frame object
    m_preview = new wxPreviewFrame(preview, this,
                                   preview_title,
                                   wxPoint(100, 100),  // default position
                                   wxSize(600, 650));  // default size

#if 1
    // intercept the close event so we can retrieve the preview zoom
    // factor, so it can be restored the next time.
    m_preview->Bind(wxEVT_CLOSE_WINDOW, &PrinterFrame::PP_OnClose, this);

    m_preview->Initialize();

    const std::string subgroup("ui/printpreview");
    wxRect default_geom(100, 100, 600, 650);
    host::configReadWinGeom(m_preview, subgroup, &default_geom, false);

    // it appears that these both have to be set -- setting one doesn't
    // automatically refresh the other
    m_preview->GetControlBar()->SetZoomControl(m_preview_zoom);
    preview->SetZoom(m_preview_zoom);
#else
    // FIXME: this is how the printing sample ends it -- study this to see
    //        what all the code above is doing.
    m_preview->Centre(wxBOTH);
    m_preview->InitializeWithModality(wxPreviewFrame_AppModal);
#endif

    m_preview->Show();
}


void
PrinterFrame::OnPrint(wxCommandEvent& WXUNUSED(event))
{
    system2200::freezeEmu(true);

    wxPrintDialogData printDialogData(*m_printData);
    wxPrinter printer(&printDialogData);

    Printout printout("", m_printer);
    if (!printer.Print(this, &printout, true)) {
        if (wxPrinter::GetLastError() == wxPRINTER_ERROR) {
            wxMessageBox("There was a problem printing.\n"
                         "Perhaps your current printer is not set correctly?",
                         "Printing", wxOK);
        } else {
            wxMessageBox("Printing canceled", "Printing", wxOK);
        }
    } else {
        (*m_printData) = printer.GetPrintDialogData().GetPrintData();
    }

    system2200::freezeEmu(false);
}


void
PrinterFrame::OnPrintSetup(wxCommandEvent& WXUNUSED(event))
{
    system2200::freezeEmu(true);

    wxPrintDialogData printDialogData(*m_printData);
    wxPrintDialog printerDialog(this, &printDialogData);

// SetSetupDialog(bool) deprecated since v2.5.4:
//  printerDialog.GetPrintDialogData().SetSetupDialog(true);
    printerDialog.ShowModal();

    (*m_printData) = printerDialog.GetPrintDialogData().GetPrintData();

    system2200::freezeEmu(false);
}


void
PrinterFrame::OnPageSetup(wxCommandEvent& WXUNUSED(event))
{
    (*m_pageSetupData) = *m_printData;
    wxPageSetupDialog pageSetupDialog(this, m_pageSetupData.get());
    pageSetupDialog.ShowModal();

    (*m_printData)     = pageSetupDialog.GetPageSetupData().GetPrintData();
    (*m_pageSetupData) = pageSetupDialog.GetPageSetupData();

    // (re)set margins
    wxPoint point, point2;
    point  = m_pageSetupData->GetMarginTopLeft();
    point2 = m_pageSetupData->GetMarginBottomRight();
    m_printer->setMargins(point.x, point2.x, point.y, point2.y);

    // (re)set page orientation
    m_printer->setOrientation(m_printData->GetOrientation());

    // (re)set paper id
    m_printer->setPaperId(m_printData->GetPaperId());

    // (re)set paper name
    std::string papername(paperSize(m_printer->getPaperId()));
    if (papername.empty()) {
        // we did not find a match. use letter, and issue warning
        m_printer->setPaperId(wxPAPER_NONE);
        papername = paperSize(wxPAPER_NONE);
        //UI_warn("Paper Type is not supported. Assuming None.");
    }
    m_printer->setPaperName(papername);

    // (re)set paper bin
    m_printer->setBin(m_printData->GetBin());

        // (re)set printer name
    m_printer->setRealPrinterName(std::string(m_printData->GetPrinterName()));

    // commit changes to config file
    saveDefaults();
}


void
PrinterFrame::OnFontSize(wxCommandEvent &event)
{
    int size = 12;
    switch (event.GetId()) {
        case Display_FontSize8:  size =  8; break;
        case Display_FontSize10: size = 10; break;
        case Display_FontSize12: size = 12; break;
        case Display_FontSize14: size = 14; break;
        case Display_FontSize18: size = 18; break;
        default:
            // the user must have edited the ini file and added a bogus
            // font size specifier.  default to something sane.
            size = 12;
            break;
    }
    m_font_size = size;
    m_printer->setFontSize(size);
}


void
PrinterFrame::OnDisplayGreenbar(wxCommandEvent& WXUNUSED(event))
{
    const bool greenbar = m_printer->getGreenbar();
    m_printer->setGreenbar(!greenbar);
}


// called when the window is manually closed ("X" button, or sys menu)
void
PrinterFrame::OnClose(wxCloseEvent& WXUNUSED(event))
{
    Show(false);  // simply hide the window
}


void
PrinterFrame::OnConfigureDialog(wxCommandEvent& WXUNUSED(event))
{
    // The validators defined in the dialog implementation bind controls
    // and variables together. Values are transferred between them behind
    // the scenes, so here we don't have to query the controls for their
    // values.
    system2200::freezeEmu(true);    // halt emulation

    PrinterDialogDataTransfer *data = new PrinterDialogDataTransfer();

    // set data values here
    int linelength, pagelength;
    m_printer->getPageAttributes(linelength, pagelength);

    data->m_line_length    = std::to_string(linelength);
    data->m_page_length    = std::to_string(pagelength);
    data->m_cb_auto_show   = m_printer->getAutoshow();
    data->m_cb_print_as_go = m_printer->getPrintasgo();
    data->m_cb_port_direct = m_printer->getPortdirect();
    data->m_port_string    = m_printer->getPortstring();

    PrinterConfigDlg dialog(this, "Printer Configuration", data);

    // When the dialog is displayed, validators automatically transfer
    // data from variables to their corresponding controls.
    if (dialog.ShowModal() == wxID_OK) {
        // 'OK' was pressed, so controls that have validators are
        // automatically transferred to the variables we specified
        // when we created the validators.
        long longllength = std::stol(std::string(data->m_line_length));
        long longplength = std::stol(std::string(data->m_page_length));

        m_printer->setPageAttributes(longllength, longplength);
        m_printer->setAutoshow(data->m_cb_auto_show);
        m_printer->setPrintasgo(data->m_cb_print_as_go);
        m_printer->setPortdirect(data->m_cb_port_direct);
        m_printer->setPortstring(std::string(data->m_port_string));

#if 0
        // PHE: clear the printer log because any previous printing
        // which used hex(0C) for a formfeed is no longer valid
        m_printer->PrintClear();
        // JTB: this is true, but this shortcoming is small compared
        //      to the inconvenience of having the print log wiped
        //      due to a configuration change.  this can be revisited...
#endif
    }

    delete data;

    system2200::freezeEmu(false);   // run emulation
}


void
PrinterFrame::OnMenuOpen(wxMenuEvent& WXUNUSED(event))
{
    setMenuChecks();
}


// other class functions ...

// emit a character to the display
void
PrinterFrame::printChar(uint8 byte)
{
    // send it to the Printer
    m_printer->printChar(byte);
}


// destroy a specific printer view
// system may defer destruction for a while until it is safe
void
PrinterFrame::destroyWindow()
{
    saveDefaults();
    Destroy();
}


// print the contents of the stream, then clear it (if successful)
void
PrinterFrame::printAndClear()
{
    if (m_printer->isEmpty()) {
        return;
    }

    system2200::freezeEmu(true);

    // remember where the focus was so we can restore it
    wxWindow *winHasFocus = FindFocus();
    assert(winHasFocus != nullptr);

    wxPrintDialogData printDialogData(*m_printData);
    printDialogData.SetToPage(m_printer->numberOfPages());
    wxPrinter printer(&printDialogData);

    Printout printout("", m_printer);
    if (!printer.Print(this, &printout, false)) {
        if (wxPrinter::GetLastError() == wxPRINTER_ERROR) {
            wxMessageBox("There was a problem printing.\n"
                        "Perhaps your current printer is not set correctly?",
                        "Printing", wxOK);
        } else {
            assert(false);    // this should never happen
        }
    } else {
        // printing was ok. now clear the stream.
        m_printer->printClear();
    }

    // restore the focus
    winHasFocus->SetFocus();
    winHasFocus = nullptr;

    system2200::freezeEmu(false);
}

// vim: ts=8:et:sw=4:smarttab
