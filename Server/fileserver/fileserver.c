/**
 * fileserver.c - a real, standalone userspace file server.
 *
 * WHAT THIS PROVES
 * -----------------
 * Not just that the SYS_IPC_* syscalls added this session work - an
 * earlier in-kernel test (COS_VALIDATION_IPC_TEST) already proved the
 * layer beneath them is sound. This proves the actual point of adding
 * them: that a SEPARATE, ordinary userspace PROCESS can receive a
 * request from ANOTHER userspace process and answer it, entirely through
 * the syscall boundary - real argument marshalling across two
 * independently-scheduled processes, not a kernel thread calling the
 * IPC functions directly.
 *
 * SCOPE, DELIBERATELY NARROW
 * --------------------------
 * This is a read-only file server: it opens a path a client asks for and
 * sends the content back. It does NOT replace the kernel's own FatFs
 * path (cos_fs_read_file(), reached via SYS_READ_FILE) - it is built ON
 * TOP of it, by calling cos_read_file() itself once it receives a
 * request. Moving FatFs itself into a process, so a corrupt filesystem
 * image cannot corrupt the kernel, is the real isolation win a file
 * server should eventually provide - and is NOT attempted here. That
 * needs the disk driver reachable from userspace (or a block-server
 * underneath this one) and, more importantly, a real answer to the
 * bootstrap ordering problem it creates: the very first userspace
 * process's ELF image has to come from SOMEWHERE before any userspace
 * file server exists to serve it. Getting that ordering wrong does not
 * fail loudly - it fails as "the OS no longer boots", which is a far
 * worse outcome than anything this session's other fixes were catching.
 * This program exists to prove the IPC PATTERN end-to-end first, on a
 * foundation safe enough that getting it wrong just means this one
 * feature doesn't work - not that nothing does.
 *
 * PROTOCOL
 * --------
 * Request  (msg_type = FS_MSG_READ): payload is a NUL-terminated path,
 *          up to COS_IPC_MAX_DATA_SIZE - 1 bytes.
 * Response: sent via cos_ipc_respond(), whose wire format (see
 *          src/kernel/ipc.c) prepends the status as a raw uint64_t
 *          before any payload - it is NOT a separate field the receiver
 *          gets back through cos_ipc_recv()'s return value, which only
 *          reports whether the receive itself succeeded. status = 0 with
 *          the file content following it means success; a negative
 *          FS_ERR_* with no payload after it means failure. Every
 *          caller of cos_ipc_respond() delegates that framing to the
 *          kernel; only the RECEIVING side (fileclient.c's
 *          unpack_response()) has to know to peel the first 8 bytes off.
 *          A file larger than one IPC message is reported as
 *          FS_ERR_TOO_LARGE rather than silently truncated - a client
 *          that got back less than the real file with no way to tell
 *          would be a worse failure mode than a clear error.
 * Request  (msg_type = FS_MSG_SHUTDOWN): no payload; the server responds
 *          once (status 0) and then exits. Given only for tests to be
 *          able to end this process deterministically rather than
 *          leaving it running forever with nothing left to talk to it.
 */
#include "cos.h"

#define FS_MSG_READ     1u
#define FS_MSG_SHUTDOWN 2u

#define FS_ERR_BAD_REQUEST  (-1)
#define FS_ERR_NOT_FOUND    (-2)
#define FS_ERR_TOO_LARGE    (-3)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    cos_puts("[fileserver] starting, pid=");
    /* No integer-to-string helper in this tiny libc surface worth adding
     * for one diagnostic line - cos_getpid() is reported by whoever
     * spawns this process instead, which already needs the value to
     * address it. */
    cos_puts("(see spawner's own log)\n");

    static char path_buf[COS_IPC_MAX_DATA_SIZE];
    static char file_buf[COS_IPC_MAX_DATA_SIZE];

    for (;;) {
        cos_ipc_msg_t hdr;
        int rc = cos_ipc_recv(&hdr, path_buf, sizeof(path_buf),
                              COS_IPC_WAIT_FOREVER);
        if (rc != COS_IPC_SUCCESS) {
            /* WAIT_FOREVER should never time out; anything else here is
             * a real problem worth stopping for rather than spinning on. */
            cos_puts("[fileserver] recv failed, exiting\n");
            return 1;
        }

        if (hdr.msg_type == FS_MSG_SHUTDOWN) {
            cos_ipc_respond(hdr.src_pid, 0, 0, 0, 0);
            cos_puts("[fileserver] shutdown requested, exiting\n");
            return 0;
        }

        if (hdr.msg_type != FS_MSG_READ || hdr.length == 0 ||
            hdr.length >= sizeof(path_buf)) {
            cos_ipc_respond(hdr.src_pid, 0, (uint64_t)FS_ERR_BAD_REQUEST, 0, 0);
            continue;
        }
        /* cos_ipc_recv() truncates to the caller's buffer capacity but
         * does not itself NUL-terminate - the payload is opaque bytes as
         * far as IPC is concerned. This server's own protocol says the
         * payload IS a path string, so enforcing termination is this
         * server's job, not the transport's. */
        path_buf[hdr.length] = '\0';

        ssize_t got = cos_read_file(path_buf, file_buf, sizeof(file_buf));
        if (got < 0) {
            cos_ipc_respond(hdr.src_pid, 0, (uint64_t)FS_ERR_NOT_FOUND, 0, 0);
        } else if ((size_t)got >= sizeof(file_buf)) {
            /* cos_read_file() filled the whole buffer - the real file may
             * be exactly this size or larger; either way this transport
             * cannot prove it got the complete file, so it must not claim
             * to. */
            cos_ipc_respond(hdr.src_pid, 0, (uint64_t)FS_ERR_TOO_LARGE, 0, 0);
        } else {
            cos_ipc_respond(hdr.src_pid, 0, 0, file_buf, (uint64_t)got);
        }
    }
}
