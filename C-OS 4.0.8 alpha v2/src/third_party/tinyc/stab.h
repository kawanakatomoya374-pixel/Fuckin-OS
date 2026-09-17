/* Minimal STAB debug info definitions for TinyCC */
#ifndef STAB_H
#define STAB_H

/* STAB entry structure */
struct nlist {
    union {
        char *n_name;
        long n_strx;
    } n_un;
    unsigned char n_type;
    char n_other;
    short n_desc;
    unsigned long n_value;
};

/* Symbol types */
#define N_UNDF  0x00
#define N_ABS   0x02
#define N_TEXT  0x04
#define N_DATA  0x06
#define N_BSS   0x08
#define N_COMM  0x12
#define N_FN    0x1e

/* Debug symbol types */
#define N_GSYM  0x20
#define N_FNAME 0x22
#define N_FUN   0x24
#define N_STSYM 0x26
#define N_LCSYM 0x28
#define N_MAIN  0x2a
#define N_PC    0x30
#define N_NSYMT 0x32
#define N_SO    0x44
#define N_OSO   0x46
#define N_LSYM  0x80
#define N_BINCL 0x82
#define N_SOL   0x84
#define N_PARAMS 0x85
#define N_VERSION 0x86
#define N_OLEVEL 0x87
#define N_PSYM  0xa0
#define N_EINCL 0xa2
#define N_ENTRY 0xa4
#define N_LBRAC 0xc0
#define N_EXCL  0xc2
#define N_RBRAC 0xe0
#define N_BCOMM 0xe2
#define N_ECOMM 0xe4
#define N_ECOML 0xe8
#define N_LENG  0xfe

#endif /* STAB_H */
