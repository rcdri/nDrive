/* nDrive - entry gates for the hooked OS register helpers.
 * The originals are leaf functions:
 *   read32:  ldr r3,[r0,#8]; ldr r0,[r1,r3]; bx lr      (r0 = handle, r1 = offset)
 *   write32: ldr r3,[r0,#8]; str r2,[r1,r3]; bx lr      (r2 = value)
 * The gates keep every register except the result intact and align the stack
 * for C, since callers (tasks, interrupt handlers) may have 4-byte stacks. */
.syntax unified
.arm
.text
.align 2

.global usbhook_read_gate
.type usbhook_read_gate, %function
usbhook_read_gate:
    push {r1, r2, r3, r4, r12, lr}
    mov r4, sp
    bic sp, sp, #7
    bl usbhook_read
    mov sp, r4
    pop {r1, r2, r3, r4, r12, pc}
.size usbhook_read_gate, .-usbhook_read_gate

.global usbhook_write_gate
.type usbhook_write_gate, %function
usbhook_write_gate:
    push {r0, r1, r2, r3, r4, r12, lr}
    mov r4, sp
    bic sp, sp, #7
    bl usbhook_write
    mov sp, r4
    pop {r0, r1, r2, r3, r4, r12, pc}
.size usbhook_write_gate, .-usbhook_write_gate

.section .note.GNU-stack,"",%progbits
