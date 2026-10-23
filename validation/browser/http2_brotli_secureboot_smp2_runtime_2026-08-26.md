# HTTP/2 + Brotli Secure Boot SMP2 runtime validation — 2026-08-26

## Scope

This validation used the **strict q35 / TCG multi-thread / 2 GiB / Secure Boot UEFI** launcher with `--cpus 2`, AC97, E1000, and the EHCI USB keyboard/mouse topology. It exercised the real in-guest HTTP path, not the host-only Brotli fixture decoder.

The dedicated validation build was signed with the existing C-OS development Secure Boot key. A temporary diagnostic worker waited for GUI/E1000/DHCP readiness and issued an ordinary `http_get()` for `https://nghttp2.org/httpbin/brotli`. The normal product build keeps this worker disabled through `HTTP_RUNTIME_SMOKE=0`.

## Final result

| Check | Evidence | Result |
|---|---|---|
| Secure Boot UEFI kernel startup | Signed ISO booted through the strict launcher | Pass |
| SMP | `[SMP] Online CPUs=2` | Pass |
| TLS ALPN | `[TLS] Connected for nghttp2.org ALPN=h2` | Pass |
| HTTP/2 framing and HPACK | `[HTTP/2] Single-stream response received, status: 200, length: 635` | Pass |
| Brotli runtime decoding | Test endpoint returned `Content-Encoding: br`; worker reported a decoded 198-byte body | Pass |
| End-to-end acceptance | `[HTTP-TEST] PASS: HTTP/2+Brotli status=200 bytes=198` | Pass |
| Core device startup | AC97 initialized; E1000 RX/TX rings initialized; TinyUSB service loop active | Pass |
| Fatal kernel markers | No `PANIC`, `#PF`, `#GP`, triple-fault, or OOM marker in the serial log | Pass |

## Defect found and corrected during validation

The first h2 attempt negotiated ALPN successfully, but response status was observed as zero. The root cause was in the HTTP/2 synthetic response bridge: the `:status` pseudo-header populated `http->status_code` but was not appended to the synthetic `HTTP/2 ` status line. The shared HTTP decoder then parsed the following regular header name as the status code and overwrote the value with zero.

The adapter now serializes `:status` into the synthetic status line before handing the response to the existing bounded HTTP decoder. A follow-up in-guest run passed over an actual ALPN `h2` connection. The diagnostic worker was also hardened so an HTTP/1.1 fallback cannot be reported as an HTTP/2 success; it requires `http->used_http2 == 1`.

## Boundary of this validation

This proves one bounded GET stream over a freshly negotiated h2 TLS connection, including client preface/SETTINGS, HPACK header handling, DATA delivery, content decoding, and GOAWAY. It does **not** prove HTTP/2 multiplexing, concurrent streams, or parallel TLS fetch workers. Those remain separately gated on dispatcher/E1000/DNS reentrancy and additional concurrency design work.

## Evidence locations

| Artifact | Purpose |
|---|---|
| `validation/http2_brotli_smp2_serial.log` | Full boot and protocol log for the successful run |
| `validation/http2_brotli_smp2_debug.log` | Strict QEMU diagnostics |
| `build/secureboot/C-OS_4.0.8_alpha_http2_brotli_smoke_secure.iso` | Temporary signed diagnostic ISO; not the intended production deliverable |
| `src/kernel/drivers/http2_client.c` | ALPN-gated single-stream HTTP/2 adapter |
| `src/kernel/kernel.c` | Compile-time-disabled validation worker (`COS_HTTP_RUNTIME_SMOKE`) |
