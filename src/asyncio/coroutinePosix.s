/*
  Use rewritten libaco context switch function
  https://github.com/hnes/libaco
*/

.text
.globl switchContext
#if defined(__APPLE__)
#else
.type  switchContext, @function
#endif
.intel_syntax noprefix
switchContext:

/*
  context+0x00 = EIP
  context+0x04 = ESP
  context+0x08 = EBP
  context+0x0C = EDI
  context+0x10 = ESI
  context+0x14 = EBX
  context+0x18 = fpucw16
  context+0x1C = mxcsr32
*/

#ifdef __i386__
    // EDX = return address (EIP)
    mov     edx, DWORD PTR [esp]
    // ECX = stack address after return (ESP)
    lea     ecx, [esp+0x4]
    // EAX = source context
    mov     eax, DWORD PTR [esp+0x4]
    mov     DWORD PTR [eax+0x00], edx /* EIP */
    mov     DWORD PTR [eax+0x04], ecx /* ESP */
    mov     DWORD PTR [eax+0x08], ebp
    mov     DWORD PTR [eax+0x0C], edi
    mov     DWORD PTR [eax+0x10], esi
    mov     DWORD PTR [eax+0x14], ebx
    fnstcw  WORD  PTR [eax+0x18]
    stmxcsr DWORD PTR [eax+0x1C]

    // ECX = destination context
    mov     ecx, DWORD PTR [esp+0x08]

    mov     eax, DWORD PTR [ecx+0x00] /* EIP */
    mov     edx, DWORD PTR [ecx+0x04] /* ESP */
    mov     ebp, DWORD PTR [ecx+0x08]
    mov     edi, DWORD PTR [ecx+0x0C]
    mov     esi, DWORD PTR [ecx+0x10]
    mov     ebx, DWORD PTR [ecx+0x14]
    fldcw   WORD  PTR      [ecx+0x18]
    ldmxcsr DWORD PTR      [ecx+0x1C]
    mov     esp,edx
    jmp     eax

#elif __x86_64__
/*
  context+0x00 = R12
  context+0x08 = R13
  context+0x10 = R14
  context+0x18 = R15
  context+0x20 = RIP
  context+0x28 = RSP
  context+0x30 = RBX
  context+0x38 = RBP
  context+0x40 = fpucw16
  context+0x44 = mxcsr32
*/

    // RDI = source context
    // EDX = return address (RIP)
    mov     rdx, QWORD PTR [rsp]
    // RCX = stack address after return (ESP)
    lea     rcx, [rsp+0x8]
    mov     QWORD PTR [rdi+0x00], r12
    mov     QWORD PTR [rdi+0x08], r13
    mov     QWORD PTR [rdi+0x10], r14
    mov     QWORD PTR [rdi+0x18], r15
    mov     QWORD PTR [rdi+0x20], rdx /* RIP */
    mov     QWORD PTR [rdi+0x28], rcx /* RSP */
    mov     QWORD PTR [rdi+0x30], rbx
    mov     QWORD PTR [rdi+0x38], rbp
    fnstcw  WORD PTR  [rdi+0x40]
    stmxcsr DWORD PTR [rdi+0x44]

    // RSI = destination context
    mov     r12, QWORD PTR [rsi+0x00]
    mov     r13, QWORD PTR [rsi+0x08]
    mov     r14, QWORD PTR [rsi+0x10]
    mov     r15, QWORD PTR [rsi+0x18]
    mov     rax, QWORD PTR [rsi+0x20] /* RIP */
    mov     rcx, QWORD PTR [rsi+0x28] /* RSP */
    mov     rbx, QWORD PTR [rsi+0x30]
    mov     rbp, QWORD PTR [rsi+0x38]
    fldcw   WORD PTR       [rsi+0x40]
    ldmxcsr DWORD PTR      [rsi+0x44]
    mov     rsp,rcx
    // RDI is pointer to coroutineTy object
    // Used as first fiberEntryPoint function argument
    mov     rdi, rsi
    jmp     rax

#else
    #error "platform not supported"
#endif

.globl x86InitFPU
#if defined(__APPLE__)
#else
.type  x86InitFPU, @function
#endif
.intel_syntax noprefix
x86InitFPU:
#ifdef __i386__
    mov     eax, DWORD PTR [esp+0x4]
    fnstcw  WORD  PTR [eax+0x18]
    stmxcsr DWORD PTR [eax+0x1C]
    ret 4
#elif __x86_64__
    fnstcw  WORD PTR  [rdi+0x40]
    stmxcsr DWORD PTR [rdi+0x44]
    ret
#else
    #error "platform not supported"
#endif
