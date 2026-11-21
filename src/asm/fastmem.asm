default rel

section .text

global mc_fast_copy
global mc_fast_zero
global mc_varint_decode
global mc_varint_encode

mc_fast_copy:
    test rdx, rdx
    jz .done
    cmp rdx, 64
    jb .small
    mov rcx, rdx
    rep movsb
    ret
.small:
    mov rcx, rdx
    shr rcx, 3
    rep movsq
    mov rcx, rdx
    and rcx, 7
    rep movsb
.done:
    ret

mc_fast_zero:
    test rsi, rsi
    jz .done
    xor eax, eax
    cmp rsi, 64
    jb .small
    mov rcx, rsi
    rep stosb
    ret
.small:
    mov rcx, rsi
    shr rcx, 3
    rep stosq
    mov rcx, rsi
    and rcx, 7
    rep stosb
.done:
    ret

mc_varint_decode:
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
.loop:
    cmp r10, rsi
    jae .incomplete
    movzx eax, byte [rdi + r10]
    mov r11d, eax
    and r11d, 0x7F
    mov ecx, r9d
    shl r11d, cl
    or r8d, r11d
    test al, 0x80
    jz .done
    inc r10
    cmp r10, 5
    jae .malformed
    add r9d, 7
    jmp .loop
.done:
    inc r10
    mov dword [rdx], r8d
    mov eax, r10d
    ret
.incomplete:
    xor eax, eax
    ret
.malformed:
    mov eax, -1
    ret

mc_varint_encode:
    mov eax, edi
    xor ecx, ecx
.loop:
    cmp eax, 0x80
    jb .last
    mov r8d, eax
    and r8d, 0x7F
    or r8b, 0x80
    mov byte [rsi + rcx], r8b
    shr eax, 7
    inc ecx
    jmp .loop
.last:
    mov byte [rsi + rcx], al
    inc ecx
    mov eax, ecx
    ret

global mc_fast_copy_avx2
global mc_fast_zero_avx2

mc_fast_copy_avx2:
    test rdx, rdx
    jz .done
    mov rcx, rdx
    cmp rcx, 32
    jb .tail
.loop:
    vmovdqu ymm0, [rsi]
    vmovdqu [rdi], ymm0
    add rsi, 32
    add rdi, 32
    sub rcx, 32
    cmp rcx, 32
    jae .loop
.tail:
    test rcx, rcx
    jz .finish
    rep movsb
.finish:
    vzeroupper
    ret
.done:
    ret

mc_fast_zero_avx2:
    test rsi, rsi
    jz .done
    vpxor ymm0, ymm0, ymm0
    mov rcx, rsi
    cmp rcx, 32
    jb .tail
.loop:
    vmovdqu [rdi], ymm0
    add rdi, 32
    sub rcx, 32
    cmp rcx, 32
    jae .loop
.tail:
    test rcx, rcx
    jz .finish
    xor eax, eax
    rep stosb
.finish:
    vzeroupper
    ret
.done:
    ret
