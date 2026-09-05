/**
  ******************************************************************************
  * @file    ffconf.h
  * @brief   FatFs module configuration for APIE (STM32H7R3Z8 sink rig).
  *          Minimal footprint: single SD volume, 8.3 names only, code page 437
  *          (Latin), no reentrancy (no RTOS), f_printf enabled for log lines,
  *          FS_TINY=1 to share the single sector buffer across file objects.
  *          Read-write, no exFAT, no LFN, no relative path.
  ******************************************************************************
  */
#ifndef FFCONF_H_DEFINED
#define FFCONF_H_DEFINED

#define FFCONF_DEF      80286   /* Revision ID must match ff.h (ChaN R0.15) */

#define FF_FS_READONLY  0
#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0
#define FF_USE_MKFS     0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  1   /* enable f_puts / f_printf for AppendLine() */
#define FF_PRINT_LLI    1
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  3   /* UTF-8 output (we only write ASCII anyway) */
#define FF_CODE_PAGE    437 /* US English */
#define FF_USE_LFN      0   /* 8.3 only — smallest footprint */
#define FF_LFN_BUF      255
#define FF_SFN_BUF      12
#define FF_FS_RPATH     0
#define FF_VOLUMES      1
#define FF_VOLUME_STRS  "sd"
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS       512
#define FF_MAX_SS       512
#define FF_LBA64        0
#define FF_MIN_GPT      0x100000000
#define FF_USE_TRIM     0
#define FF_FS_TINY      1
#define FF_FS_EXFAT     0
#define FF_FS_NORTC     0
#define FF_NORTC_MON    1
#define FF_NORTC_MDAY   1
#define FF_NORTC_YEAR   2025
#define FF_FS_NOFSINFO  0
#define FF_FS_LOCK      0
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT   1000
#define FF_SYNC_t       HANDLE

#endif /* FFCONF_H_DEFINED */
