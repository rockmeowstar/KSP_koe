option casemap:none
PUBLIC UniformUpdateKernelEnd
.code
UniformUpdateKernel PROC
    movsxd r10, edx
    add r10, qword ptr [rcx+8]
    mov r11, r8
    mov rdx, r9
    cmp byte ptr [rsp+28h], 0
    jne CopyBlocks
    test rdx, rdx
    je Finished
    cmp rdx, 1
    jne MultipleBlocks
    mov rax, qword ptr [r11]
    cmp qword ptr [r10], rax
    jne CopySingle
    mov rax, qword ptr [r11+8]
    cmp qword ptr [r10+8], rax
    je Finished
CopySingle:
    movdqu xmm0, xmmword ptr [r11]
    movdqu xmmword ptr [r10], xmm0
    jmp MarkDirty
MultipleBlocks:
    mov rax, qword ptr [r11]
    cmp qword ptr [r10], rax
    jne CopyBlocks
CompareBlocks:
    movdqu xmm0, xmmword ptr [r11]
    movdqu xmm1, xmmword ptr [r10]
    pcmpeqb xmm1, xmm0
    pmovmskb eax, xmm1
    cmp eax, 0ffffh
    jne FirstDifference
    add r10, 16
    add r11, 16
    dec rdx
    jne CompareBlocks
    ret
FirstDifference:
    movdqu xmmword ptr [r10], xmm0
    add r10, 16
    add r11, 16
    dec rdx
    je MarkDirty
    jmp CopyNext
CopyBlocks:
    test rdx, rdx
    je MarkDirty
CopyNext:
    movdqu xmm0, xmmword ptr [r11]
    movdqu xmmword ptr [r10], xmm0
    add r10, 16
    add r11, 16
    dec rdx
    jne CopyNext
MarkDirty:
    mov byte ptr [rcx], 1
Finished:
    ret
UniformUpdateKernelEnd::
UniformUpdateKernel ENDP
END
