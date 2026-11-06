; WRG.COM - WifiRetroGate console for DOS, over a packet driver.
;
; Talks to WifiRetroGate with raw Ethernet frames (EtherType 0x88B5, the same
; protocol as wrg.py, see esp32-bridge/main/rawcfg.h). Needs nothing but a
; packet driver for the network card: no TCP/IP, no configuration.
;
;   WRG                 interactive prompt, "exit" to quit
;   WRG status          run one command and exit (output can be redirected)
;   WRG wifi add "My Network" password
;
; Exit code: 0 = answered, 1 = no answer or error, 2 = no packet driver.
;
; Build: nasm -f bin -o WRG.COM wrg.asm   (8086, runs on any PC with DOS 2+)

	cpu	8086
	org	100h

SLOTS		equ 4		; receive buffers (a reply has up to 3 parts)
SLOTSIZE	equ 1516
HEADER		equ 23		; Ethernet header 14 + WRG header 9
MINFRAME	equ 60
LINEMAX		equ 199		; the device reads at most 199 characters
TIMEOUT		equ 146		; BIOS ticks (18.2 per second): 8 s, scan is slow
RETRIES		equ 3

start:
	cld
	mov	byte [inbuf], LINEMAX+1

	; command line: PSP 80h = length, 81h = text
	mov	si, 81h
	mov	cl, [80h]
	xor	ch, ch
	call	trim
	mov	[arg_ptr], si
	mov	[arg_len], cx
	cmp	cx, 2
	jne	find_driver
	cmp	word [si], '/?'
	jne	find_driver
	mov	dx, msg_usage
	call	puts
	mov	ax, 4C00h
	int	21h

	; a packet driver has "PKT DRVR" 3 bytes into its handler, INT 60h..80h
find_driver:
	mov	bx, 60h*4
.next:
	xor	ax, ax
	mov	es, ax
	les	di, [es:bx]
	add	di, 3
	mov	si, sig
	mov	cx, 8
	repe	cmpsb
	je	.found
	add	bx, 4
	cmp	bx, 80h*4
	jbe	.next
	mov	dx, msg_nodriver
	call	puts
	mov	ax, 4C02h
	int	21h
.found:
	mov	ax, bx
	shr	ax, 1
	shr	ax, 1
	mov	[pkt_int], al

	; access_type: Ethernet II (class 1), any type and number, our EtherType
	push	ds
	pop	es
	mov	ax, 0201h
	mov	bx, 0FFFFh
	xor	dl, dl
	mov	si, ethertype
	mov	cx, 2
	mov	di, receiver
	call	pkt_call
	jnc	.registered
	mov	dx, msg_access
	jmp	driver_error
.registered:
	mov	[handle], ax
	mov	byte [have_handle], 1

	; our MAC address, the replies come to it
	push	ds
	pop	es
	mov	ah, 6
	mov	bx, [handle]
	mov	di, mymac
	mov	cx, 6
	call	pkt_call
	jnc	.have_mac
	mov	dx, msg_getaddr
	jmp	driver_error
.have_mac:

	; Ctrl+C / Ctrl+Break must give the handle back, or the driver would
	; later call a receiver that is no longer in memory
	mov	ax, 2523h
	mov	dx, ctrl_c
	int	21h

	; sequence numbers start from the clock
	call	ticks
	mov	[seq], dx

	mov	cx, [arg_len]
	jcxz	interactive
	mov	si, [arg_ptr]
	call	command
	mov	al, 0
	jnc	.exit
	mov	al, 1
.exit:
	jmp	quit_al

interactive:
	mov	dx, msg_banner1
	call	puts
	mov	al, [pkt_int]
	call	puthex8
	mov	dx, msg_banner2
	call	puts
	call	putmac
	mov	dx, msg_banner3
	call	puts
.prompt:
	mov	dx, msg_prompt
	call	puts
	mov	dx, inbuf
	mov	ah, 0Ah
	int	21h
	mov	dx, crlf
	call	puts
	mov	si, inbuf+2
	mov	cl, [inbuf+1]
	xor	ch, ch
	call	trim
	jcxz	.prompt
	cmp	cx, 4
	jne	.run
	mov	ax, [si]
	or	ax, 2020h		; lower case
	mov	dx, [si+2]
	or	dx, 2020h
	cmp	dx, 'it'
	jne	.run
	cmp	ax, 'ex'
	je	.quit
	cmp	ax, 'qu'
	je	.quit
.run:
	call	command
	jmp	.prompt
.quit:
	mov	al, 0
	jmp	quit_al

; DX = message; the driver's error code is in DH
driver_error:
	push	dx
	call	puts
	pop	dx
	mov	al, dh
	call	puthex8
	mov	dx, msg_crlf_end
	call	puts
	mov	al, 1
	; fall through

; leave with exit code AL, the handle given back
quit_al:
	push	ax
	call	release
	pop	ax
	mov	ah, 4Ch
	int	21h

ctrl_c:
	push	cs
	pop	ds
	call	release
	mov	ax, 4C01h
	int	21h

release:
	cmp	byte [have_handle], 0
	je	.done
	mov	ah, 3
	mov	bx, [handle]
	call	pkt_call
	mov	byte [have_handle], 0
.done:
	ret

; call the packet driver (the INT number is patched in)
pkt_call:
	push	ds
	db	0CDh			; INT
pkt_int:
	db	60h
	pop	ds
	ret

; skip spaces and tabs at both ends of SI/CX
trim:
	jcxz	.done
	cmp	byte [si], ' '
	je	.front
	cmp	byte [si], 9
	jne	.back
.front:
	inc	si
	dec	cx
	jmp	trim
.back:
	mov	bx, si
	add	bx, cx
	cmp	byte [bx-1], ' '
	je	.cut
	cmp	byte [bx-1], 9
	jne	.done
.cut:
	dec	cx
	jnz	.back
.done:
	ret

; Send the command SI/CX and print the reply. CF set if no full reply.
command:
	push	ds
	pop	es
	cmp	cx, LINEMAX
	jbe	.fits
	mov	cx, LINEMAX
.fits:
	inc	word [seq]
	mov	bx, si
	mov	di, txbuf
	mov	ax, 0FFFFh		; to everyone: the device answers
	stosw
	stosw
	stosw
	mov	si, mymac
	movsw
	movsw
	movsw
	mov	ax, 0B588h		; EtherType 88 B5
	stosw
	mov	ax, 'WR'
	stosw
	mov	ax, 0147h		; "G", version 1
	stosw
	mov	al, 1			; request
	stosb
	mov	ax, [seq]		; big endian
	xchg	al, ah
	stosw
	mov	ax, 0100h		; part 0, last
	stosw
	mov	si, bx
	rep	movsb
	mov	ax, di
	sub	ax, txbuf
	cmp	ax, MINFRAME
	jae	.long
	mov	cx, MINFRAME
	sub	cx, ax
	xor	al, al
	rep	stosb
	mov	ax, MINFRAME
.long:
	mov	[txlen], ax

	mov	byte [tries], RETRIES
	mov	byte [expect], 0
	mov	byte [done], 0
	mov	byte [lastch], 10
.send:
	mov	ah, 4
	mov	si, txbuf
	mov	cx, [txlen]
	call	pkt_call
	jnc	.sent
	mov	dx, msg_send
	call	puts
	stc
	ret
.sent:
	call	ticks
	mov	[t0], dx
.wait:
	call	poll_rx
	cmp	byte [done], 0
	jne	.complete
	mov	ah, 1			; Esc stops waiting
	int	16h
	jz	.clock
	cmp	al, 27
	jne	.clock
	xor	ah, ah
	int	16h
	call	endline
	mov	dx, msg_cancel
	call	puts
	stc
	ret
.clock:
	call	ticks
	sub	dx, [t0]
	cmp	dx, TIMEOUT
	jb	.wait
	cmp	byte [expect], 0
	jne	.incomplete
	dec	byte [tries]
	jnz	.send
	mov	dx, msg_noanswer
	call	puts
	stc
	ret
.incomplete:
	call	endline
	mov	dx, msg_incomplete
	call	puts
	stc
	ret
.complete:
	call	endline
	clc
	ret

; handle the frames the receiver has filled, oldest first
poll_rx:
	mov	bx, [rx_tail]
	cmp	word [slot_state+bx], 2
	jne	.done
	mov	si, [slot_addr+bx]
	mov	cx, [slot_len+bx]
	push	bx
	call	frame_in
	pop	bx
	mov	word [slot_state+bx], 0
	add	bx, 2
	and	bx, SLOTS*2-1
	mov	[rx_tail], bx
	jmp	poll_rx
.done:
	ret

; SI/CX = received frame; print it if it is the next part of our reply
frame_in:
	cmp	cx, HEADER
	jb	.no
	push	ds
	pop	es
	push	si
	push	cx
	mov	di, mymac
	mov	cx, 3
	repe	cmpsw
	pop	cx
	pop	si
	jne	.no
	cmp	word [si+12], 0B588h
	jne	.no
	cmp	word [si+14], 'WR'
	jne	.no
	cmp	word [si+16], 0147h	; "G", version 1
	jne	.no
	cmp	byte [si+18], 2		; reply
	jne	.no
	mov	ax, [seq]
	xchg	al, ah
	cmp	[si+19], ax
	jne	.no
	mov	al, [si+21]
	cmp	al, [expect]
	jne	.no			; a repeat (after a retry) or a gap
	inc	byte [expect]
	test	byte [si+22], 1
	jz	.text
	mov	byte [done], 1
.text:
	add	si, HEADER
	sub	cx, HEADER
	jcxz	.printed
.char:
	lodsb
	or	al, al			; Ethernet padding
	jz	.printed
	call	putc
	loop	.char
.printed:
	call	ticks			; more parts may follow: wait again
	mov	[t0], dx
.no:
	ret

; print one character of UTF-8 text: "\n" becomes CR LF, any non-ASCII
; character a "?"
putc:
	cmp	al, 10
	je	.newline
	cmp	al, 9
	je	.out
	cmp	al, 20h
	jb	.skip
	cmp	al, 80h
	jb	.out
	cmp	al, 0C0h
	jb	.skip			; continuation byte
	mov	al, '?'
.out:
	mov	[lastch], al
	mov	dl, al
	mov	ah, 2
	int	21h
.skip:
	ret
.newline:
	mov	[lastch], al
	mov	dx, crlf
	jmp	puts

; end the output with a line break
endline:
	cmp	byte [lastch], 10
	je	.done
	mov	byte [lastch], 10
	mov	dx, crlf
	call	puts
.done:
	ret

; DX = BIOS tick counter (read directly: INT 1Ah would eat DOS's midnight flag)
ticks:
	push	es
	mov	dx, 40h
	mov	es, dx
	mov	dx, [es:6Ch]
	pop	es
	ret

puts:
	mov	ah, 9
	int	21h
	ret

puthex8:
	push	ax
	mov	cl, 4
	shr	al, cl
	call	.nibble
	pop	ax
.nibble:
	and	al, 0Fh
	add	al, '0'
	cmp	al, '9'
	jbe	.out
	add	al, 'a' - '9' - 1
.out:
	mov	dl, al
	mov	ah, 2
	int	21h
	ret

putmac:
	mov	si, mymac
	mov	cx, 6
.byte:
	lodsb
	push	cx
	call	puthex8
	pop	cx
	cmp	cx, 1
	je	.done
	mov	dl, ':'
	mov	ah, 2
	int	21h
	loop	.byte
.done:
	ret

; Packet driver receiver, called by the driver twice per frame:
; AX = 0: CX = length, return a buffer in ES:DI (0:0 = drop the frame)
; AX = 1: the frame is in the buffer (DS:SI)
receiver:
	or	ax, ax
	jnz	.filled
	cmp	cx, SLOTSIZE
	ja	.drop
	push	bx
	mov	bx, [cs:rx_head]
	cmp	word [cs:slot_state+bx], 0
	jne	.full
	mov	word [cs:slot_state+bx], 1
	mov	[cs:slot_len+bx], cx
	mov	di, [cs:slot_addr+bx]
	push	cs
	pop	es
	add	bx, 2
	and	bx, SLOTS*2-1
	mov	[cs:rx_head], bx
	pop	bx
	retf
.full:
	pop	bx
.drop:
	xor	di, di
	mov	es, di
	retf
.filled:
	push	bx
	xor	bx, bx
.find:
	cmp	si, [cs:slot_addr+bx]
	je	.mark
	add	bx, 2
	cmp	bx, SLOTS*2
	jb	.find
	pop	bx
	retf
.mark:
	mov	word [cs:slot_state+bx], 2
	pop	bx
	retf

sig		db 'PKT DRVR'
ethertype	db 88h, 0B5h

msg_usage	db 'WRG - WifiRetroGate console over a packet driver', 13, 10
		db 13, 10
		db '  WRG              interactive, "exit" to quit', 13, 10
		db '  WRG command      run one command, e.g. WRG status', 13, 10
		db 13, 10
		db 'Needs a packet driver for the network card (INT 60h-80h).', 13, 10
		db 'WRG help lists the commands.', 13, 10, '$'
msg_nodriver	db 'No packet driver found (INT 60h-80h). Load the one for your', 13, 10
		db 'network card first, e.g. NE2000 0x60 3 0x300', 13, 10, '$'
msg_access	db 'The packet driver refused EtherType 88B5 (an Ethernet driver is needed), error 0x$'
msg_getaddr	db 'The packet driver did not give its MAC address, error 0x$'
msg_send	db 'The packet driver could not send.', 13, 10, '$'
msg_noanswer	db 'No answer from WifiRetroGate. Is the cable in?', 13, 10, '$'
msg_incomplete	db '(the rest of the answer did not arrive)', 13, 10, '$'
msg_cancel	db '(stopped waiting)', 13, 10, '$'
msg_banner1	db 'WifiRetroGate console, packet driver at INT 0x$'
msg_banner2	db ', MAC $'
msg_banner3	db 13, 10, '"help" lists the commands, "exit" quits.', 13, 10, '$'
msg_prompt	db 'wrg> $'
msg_crlf_end	db '.'
crlf		db 13, 10, '$'

have_handle	db 0
rx_head		dw 0
rx_tail		dw 0
slot_state	times SLOTS dw 0	; 0 = free, 1 = being filled, 2 = filled
slot_addr	dw rxbuf, rxbuf+SLOTSIZE, rxbuf+2*SLOTSIZE, rxbuf+3*SLOTSIZE

	section .bss
inbuf		resb LINEMAX+3		; DOS line input: size, count, text
handle		resw 1
mymac		resb 6
seq		resw 1
arg_ptr		resw 1
arg_len		resw 1
slot_len	resw SLOTS
txlen		resw 1
t0		resw 1
tries		resb 1
expect		resb 1
done		resb 1
lastch		resb 1
txbuf		resb 1514
rxbuf		resb SLOTS*SLOTSIZE
