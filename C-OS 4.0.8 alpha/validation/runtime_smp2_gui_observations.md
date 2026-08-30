# Secure Boot SMP2 GUI observation — 2026-08-26

The latest signed `C-OS_4.0.8_alpha_secure.iso` reached the 1024×768 C-OS desktop under the strict q35/TCG multi-thread profile with two virtual CPUs. The desktop was visually stable and showed the expected NetSurf browser desktop icon in the first row, second column; its approximate activation centre is **(136, 53)** in the 1024×768 framebuffer.

The taskbar is visible and no kernel panic, exception overlay, or boot-failure screen was visible. Serial evidence is retained separately in `runtime_smp2_serial.log` and confirms GUI entry, AC97 initialization, E1000 initialization, TinyUSB EHCI enumeration start, and enabled scheduler preemption.

A second screen capture after relative QMP mouse injection and double-click still showed the desktop unchanged, with the on-screen cursor remaining near the screen centre. This establishes that the default QMP input route was not controlling the TinyUSB EHCI mouse consumed by C-OS. `query-mice` reported both a QEMU HID mouse (index 4, non-current) and a PS/2 mouse (index 2, current); the next diagnostic must explicitly select the HID device before injecting relative movement.
