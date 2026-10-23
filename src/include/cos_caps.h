/* cos_caps.h - capabilities a ring-3 process holds. A process starts with COS_CAP_ALL and can only ever give some up
 * (SYS_SANDBOX); children inherit what the parent has left. Mirrored in userland/include/cos.h. */
#ifndef COS_CAPS_H
#define COS_CAPS_H

#define COS_CAP_FS_READ   0x00000001u   /* open/read/stat/list files, load libraries */
#define COS_CAP_FS_WRITE  0x00000002u   /* create/modify/delete/rename files and directories */
#define COS_CAP_NET       0x00000004u   /* name resolution, HTTP */
#define COS_CAP_SPAWN     0x00000008u   /* start programs, signal other processes, open files in the file manager */
#define COS_CAP_WINDOW    0x00000010u   /* windows, clipboard, UI preferences, image decoding */
#define COS_CAP_AUDIO     0x00000020u
#define COS_CAP_SETTINGS  0x00000040u   /* read/change system settings (appearance, input, autostart ...); also needed by the network and scan actions */
#define COS_CAP_DEVICES   0x00000080u   /* inventory of PS/2 + USB devices */
#define COS_CAP_IPC       0x00000100u   /* IPC endpoints and shared memory */
#define COS_CAP_COMPUTE   0x00000200u   /* use the other CPUs (cos_par worker pool) */
#define COS_CAP_SYSINFO   0x00000400u   /* read-only system statistics and the process list */
#define COS_CAP_SHELL     0x00000800u   /* act as the desktop shell: own the taskbar panel, list and control other windows */
#define COS_CAP_POWER     0x00001000u   /* sleep, restart, shut down (SYS_SETTINGS_ACTION 1..3) */
#define COS_CAP_ACCOUNT   0x00002000u   /* change the administrator password */
#define COS_CAP_UNKNOWN  0x80000000u   /* any syscall not classified above: only a fully privileged process may use it */
#define COS_CAP_ALL       0xFFFFFFFFu

#endif
