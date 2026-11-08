#ifndef UAPI_RESCUE_H
#define UAPI_RESCUE_H

/* Ranix rescue channel (kernel/rescue.c). Answered by the kernel's own network
 * stack, in the RX path, like ping: it keeps working when userspace is wedged
 * (sshd stopped, init stuck, the desktop frozen).
 *
 * Transport: UDP/IPv4 port RESCUE_PORT, host init network namespace only, text.
 *   -> "RX1 HELLO"
 *   <- "RX1 NONCE <nonce:32 hex> <uptime_s> <boot:16 hex>"
 *   -> "RX1 CMD <nonce> <mac:64 hex> <command>"
 *        mac = HMAC-SHA256(key, "RX1 CMD <nonce> <command>")
 *   <- "RX1 R <nonce> <seq> <more:0|1> <mac>\n<body>"   (one or more packets)
 *        mac = HMAC-SHA256(key, "RX1 R <nonce> <seq> <more>\n<body>")
 *   <- "RX1 ERR <reason>"                                (unauthenticated)
 * A nonce lives RESCUE_NONCE_SECS and is spent by its first CMD, good or bad, so
 * a captured command cannot be replayed and the key cannot be guessed online.
 *
 * Sources: LAN only (127/8, 10/8, 172.16/12, 192.168/16, 169.254/16); 40 packets/s,
 * 5 bad MACs/s.
 *
 * Commands: ping | ps | stuck | log [bytes] | kill <pid> [sig] | killpg <pgid> [sig]
 *           | sync | reboot [grace_s] | reboot-now | poweroff | deadline <s> | claims
 *
 * /dev/rescue (root): write "key <64 hex>" sets the key (no key = channel off),
 * "testkey <64 hex>" sets a second key that expires after 120 s (self-tests use
 * it so they never disturb the real one), "off" clears both. read: status text.
 */

#define RESCUE_PORT        7395
#define RESCUE_NONCE_SECS  30
#define RESCUE_KEY_BYTES   32

/* Self-test hooks on /dev/rescue (CAP_SYS_ADMIN). They exist so the
 * kill-always-wins test can drive the kernel paths it guards. arg = seconds (1..60). */
#define RESCUE_IOC_TEST_LOOP  0x5201  /* kernel loop that checks kloop_should_stop: -EINTR when killed, 0 at timeout */
#define RESCUE_IOC_TEST_HOLD  0x5202  /* take the test mutex and sleep (killable) holding it */
#define RESCUE_IOC_TEST_LOCK  0x5203  /* mutex_lock_killable on the test mutex, then release */
#define RESCUE_IOC_TEST_SPIN  0x5204  /* busy loop that never checks: drives the kstall watch */

#endif
