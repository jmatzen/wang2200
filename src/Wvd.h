#ifndef _INCLUDE_WVD_H_
#define _INCLUDE_WVD_H_

// wang virtual disk object
//
// it represents an interface to a .wvd (wang virtual disk) file,
// and offers some useful services, including caching the metadata,
// and is useful both for manipulaitng existing files as well as
// creating new ones.
//
// there are two modes of operation, but they start the same way, by creating
// a Wvd instance.   given this object, say wvd, call
//
//      wvd.create(disk_type, # platters, sectors-per-platter) to create
//          a new disk with the specified geometry.  subsequent calls
//          using the setters and getters can change these attributes,
//          as well as others.  this mode is used only by the DiskFactory
//          dialog, to create new virtual disk images.  when the disk is
//          configured as desired, calling wvd.save(filename) creates a
//          new virtual disk image, the image is formatted, and no more
//          changes are allowed on the fundamental metadata.
//
//      wvd.open(filename)
//          this opens an existing virtual disk image and reads and caches
//          disk metadata, including the disk type, the geometry, and the
//          write protect status.  this mechanism is used by the emulator
//          to access virtual disk images.  the disk type and geometry
//          can't be changed, but the write protect status and the label
//          can be modified with the setter functions.
//
//          individual sectors can be read and written, and the disk can be
//          formatted.  to make these file operations efficient, a hidden
//          file handle is kept open on the file, rather than reopening the
//          file for each sector access.  however, there is one complication
//          as a result of this.  a virtual disk may be inserted in a disk
//          drive at the time the disk factory wants to operate on the
//          disk -- for example, to change the write protect status, or to
//          reformat the drive.  a call to wvd.flush() causes the associated
//          filehandle to be closed, which allows the external routine to
//          modify the virtual disk image.  the next time wvd is operated on,
//          the filehandle will be reopened, and the disk metadata will be
//          read in again and cached, in case it has changed.
//
//          if some disk metadata has changed, namely the write protect status
//          or the label, wvd.save() must be called.  unlike the create() case,
//          no filename should be supplied, as there is already one associated
//          with wvd when open() was called.
//
//          once the virtual disk image is no longer needed, for example, when
//          the disk is ejected from the logical drive, wvd.close() must be
//          called.

#include <fstream>

#include "w2200.h"

class Wvd
{
public:
    CANT_ASSIGN_OR_COPY_CLASS(Wvd);

    // making a valid Wvd is a two step process.  create a container with the
    // default constructor, then call either open() or create() to fill it.
     Wvd() = default;  // must be followed by either open() or create()
    ~Wvd();

    // new blank disk with default values
    void create(int disk_type, int platters, int sectors_per_platter);
    // initialize from named file
    bool open(const std::string &filename);
    // forget about current file
    void close();

    // metadata setters/getters
    bool isModified() const noexcept;
    void setModified(bool modified=true) noexcept;

    std::string getPath() const;
    void setPath(const std::string &filename);

    // the emulator supports three types of timing for disks.
    // this is the encoding which appears in the metadata disktype
    // byte of the virtual disk image.
    enum disktype_t {
           DISKTYPE_FD5,        // 5.25" floppy
           DISKTYPE_FD8,        // 8" floppy
           DISKTYPE_HD60,       // 2260-style hard disk
           DISKTYPE_HD80,       // 2280-style hard disk
           DISKTYPE_ILLEGAL     // first illegal value
    };
    int  getDiskType();
    void setDiskType(int type);

    int  getNumPlatters();
    void setNumPlatters(int num);

    // sectors/platter
    int  getNumSectors();
    void setNumSectors(int num);

    bool getWriteProtect();
    void setWriteProtect(bool wp);

    std::string getLabel();
    void setLabel(const std::string &newlabel);

    // this saves modified state when open() was used to get it to begin with:
    void save();
    // create a new disk when create() was the original call:
    void save(const std::string &filename);

    // logical sector data access.
    // returns true on success, false on failure.
    // these only apply to real disk images, not those that
    // have been created but not yet saved.
    bool readSector (int platter, int sector, const uint8 *buffer);
    bool writeSector(int platter, int sector, const uint8 *buffer);

    // flush any pending write and close the filehandle,
    // but keep the association (unlike close())
    void flush();

    // format the given platter of the virtual disk image.
    // returns true if successful.
    bool format(int platter);

private:
    // make sure metadata is up to date
    void refreshMetadata() { if (m_metadata_stale && !!m_file) { reopen(); } }
    void reopen();

    // write 256 bytes to an absolute sector address
    bool rawWriteSector(int sector, const uint8 *data);

    // read 256 bytes from an absolute sector address
    bool rawReadSector(int sector, const uint8 *data);

    // write header block for wang virtual disk
    // return true on success
    bool writeHeader();

    // retrieve the metadata from the virtual disk image.
    // return true on success, otherwise complain why and return false.
    bool readHeader();

    // create a virtual disk file if it doesn't exist, erase it if does.
    // write the header and then format all platters.
    // returns true on success.
    bool createFile(const std::string &filename);

    // ----- data members -----
    std::unique_ptr<std::fstream> m_file;   // file handle
    bool          m_metadata_stale      = true;    // is the metadata possibly out of date?
    bool          m_metadata_modified   = false;   // metadata has been modified
    bool          m_has_path            = false;   // is m_path valid?
    std::string   m_path;                          // path to virtual disk
    std::string   m_label;                         // disk label
    disktype_t    m_disk_type           = DISKTYPE_ILLEGAL;  // disk type encoding
    int           m_num_platters        = 0;       // platters in the virtual disk image
    int           m_num_platter_sectors = 0;       // sectors per platter
    bool          m_write_protect       = false;   // true=don't write
};

#endif // _INCLUDE_WVD_H_

// vim: ts=8:et:sw=4:smarttab
