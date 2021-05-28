/*
 * (c) Copyright 2021 by Coinkite Inc. This file is covered by license found in COPYING-CC.
 *
 * Implement a ram disk in PSRAM, accessible by host as MSC and mpy as block dev.
 *
 */
#include <stdint.h>

#include "usbd_cdc_msc_hid.h"
#include "usbd_msc_interface.h"
#include "usbd_cdc_msc_hid0.h"
#include "usbd_msc_bot.h"
#include "usbd_msc_scsi.h"
#include "usbd_ioreq.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"
#include "lib/oofatfs/ff.h"
#include "py/runtime.h"
#include "py/mperrno.h"

// Our storage, in quad-serial SPI PSRAM chip
static uint8_t *PSRAM_BASE = (uint8_t *)0x90000000;    // OCTOSPI mapping
static const uint32_t PSRAM_SIZE = 0x800000;                 // 8 megs
static const uint32_t BLOCK_SIZE = 512;
static const uint32_t BLOCK_COUNT = 16384;     // =PSRAM_SIZE / BLOCK_SIZE

STATIC mp_obj_t psram_wipe_and_setup(mp_obj_t unused_self);

// This flag is needed to support removal of the medium, so that the USB drive
// can be unmounted and won't be remounted automatically.
#define FLAGS_STARTED (0x01)

#define FLAGS_READONLY (0x02)

// psram_init()
//
    void
psram_init(void)
{
    // always clear and reset contents
    psram_wipe_and_setup(NULL);
}

static uint8_t *block_to_ptr(uint32_t blk, uint16_t num_blk)
{
    // Range checking on incoming requests also done in SCSI_CheckAddressRange()
    // but this is extra layer of safety, needed since we might expose
    // our address space otherwise.
    if(blk >= BLOCK_COUNT) return NULL;
    if(blk+(num_blk*BLOCK_SIZE) >= BLOCK_COUNT) return NULL;

    return &PSRAM_BASE[blk * BLOCK_SIZE];
}

STATIC const uint8_t psram_msc_lu_num = 1;
STATIC uint16_t psram_msc_lu_flags;

static inline void lu_flag_set(uint8_t lun, uint8_t flag) {
    psram_msc_lu_flags |= flag << (lun * 2);
}

static inline void lu_flag_clr(uint8_t lun, uint8_t flag) {
    psram_msc_lu_flags &= ~(flag << (lun * 2));
}

static inline bool lu_flag_is_set(uint8_t lun, uint8_t flag) {
    return psram_msc_lu_flags & (flag << (lun * 2));
}

// Sent in response to MODE SENSE(6) command
const uint8_t PSRAM_MSC_Mode_Sense6_Data[4] = {
    0x03, // mode data length
    0x00, // medium type
    0x00, // bit 7: write protect
    0x00, // block descriptor length
};

// Sent in response to MODE SENSE(10) command
const uint8_t PSRAM_MSC_Mode_Sense10_Data[8] = {
    0x00, 0x06, // mode data length
    0x00, // medium type
    0x00, // bit 7: write protect
    0x00,
    0x00,
    0x00, 0x00, // block descriptor length
};

STATIC const uint8_t psram_msc_vpd00[6] = {
    0x00, // peripheral qualifier; peripheral device type
    0x00, // page code
    0x00, // reserved
    2, // page length (additional bytes beyond this entry)
    0x00, // page 0x00 supported
    0x83, // page 0x83 supported
};

STATIC const uint8_t psram_msc_vpd83[4] = {
    0x00, // peripheral qualifier; peripheral device type
    0x83, // page code
    0x00, 0x00, // page length (additional bytes beyond this entry)
};

STATIC const int8_t psram_msc_inquiry_data[36] = {
    0x00, // peripheral qualifier; peripheral device type
    0x80, // 0x00 for a fixed drive, 0x80 for a removable drive
    0x02, // version
    0x02, // response data format
    (STANDARD_INQUIRY_DATA_LEN - 5), // additional length
    0x00, // various flags
    0x00, // various flags
    0x00, // various flags
    'C', 'o', 'i', 'n', 'k', 'i', 't', 'e', // Manufacturer : 8 bytes
    'C', 'O', 'L', 'D', 'C', 'A', 'R', 'D', // Product      : 16 Bytes
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    '4', '.', '0','0',                      // Version      : 4 Bytes
};

// Initialise all logical units (it's only ever called once, with lun_in=0)
STATIC int8_t psram_msc_Init(uint8_t lun_in) {
    if (lun_in != 0) {
        return 0;
    }
    lu_flag_set(0, FLAGS_STARTED);
    lu_flag_clr(0, FLAGS_READONLY);

    return 0;
}

// Process SCSI INQUIRY command for the logical unit
STATIC int psram_msc_Inquiry(uint8_t lun, const uint8_t *params, uint8_t *data_out) {
    if (params[1] & 1) {
        // EVPD set - return vital product data parameters
        uint8_t page_code = params[2];

        switch (page_code) {
            case 0x00: // Supported VPD pages
                memcpy(data_out, psram_msc_vpd00, sizeof(psram_msc_vpd00));
                return sizeof(psram_msc_vpd00);

            case 0x83: // Device identification
                memcpy(data_out, psram_msc_vpd83, sizeof(psram_msc_vpd83));
                return sizeof(psram_msc_vpd83);

            default: // Unsupported
                return -1;
        }
    }

    // A standard inquiry

    if (lun >= psram_msc_lu_num) {
        return -1;
    }

    uint8_t alloc_len = params[3] << 8 | params[4];
    int len = MIN(sizeof(psram_msc_inquiry_data), alloc_len);
    memcpy(data_out, psram_msc_inquiry_data, len);

    return len;
}

// Get storage capacity of a logical unit
STATIC int8_t psram_msc_GetCapacity(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
{
    *block_num = BLOCK_COUNT;
    *block_size = BLOCK_SIZE;

    return 0;
}

// Check if a logical unit is ready
STATIC int8_t psram_msc_IsReady(uint8_t lun) {
    if (lun >= psram_msc_lu_num) {
        return -1;
    }

    // NOTE: called frequently, and must be T for MacOS to recognize at all
    return lu_flag_is_set(lun, FLAGS_STARTED) ? 0 : -1;
}

// Check if a logical unit is write protected
STATIC int8_t psram_msc_IsWriteProtected(uint8_t lun) {
    if (lun >= psram_msc_lu_num) {
        return -1;
    }
    return lu_flag_is_set(lun, FLAGS_READONLY) ? 1 : 0;
}

// Start or stop a logical unit
STATIC int8_t psram_msc_StartStopUnit(uint8_t lun, uint8_t started) {
    if (lun >= psram_msc_lu_num) {
        return -1;
    }
    printf("PSRAMdisk: started=%d\n", started);
    if (started) {
        lu_flag_set(lun, FLAGS_STARTED);
    } else {
        lu_flag_clr(lun, FLAGS_STARTED);
    }
    return 0;
}

// Prepare a logical unit for possible removal
STATIC int8_t psram_msc_PreventAllowMediumRemoval(uint8_t lun, uint8_t param) {
    printf("PSRAMdisk: prevallow=%d\n", param);
    return 0;
}

// Read data from a logical unit
STATIC int8_t psram_msc_Read(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len) {
    if (lun >= psram_msc_lu_num) {
        return -1;
    }

    uint8_t *ptr = block_to_ptr(blk_addr, blk_len);
    if(!ptr) return -1;

    memcpy(buf, ptr, blk_len*BLOCK_SIZE);

    return 0;
}

// Write data to a logical unit
STATIC int8_t psram_msc_Write(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len) {
    if (lun >= psram_msc_lu_num) {
        return -1;
    }

    uint8_t *ptr = block_to_ptr(blk_addr, blk_len);
    if(!ptr) return -1;

    memcpy(ptr, buf, blk_len*BLOCK_SIZE);

    return 0;
}

// Get the number of attached logical units
STATIC int8_t psram_msc_GetMaxLun(void) {
    return psram_msc_lu_num - 1;
}


// Table of operations for the SCSI layer to call
USBD_StorageTypeDef psramdisk_fops = {
    psram_msc_Init,
    psram_msc_Inquiry,
    psram_msc_GetCapacity,
    psram_msc_IsReady,
    psram_msc_IsWriteProtected,
    psram_msc_StartStopUnit,
    psram_msc_PreventAllowMediumRemoval,
    psram_msc_Read,
    psram_msc_Write,
    psram_msc_GetMaxLun,
};

void psramdisk_USBD_MSC_RegisterStorage(int num_lun, usbd_cdc_msc_hid_state_t *usbd) {
    // equiv to usbdev/class/inc/usbd_cdc_msc_hid.h
    usbd->MSC_BOT_ClassData.bdev_ops = &psramdisk_fops;
    mp_printf(&mp_plat_print, "PSRAMdisk: activated\n");
}

//
// mpy user interface: os.AbstractBlockDev interface
//
// see <https://docs.micropython.org/en/latest/library/uos.html#simple-and-extended-interface>
//

typedef struct _psram_obj_t {
    mp_obj_base_t base;
    // nada?
} psram_obj_t;

// singleton
const mp_obj_type_t psram_type;
const psram_obj_t psram_obj = {
    { &psram_type },
};


STATIC void psram_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    mp_printf(print, "PSRAM()");
}

STATIC mp_obj_t psram_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    // Parse arguments: none allowed
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, 0, NULL, NULL);

    // singleton, we take no args
    return MP_OBJ_FROM_PTR(&psram_obj);
}

STATIC mp_obj_t psram_readblocks(size_t n_args, const mp_obj_t *args) {
    //psram_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t block_num = mp_obj_get_int(args[1]);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_WRITE);

    // Full range check; not supporting partial blocks nor offsets
    uint16_t blk_len = bufinfo.len / BLOCK_SIZE;
    if(blk_len < 1) goto fail;
    if((blk_len * BLOCK_SIZE) != bufinfo.len) goto fail;

    uint8_t *ptr = block_to_ptr(block_num, blk_len);
    if(!ptr) goto fail;

    memcpy(bufinfo.buf, ptr, bufinfo.len);

    return MP_OBJ_NEW_SMALL_INT(0);
fail:
    mp_raise_ValueError(NULL);
    return mp_const_none;                   // not reached
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(psram_readblocks_obj, 3, 3, psram_readblocks);

STATIC mp_obj_t psram_writeblocks(size_t n_args, const mp_obj_t *args) {
    //psram_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t block_num = mp_obj_get_int(args[1]);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);

    // Full range check; not supporting partial blocks nor offsets
    uint16_t blk_len = bufinfo.len / BLOCK_SIZE;
    if(blk_len < 1) goto fail;
    if((blk_len * BLOCK_SIZE) != bufinfo.len) goto fail;

    uint8_t *ptr = block_to_ptr(block_num, blk_len);
    if(!ptr) goto fail;

    memcpy(ptr, bufinfo.buf, bufinfo.len);

    return MP_OBJ_NEW_SMALL_INT(0);
fail:
    mp_raise_ValueError(NULL);
    return mp_const_none;                   // not reached
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(psram_writeblocks_obj, 3, 3, psram_writeblocks);

int direct_psram_read_blocks(uint8_t *dest, uint32_t block_num, uint32_t num_blocks) {
    // Return zero or -MP_EIO
    uint8_t *ptr = block_to_ptr(block_num, num_blocks);
    if(!ptr) return -MP_EIO;

    memcpy(dest, ptr, num_blocks * BLOCK_SIZE);

    return 0;
}
int direct_psram_write_blocks(const uint8_t *src, uint32_t block_num, uint32_t num_blocks) {
    // Return zero or -MP_EIO
    uint8_t *ptr = block_to_ptr(block_num, num_blocks);
    if(!ptr) return -MP_EIO;

    memcpy(ptr, src, num_blocks * BLOCK_SIZE);

    return 0;
}

STATIC mp_obj_t psram_ioctl(mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in) {
    //psram_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t cmd = mp_obj_get_int(cmd_in);

    switch (cmd) {
        case MP_BLOCKDEV_IOCTL_INIT:
        case MP_BLOCKDEV_IOCTL_DEINIT:
        case MP_BLOCKDEV_IOCTL_SYNC:
            // nothing to do
            return MP_OBJ_NEW_SMALL_INT(0);

        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            return MP_OBJ_NEW_SMALL_INT(BLOCK_COUNT);

        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            return MP_OBJ_NEW_SMALL_INT(BLOCK_SIZE);

        case MP_BLOCKDEV_IOCTL_BLOCK_ERASE: {
            mp_int_t block_num = mp_obj_get_int(arg_in);

            uint8_t *ptr = block_to_ptr(block_num, 1);
            if(!ptr) return mp_const_none;

            memset(ptr, 0xff, BLOCK_SIZE);

            return MP_OBJ_NEW_SMALL_INT(0);
        }

        default:
            return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(psram_ioctl_obj, psram_ioctl);

static void psram_init_vfs(fs_user_mount_t *vfs) {
    // Simulates mounting the block device into VFS system. Assumes FAT format.
    vfs->base.type = &mp_fat_vfs_type;
    vfs->blockdev.flags |= MP_BLOCKDEV_FLAG_NATIVE | MP_BLOCKDEV_FLAG_HAVE_IOCTL;

    vfs->fatfs.drv = vfs;
    vfs->fatfs.part = 0; // no partions; we have no MBR, like a floppy
    vfs->blockdev.readblocks[0] = MP_OBJ_FROM_PTR(&psram_readblocks_obj);
    vfs->blockdev.readblocks[1] = MP_OBJ_FROM_PTR(&psram_obj);
    vfs->blockdev.readblocks[2] = MP_OBJ_FROM_PTR(direct_psram_read_blocks);
    vfs->blockdev.writeblocks[0] = MP_OBJ_FROM_PTR(&psram_writeblocks_obj);
    vfs->blockdev.writeblocks[1] = MP_OBJ_FROM_PTR(&psram_obj);
    vfs->blockdev.writeblocks[2] = MP_OBJ_FROM_PTR(direct_psram_write_blocks);
    vfs->blockdev.u.ioctl[0] = MP_OBJ_FROM_PTR(&psram_ioctl_obj);
    vfs->blockdev.u.ioctl[1] = MP_OBJ_FROM_PTR(&psram_obj);
}

mp_obj_t psram_wipe_and_setup(mp_obj_t unused_self)
{
    // Erase and reformat filesystem
    //  - before calling do this, probably should unmount it.

    // Wipe contents for security.
    memset(PSRAM_BASE, 0x21, BLOCK_SIZE * BLOCK_COUNT);

    // Build obj to handle python protocol
    fs_user_mount_t vfs;
    psram_init_vfs(&vfs);

    // newfs:
    // - FAT16 (auto)
    // - cluster=sector=512 to keep it simple
    // - FM_SFD=>start sector=0, not 63 "single partition" no MBR wastage
    uint8_t working_buf[FF_MAX_SS];
    FRESULT res = f_mkfs(&vfs.fatfs, FM_FAT|FM_SFD, BLOCK_SIZE, working_buf, sizeof(working_buf));
    if (res != FR_OK) {
        mp_printf(&mp_plat_print, "PSRAM: can't create filesystem\n");
        goto fail;
    }

    // set volume label, which becomes mountpoint on MacOS
    // .. can't do this from python AFAIK
    f_setlabel(&vfs.fatfs, "COLDCARD");

    FIL fp;
    UINT n;

    // create an ident file, or two
    // - algo matches shared/version.py serial_number() function
    {   char    fname[80];
        const uint8_t *id = (const uint8_t *)MP_HAL_UNIQUE_ID_ADDRESS;     // 12 bytes, binary
        snprintf(fname, sizeof(fname),
                "ckcc-%02X%02X%02X%02X%02X%02X.txt",
                id[11], id[10] + id[2], id[9], id[8] + id[0], id[7], id[6]);

        f_open(&vfs.fatfs, &fp, fname, FA_WRITE | FA_CREATE_ALWAYS);
        f_write(&fp, fname+5, 12, &n);
        f_close(&fp);

        f_open(&vfs.fatfs, &fp, "serial.txt", FA_WRITE | FA_CREATE_ALWAYS);
        f_write(&fp, fname+5, 12, &n);
        f_close(&fp);
    }

    return mp_const_none;

fail:
    mp_raise_ValueError(NULL);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(psram_wipe_obj, psram_wipe_and_setup);

// copy from lib/oofatfs/ff.c
static DWORD clst2sect (    /* !=0:Sector number, 0:Failed (invalid cluster#) */
    FATFS* fs,      /* Filesystem object */
    DWORD clst      /* Cluster# to be converted */
)
{
    clst -= 2;      /* Cluster number is origin from 2 */
    if (clst >= fs->n_fatent - 2) return 0;     /* Is it invalid cluster number? */
    return fs->database + fs->csize * clst;     /* Start sector number of the cluster */
}


mp_obj_t psram_mmap_file(mp_obj_t unused_self, mp_obj_t fname_in)
{
    // Find a file on FATFS and return a list of tuples which
    // provide the physical locations/lengths of the bytes of the
    // file's contents. Effectively it's the mmap call.
    const char *fname = mp_obj_str_get_str(fname_in);

    // Build obj to handle python protocol
    fs_user_mount_t vfs;
    psram_init_vfs(&vfs);

    FRESULT res = f_mount(&vfs.fatfs);
    if (res != FR_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("unmounted"));
        goto fail;
    }

#if 0
    pyb_file_obj_t  *file

    file->fp
#endif

    // see <http://elm-chan.org/fsw/ff/doc/lseek.html> to learn this magic
    FIL fp = {};

    // import ckcc; ckcc.PSRAM().mmap('serial.txt')

    if(f_open(&vfs.fatfs, &fp, fname, FA_READ) != FR_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("open file"));
        goto fail;
    }

    DWORD   mapping[64];
    mapping[0] = MP_ARRAY_SIZE(mapping);
    fp.cltbl = mapping;

    int rv = f_lseek(&fp, CREATE_LINKMAP);
    if(rv != FR_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("lseek"));
        goto fail;
    }

    // Convert and remap list of clusters
    
    int num_used = (mapping[0] - 1)/2;
printf("[0] = %lu\n", mapping[0]);
printf("num = %d\n", num_used);
    DWORD *ptr = &mapping[1];

    for(int i=0; i<num_used; i++) {
        int num_clusters = *(ptr++);
        uint32_t cluster = *(ptr++);
        //if(!cluster) break;         // sentinel

        printf("[%d] (cl=0x%lx ln=%d) => ", i, cluster, num_clusters);
        printf("0x%lx\n", clst2sect(&vfs.fatfs, cluster));
    }
    f_close(&fp);

    return mp_const_none;
fail:
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(psram_mmap_file_obj, psram_mmap_file);


STATIC const mp_rom_map_elem_t psram_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&psram_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&psram_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&psram_ioctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_wipe), MP_ROM_PTR(&psram_wipe_obj) },
    { MP_ROM_QSTR(MP_QSTR_mmap), MP_ROM_PTR(&psram_mmap_file_obj) },
};

STATIC MP_DEFINE_CONST_DICT(psram_locals_dict, psram_locals_dict_table);

// our block device object for Micropython
const mp_obj_type_t psram_type = {
    { &mp_type_type },
    .name = MP_QSTR_PSRAM,
    .print = psram_print,
    .make_new = psram_make_new,
    .locals_dict = (mp_obj_dict_t *)&psram_locals_dict,
};


// EOF
