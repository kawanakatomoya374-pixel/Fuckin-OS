; Storage blob placeholder - will be replaced by build system
; This file reserves space for the storage image

section .storage
    global storage_data_start
    global storage_data_end
    global storage_data_size

; Placeholder - actual data will be inserted by pack_storage.go
storage_data_start:
    times 65536 db 0  ; 64KB placeholder (will be expanded)
storage_data_end:

storage_data_size: dd storage_data_end - storage_data_start
