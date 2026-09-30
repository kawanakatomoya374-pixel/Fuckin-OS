/**
 * fileclient.c - exercises fileserver.c end to end, entirely through the
 * syscall boundary (not the in-kernel IPC test's shortcut of calling
 * ipc_send()/ipc_receive() directly from kernel context).
 *
 * argv[1] is the fileserver's pid, as a decimal string - passed in by
 * whoever spawns both, since this OS has no name/service registry for a
 * client to look a server up by name (out of scope here; see
 * fileserver.c's own header for why a full server ecosystem is future
 * work, not attempted this session).
 *
 * Sequence:
 *   1. Write a known test file via the ordinary SYS_WRITE_FILE syscall -
 *      proving nothing new, just establishing content to ask the server
 *      to fetch back. Using the kernel's existing file write here rather
 *      than inventing one in the server keeps the server's protocol
 *      read-only and narrow, matching its own stated scope.
 *   2. Ask the server (FS_MSG_READ) to read that exact path back over
 *      IPC, and verify the returned bytes match exactly.
 *   3. Ask a path that does not exist, and verify the server reports
 *      FS_ERR_NOT_FOUND rather than crashing or hanging - a malformed or
 *      merely-unlucky request from a client is routine, not exceptional,
 *      and a server that cannot shrug one off is not providing isolation.
 *   4. Send FS_MSG_SHUTDOWN so the server exits cleanly, and exit itself
 *      with 0 (all checks passed) or 1 (something did not match).
 */
#include "cos.h"

#define FS_MSG_READ     1u
#define FS_MSG_SHUTDOWN 2u
#define FS_ERR_NOT_FOUND (-2)

/* ipc_respond()'s wire format (src/kernel/ipc.c) prepends the status as a
 * raw uint64_t before the caller's payload - it is NOT surfaced through
 * cos_ipc_recv()'s return value, which only reports whether the RECEIVE
 * itself succeeded, not what the far end's application-level status was.
 * A response therefore always looks like an ordinary successful receive;
 * unpacking it into (status, payload, payload_len) is every caller's own
 * job. Copied out via memcpy rather than a cast through an unaligned
 * `char*`, which would be undefined behaviour for a uint64_t read. */
static uint64_t unpack_response(const char *buf, uint64_t total_len,
                                const char **out_payload, uint64_t *out_payload_len)
{
    uint64_t status = (uint64_t)-1;
    if (total_len < sizeof(uint64_t)) {
        *out_payload = 0; *out_payload_len = 0;
        return status;
    }
    cos_memcpy(&status, buf, sizeof(uint64_t));
    *out_payload = buf + sizeof(uint64_t);
    *out_payload_len = total_len - sizeof(uint64_t);
    return status;
}

static uint64_t parse_pid(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); ++s; }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        cos_puts("[fileclient] usage: fileclient <server-pid>\n");
        return 1;
    }
    uint64_t server_pid = parse_pid(argv[1]);
    int failures = 0;

    static const char path[] = "/fileserver_test.txt";
    static const char content[] = "hello from the userspace file server test";

    ssize_t wrote = cos_write_file(path, content, sizeof(content));
    if (wrote != (ssize_t)sizeof(content)) {
        cos_puts("[fileclient] setup write FAILED\n");
        return 1;   /* nothing downstream can be trusted without this */
    }

    /* ---- real read, over IPC, through the syscall boundary ---- */
    {
        int rc = cos_ipc_send(server_pid, FS_MSG_READ, path, sizeof(path));
        if (rc != COS_IPC_SUCCESS) {
            ++failures;
            cos_puts("[fileclient] send(READ) FAILED\n");
        } else {
            cos_ipc_msg_t hdr;
            static char buf[COS_IPC_MAX_DATA_SIZE];
            rc = cos_ipc_recv(&hdr, buf, sizeof(buf), 5000);
            if (rc != COS_IPC_SUCCESS) {
                ++failures;
                cos_puts("[fileclient] recv(READ response) FAILED\n");
            } else {
                const char *payload; uint64_t payload_len;
                uint64_t status = unpack_response(buf, hdr.length, &payload, &payload_len);
                if (status != 0) {
                    ++failures;
                    cos_puts("[fileclient] READ response status != 0\n");
                } else if (payload_len != sizeof(content) ||
                          cos_strncmp(payload, content, sizeof(content)) != 0) {
                    ++failures;
                    cos_puts("[fileclient] READ content MISMATCH\n");
                } else {
                    cos_puts("[fileclient] read-roundtrip OK\n");
                }
            }
        }
    }

    /* ---- a request for a path that does not exist ---- */
    {
        static const char missing[] = "/this_path_does_not_exist.txt";
        int rc = cos_ipc_send(server_pid, FS_MSG_READ, missing, sizeof(missing));
        if (rc != COS_IPC_SUCCESS) {
            ++failures;
            cos_puts("[fileclient] send(missing) FAILED\n");
        } else {
            cos_ipc_msg_t hdr;
            char buf[32];
            rc = cos_ipc_recv(&hdr, buf, sizeof(buf), 5000);
            if (rc != COS_IPC_SUCCESS) {
                ++failures;
                cos_puts("[fileclient] recv(missing response) FAILED\n");
            } else {
                const char *payload; uint64_t payload_len;
                uint64_t status = unpack_response(buf, hdr.length, &payload, &payload_len);
                (void)payload;
                if (status != (uint64_t)FS_ERR_NOT_FOUND) {
                    ++failures;
                    cos_puts("[fileclient] missing-file expected NOT_FOUND, got other\n");
                } else {
                    cos_puts("[fileclient] missing-file NOT_FOUND OK\n");
                }
            }
        }
    }

    cos_ipc_send(server_pid, FS_MSG_SHUTDOWN, 0, 0);

    cos_puts(failures == 0 ? "[fileclient] PASSED\n" : "[fileclient] FAILED\n");
    return failures == 0 ? 0 : 1;
}
