//
//  patchfinder64.c
//  extra_recipe
//
//  Created by xerub on 06/06/2017.
//  Copyright © 2017 xerub. All rights reserved.
//

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

typedef unsigned long long addr_t;

#define IS64(image) (*(uint8_t *)(image) & 1)

#define MACHO(p) ((*(unsigned int *)(p) & ~1) == 0xfeedface)

/* generic stuff *************************************************************/

#define UCHAR_MAX 255

/* these operate on VA ******************************************************/

#define INSN_RET  0xD65F03C0, 0xFFFFFFFF
#define INSN_CALL 0x94000000, 0xFC000000
#define INSN_B    0x14000000, 0xFC000000
#define INSN_CBZ  0x34000000, 0xFC000000
#define INSN_ADRP 0x90000000, 0x9F000000

static unsigned char *
boyermoore_horspool_memmem(const unsigned char* haystack, size_t hlen,
                           const unsigned char* needle,   size_t nlen)
{
    size_t last, scan = 0;
    size_t bad_char_skip[UCHAR_MAX + 1]; /* Officially called:
                                          * bad character shift */

    /* Sanity checks on the parameters */
    if (nlen <= 0 || !haystack || !needle)
        return NULL;

    /* ---- Preprocess ---- */
    /* Initialize the table to default value */
    /* When a character is encountered that does not occur
     * in the needle, we can safely skip ahead for the whole
     * length of the needle.
     */
    for (scan = 0; scan <= UCHAR_MAX; scan = scan + 1)
        bad_char_skip[scan] = nlen;

    /* C arrays have the first byte at [0], therefore:
     * [nlen - 1] is the last byte of the array. */
    last = nlen - 1;

    /* Then populate it with the analysis of the needle */
    for (scan = 0; scan < last; scan = scan + 1)
        bad_char_skip[needle[scan]] = last - scan;

    /* ---- Do the matching ---- */

    /* Search the haystack, while the needle can still be within it. */
    while (hlen >= nlen)
    {
        /* scan from the end of the needle */
        for (scan = last; haystack[scan] == needle[scan]; scan = scan - 1)
            if (scan == 0) /* If the first byte matches, we've found it. */
                return (void *)haystack;

        /* otherwise, we need to skip some bytes and start again.
           Note that here we are getting the skip value based on the last byte
           of needle, no matter where we didn't match. So if needle is: "abcd"
           then we are skipping based on 'd' and that value will be 4, and
           for "abcdd" we again skip on 'd' but the value will be only 1.
           The alternative of pretending that the mismatched character was
           the last character is slower in the normal case (E.g. finding
           "abcd" in "...azcd..." gives 4 by using 'd' but only
           4-2==2 using 'z'. */
        hlen     -= bad_char_skip[haystack[last]];
        haystack += bad_char_skip[haystack[last]];
    }

    return NULL;
}

/* disassembler **************************************************************/

static int HighestSetBit(int N, uint32_t imm)
{
	int i;
	for (i = N - 1; i >= 0; i--) {
		if (imm & (1 << i)) {
			return i;
		}
	}
	return -1;
}

static uint64_t ZeroExtendOnes(unsigned M, unsigned N)	// zero extend M ones to N width
{
	(void)N;
	return ((uint64_t)1 << M) - 1;
}

static uint64_t RORZeroExtendOnes(unsigned M, unsigned N, unsigned R)
{
	uint64_t val = ZeroExtendOnes(M, N);
	if (R == 0) {
		return val;
	}
	return ((val >> R) & (((uint64_t)1 << (N - R)) - 1)) | ((val & (((uint64_t)1 << R) - 1)) << (N - R));
}

static uint64_t Replicate(uint64_t val, unsigned bits)
{
	uint64_t ret = val;
	unsigned shift;
	for (shift = bits; shift < 64; shift += bits) {	// XXX actually, it is either 32 or 64
		ret |= (val << shift);
	}
	return ret;
}

static int DecodeBitMasks(unsigned immN, unsigned imms, unsigned immr, int immediate, uint64_t *newval)
{
	unsigned levels, S, R, esize;
	int len = HighestSetBit(7, (immN << 6) | (~imms & 0x3F));
	if (len < 1) {
		return -1;
	}
	levels = (unsigned int)ZeroExtendOnes(len, 6);
	if (immediate && (imms & levels) == levels) {
		return -1;
	}
	S = imms & levels;
	R = immr & levels;
	esize = 1 << len;
	*newval = Replicate(RORZeroExtendOnes(S + 1, esize, R), esize);
	return 0;
}

static int DecodeMov(uint32_t opcode, uint64_t total, int first, uint64_t *newval)
{
	unsigned o = (opcode >> 29) & 3;
	unsigned k = (opcode >> 23) & 0x3F;
	unsigned rn, rd;
	uint64_t i;

	if (k == 0x24 && o == 1) {			// MOV (bitmask imm) <=> ORR (immediate)
		unsigned s = (opcode >> 31) & 1;
		unsigned N = (opcode >> 22) & 1;
		if (s == 0 && N != 0) {
			return -1;
		}
		rn = (opcode >> 5) & 0x1F;
		if (rn == 31) {
			unsigned imms = (opcode >> 10) & 0x3F;
			unsigned immr = (opcode >> 16) & 0x3F;
			return DecodeBitMasks(N, imms, immr, 1, newval);
		}
	} else if (k == 0x25) {				// MOVN/MOVZ/MOVK
		unsigned s = (opcode >> 31) & 1;
		unsigned h = (opcode >> 21) & 3;
		if (s == 0 && h > 1) {
			return -1;
		}
		i = (opcode >> 5) & 0xFFFF;
		h *= 16;
		i <<= h;
		if (o == 0) {				// MOVN
			*newval = ~i;
			return 0;
		} else if (o == 2) {			// MOVZ
			*newval = i;
			return 0;
		} else if (o == 3 && !first) {		// MOVK
			*newval = (total & ~((uint64_t)0xFFFF << h)) | i;
			return 0;
		}
	} else if ((k | 1) == 0x23 && !first) {		// ADD (immediate)
		unsigned h = (opcode >> 22) & 3;
		if (h > 1) {
			return -1;
		}
		rd = opcode & 0x1F;
		rn = (opcode >> 5) & 0x1F;
		if (rd != rn) {
			return -1;
		}
		i = (opcode >> 10) & 0xFFF;
		h *= 12;
		i <<= h;
		if (o & 2) {				// SUB
			*newval = total - i;
			return 0;
		} else {				// ADD
			*newval = total + i;
			return 0;
		}
	}

	return -1;
}

/* patchfinder ***************************************************************/

static addr_t
step64(const uint8_t *buf, addr_t start, size_t length, uint32_t what, uint32_t mask)
{
    addr_t end = start + length;
    while (start < end) {
        uint32_t x = *(uint32_t *)(buf + start);
        if ((x & mask) == what) {
            return start;
        }
        start += 4;
    }
    return 0;
}

static addr_t
step64_back(const uint8_t *buf, addr_t start, size_t length, uint32_t what, uint32_t mask)
{
    addr_t end = start - length;
    while (start >= end) {
        uint32_t x = *(uint32_t *)(buf + start);
        if ((x & mask) == what) {
            return start;
        }
        start -= 4;
    }
    return 0;
}

static addr_t
bof64(const uint8_t *buf, addr_t start, addr_t where)
{
    for (; where >= start; where -= 4) {
        uint32_t op = *(uint32_t *)(buf + where);
        if ((op & 0xFFC003FF) == 0x910003FD) {
            unsigned delta = (op >> 10) & 0xFFF;
            //printf("%x: ADD X29, SP, #0x%x\n", where, delta);
            if ((delta & 0xF) == 0) {
                addr_t prev = where - ((delta >> 4) + 1) * 4;
                uint32_t au = *(uint32_t *)(buf + prev);
                if ((au & 0x3BC003E0) == 0x298003E0) {
                    //printf("%x: STP x, y, [SP,#-imm]!\n", prev);
                    return prev;
                }
                for (addr_t diff = 4; diff < delta/4+4; diff+=4) {
                    uint32_t ai = *(uint32_t *)(buf + where - diff);
                    // SUB SP, SP, #imm
                    if ((ai&0xFFC003FF) == 0xD10003FF) {
                        return where - diff;
                    }
                    // Not stp and not str
                    if (((ai & 0xFFC003E0) != 0xA90003E0) && (ai&0xFFC001F0) != 0xF90001E0) {
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

static addr_t
xref64(const uint8_t *buf, addr_t start, addr_t end, addr_t what)
{
    addr_t i;
    uint64_t value[32];

    memset(value, 0, sizeof(value));

    end &= ~3;
    for (i = start & ~3; i < end; i += 4) {
        uint32_t op = *(uint32_t *)(buf + i);
        unsigned reg = op & 0x1F;
        int op_is_adrp = 0;
        
        if ((op & 0x9F000000) == 0x90000000) {
            op_is_adrp = 1;
            signed adr = ((op & 0x60000000) >> 18) | ((op & 0xFFFFE0) << 8);
            //printf("%llx: ADRP X%d, 0x%llx\n", i, reg, ((long long)adr << 1) + (i & ~0xFFF));
            value[reg] = ((long long)adr << 1) + (i & ~0xFFF);
        /*} else if ((op & 0xFFE0FFE0) == 0xAA0003E0) {
            unsigned rd = op & 0x1F;
            unsigned rm = (op >> 16) & 0x1F;
            //printf("%llx: MOV X%d, X%d\n", i, rd, rm);
            value[rd] = value[rm];*/
        } else if ((op & 0xFF000000) == 0x91000000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned shift = (op >> 22) & 3;
            unsigned imm = (op >> 10) & 0xFFF;
            if (shift == 1) {
                imm <<= 12;
            } else {
                //assert(shift == 0);
                if (shift > 1) continue;
            }
            //printf("%llx: ADD X%d, X%d, 0x%x\n", i, reg, rn, imm);
            value[reg] = value[rn] + imm;
        } else if ((op & 0xF9C00000) == 0xF9400000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned imm = ((op >> 10) & 0xFFF) << 3;
            //printf("%llx: LDR X%d, [X%d, 0x%x]\n", i, reg, rn, imm);
            if (!imm) continue;			// XXX not counted as true xref
            value[reg] = value[rn] + imm;	// XXX address, not actual value
        /*} else if ((op & 0xF9C00000) == 0xF9000000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned imm = ((op >> 10) & 0xFFF) << 3;
            //printf("%llx: STR X%d, [X%d, 0x%x]\n", i, reg, rn, imm);
            if (!imm) continue;			// XXX not counted as true xref
            value[rn] = value[rn] + imm;	// XXX address, not actual value*/
        } else if ((op & 0x9F000000) == 0x10000000) {
            signed adr = ((op & 0x60000000) >> 18) | ((op & 0xFFFFE0) << 8);
            //printf("%llx: ADR X%d, 0x%llx\n", i, reg, ((long long)adr >> 11) + i);
            value[reg] = ((long long)adr >> 11) + i;
        } else if ((op & 0xFF000000) == 0x58000000) {
            unsigned adr = (op & 0xFFFFE0) >> 3;
            //printf("%llx: LDR X%d, =0x%llx\n", i, reg, adr + i);
            value[reg] = adr + i;		// XXX address, not actual value
        }
        if (!op_is_adrp && value[reg] == what) {
            return i;
        }
    }
    return 0;
}

static addr_t
calc64(const uint8_t *buf, addr_t start, addr_t end, int which)
{
    addr_t i;
    uint64_t value[32];

    memset(value, 0, sizeof(value));

    end &= ~3;
    for (i = start & ~3; i < end; i += 4) {
        uint32_t op = *(uint32_t *)(buf + i);
        unsigned reg = op & 0x1F;
        if ((op & 0x9F000000) == 0x90000000) {
            signed adr = ((op & 0x60000000) >> 18) | ((op & 0xFFFFE0) << 8);
            //printf("%llx: ADRP X%d, 0x%llx\n", i, reg, ((long long)adr << 1) + (i & ~0xFFF));
            value[reg] = ((long long)adr << 1) + (i & ~0xFFF);
        /*} else if ((op & 0xFFE0FFE0) == 0xAA0003E0) {
            unsigned rd = op & 0x1F;
            unsigned rm = (op >> 16) & 0x1F;
            //printf("%llx: MOV X%d, X%d\n", i, rd, rm);
            value[rd] = value[rm];*/
        } else if ((op & 0xFF000000) == 0x91000000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned shift = (op >> 22) & 3;
            unsigned imm = (op >> 10) & 0xFFF;
            if (shift == 1) {
                imm <<= 12;
            } else {
                //assert(shift == 0);
                if (shift > 1) continue;
            }
            //printf("%llx: ADD X%d, X%d, 0x%x\n", i, reg, rn, imm);
            value[reg] = value[rn] + imm;
        } else if ((op & 0xF9C00000) == 0xF9400000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned imm = ((op >> 10) & 0xFFF) << 3;
            //printf("%llx: LDR X%d, [X%d, 0x%x]\n", i, reg, rn, imm);
            if (!imm) continue;			// XXX not counted as true xref
            value[reg] = value[rn] + imm;	// XXX address, not actual value
        } else if ((op & 0xF9C00000) == 0xF9000000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned imm = ((op >> 10) & 0xFFF) << 3;
            //printf("%llx: STR X%d, [X%d, 0x%x]\n", i, reg, rn, imm);
            if (!imm) continue;			// XXX not counted as true xref
            value[rn] = value[rn] + imm;	// XXX address, not actual value
        } else if ((op & 0x9F000000) == 0x10000000) {
            signed adr = ((op & 0x60000000) >> 18) | ((op & 0xFFFFE0) << 8);
            //printf("%llx: ADR X%d, 0x%llx\n", i, reg, ((long long)adr >> 11) + i);
            value[reg] = ((long long)adr >> 11) + i;
        } else if ((op & 0xFF000000) == 0x58000000) {
            unsigned adr = (op & 0xFFFFE0) >> 3;
            //printf("%llx: LDR X%d, =0x%llx\n", i, reg, adr + i);
            value[reg] = adr + i;		// XXX address, not actual value
        }
    }
    return value[which];
}

static addr_t
calc64mov(const uint8_t *buf, addr_t start, addr_t end, int which)
{
    addr_t i;
    uint64_t value[32];

    memset(value, 0, sizeof(value));

    end &= ~3;
    for (i = start & ~3; i < end; i += 4) {
        uint32_t op = *(uint32_t *)(buf + i);
        unsigned reg = op & 0x1F;
        uint64_t newval;
        int rv = DecodeMov(op, value[reg], 0, &newval);
        if (rv == 0) {
            if (((op >> 31) & 1) == 0) {
                newval &= 0xFFFFFFFF;
            }
            value[reg] = newval;
        }
    }
    return value[which];
}

static addr_t
find_call64(const uint8_t *buf, addr_t start, size_t length)
{
    return step64(buf, start, length, INSN_CALL);
}

static addr_t
follow_call64(const uint8_t *buf, addr_t call)
{
    long long w;
    w = *(uint32_t *)(buf + call) & 0x3FFFFFF;
    w <<= 64 - 26;
    w >>= 64 - 26 - 2;
    return call + w;
}

static addr_t
follow_cbz(const uint8_t *buf, addr_t cbz)
{
    return cbz + ((*(int *)(buf + cbz) & 0x3FFFFE0) << 10 >> 13);
}

/* kernel iOS10 **************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef NOT_DARWIN
#include <mach-o/loader.h>
#else
#include "mach-o_loader.h"
#endif

#ifdef __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__
#include <mach/mach.h>
#endif

enum string_bases {
    string_base_cstring = 0,
    string_base_pstring,
    string_base_oslstring
};

static uint8_t *kernel = NULL;
static size_t kernel_size = 0;

static addr_t xnucore_base = 0;
static addr_t xnucore_size = 0;
static addr_t prelink_base = 0;
static addr_t prelink_size = 0;
static addr_t cstring_base = 0;
static addr_t cstring_size = 0;
static addr_t pstring_base = 0;
static addr_t pstring_size = 0;
static addr_t oslstring_base = 0;
static addr_t oslstring_size = 0;
static addr_t kerndumpbase = -1;
static addr_t kernel_entry = 0;
static void *kernel_mh = 0;
static addr_t kernel_delta = 0;

int
init_kernel(size_t (*kread)(uint64_t, void *, size_t), addr_t kernel_base, const char *filename)
{
    size_t rv;
    uint8_t buf[0x4000];
    unsigned i, j;
    const struct mach_header *hdr = (struct mach_header *)buf;
    const uint8_t *q;
    addr_t min = -1;
    addr_t max = 0;
    int is64 = 0;
    
#ifdef __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__
    if (!kread || !kernel_base) {
        return -1;
    }
#else    /* __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ */
    if (!filename) {
        return -1;
    }
#endif    /* __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ */

#ifdef __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__
#define close(f)
    rv = kread(kernel_base, buf, sizeof(buf));
    if (rv != sizeof(buf)) {
        return -1;
    }
#else	/* __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ */
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    rv = read(fd, buf, sizeof(buf));
    if (rv != sizeof(buf)) {
        close(fd);
        return -1;
    }
#endif	/* __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ */

    if (!MACHO(buf)) {
        close(fd);
        return -1;
    }

    if (IS64(buf)) {
        is64 = 4;
    }

    q = buf + sizeof(struct mach_header) + is64;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (struct segment_command_64 *)q;
            if (min > seg->vmaddr) {
                min = seg->vmaddr;
            }
            if (max < seg->vmaddr + seg->vmsize) {
                max = seg->vmaddr + seg->vmsize;
            }
            if (!strcmp(seg->segname, "__TEXT_EXEC")) {
                xnucore_base = seg->vmaddr;
                xnucore_size = seg->filesize;
            }
            if (!strcmp(seg->segname, "__PLK_TEXT_EXEC")) {
                prelink_base = seg->vmaddr;
                prelink_size = seg->filesize;
            }
            if (!strcmp(seg->segname, "__TEXT")) {
                const struct section_64 *sec = (struct section_64 *)(seg + 1);
                for (j = 0; j < seg->nsects; j++) {
                    if (!strcmp(sec[j].sectname, "__cstring")) {
                        cstring_base = sec[j].addr;
                        cstring_size = sec[j].size;
                    } else if (!strcmp(sec[j].sectname, "__os_log")) {
                        oslstring_base = sec[j].addr - kerndumpbase;
                        oslstring_size = sec[j].size - kerndumpbase;
                    }
                }
            }
            if (!strcmp(seg->segname, "__PRELINK_TEXT")) {
                const struct section_64 *sec = (struct section_64 *)(seg + 1);
                for (j = 0; j < seg->nsects; j++) {
                    if (!strcmp(sec[j].sectname, "__text")) {
                        pstring_base = sec[j].addr;
                        pstring_size = sec[j].size;
                    }
                }
            }
        }
        if (cmd->cmd == LC_UNIXTHREAD) {
            uint32_t *ptr = (uint32_t *)(cmd + 1);
            uint32_t flavor = ptr[0];
            struct {
                uint64_t x[29];	/* General purpose registers x0-x28 */
                uint64_t fp;	/* Frame pointer x29 */
                uint64_t lr;	/* Link register x30 */
                uint64_t sp;	/* Stack pointer x31 */
                uint64_t pc; 	/* Program counter */
                uint32_t cpsr;	/* Current program status register */
            } *thread = (void *)(ptr + 2);
            if (flavor == 6) {
                kernel_entry = thread->pc;
            }
        }
        q = q + cmd->cmdsize;
    }

    kerndumpbase = min;
    xnucore_base -= kerndumpbase;
    prelink_base -= kerndumpbase;
    cstring_base -= kerndumpbase;
    pstring_base -= kerndumpbase;
    oslstring_base -= kerndumpbase;
    kernel_size = max - min;

#ifdef __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__
    kernel = malloc(kernel_size);
    if (!kernel) {
        return -1;
    }
    rv = kread(kerndumpbase, kernel, kernel_size);
    if (rv != kernel_size) {
        free(kernel);
        return -1;
    }

    kernel_mh = kernel + kernel_base - min;

    (void)filename;
#undef close
#else	/* __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ */
    kernel = calloc(1, kernel_size);
    if (!kernel) {
        close(fd);
        return -1;
    }

    q = buf + sizeof(struct mach_header) + is64;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (struct segment_command_64 *)q;
            size_t sz = pread(fd, kernel + seg->vmaddr - min, seg->filesize, seg->fileoff);
            if (sz != seg->filesize) {
                close(fd);
                free(kernel);
                return -1;
            }
            if (!kernel_mh) {
                kernel_mh = kernel + seg->vmaddr - min;
            }
            if (!strcmp(seg->segname, "__LINKEDIT")) {
                kernel_delta = seg->vmaddr - min - seg->fileoff;
            }
        }
        q = q + cmd->cmdsize;
    }

    close(fd);

    (void)kernel_base;
#endif	/* __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ */
    return 0;
}

void
term_kernel(void)
{
    if (kernel != NULL) {
        free(kernel);
        kernel = NULL;
    }
}

addr_t
find_register_value(addr_t where, int reg)
{
    addr_t val;
    addr_t bof = 0;
    where -= kerndumpbase;
    if (where > xnucore_base) {
        bof = bof64(kernel, xnucore_base, where);
        if (!bof) {
            bof = xnucore_base;
        }
    } else if (where > prelink_base) {
        bof = bof64(kernel, prelink_base, where);
        if (!bof) {
            bof = prelink_base;
        }
    }
    val = calc64(kernel, bof, where, reg);
    if (!val) {
        return 0;
    }
    return val + kerndumpbase;
}

addr_t
find_reference(addr_t to, int n, int prelink)
{
    addr_t ref, end;
    addr_t base = xnucore_base;
    addr_t size = xnucore_size;
    if (prelink) {
        base = prelink_base;
        size = prelink_size;
    }
    if (n <= 0) {
        n = 1;
    }
    end = base + size;
    to -= kerndumpbase;
    do {
        ref = xref64(kernel, base, end, to);
        if (!ref) {
            return 0;
        }
        base = ref + 4;
    } while (--n > 0);
    return ref + kerndumpbase;
}

addr_t
find_strref(const char *string, int n, enum string_bases string_base)
{
    uint8_t *str;
    addr_t base;
    addr_t size;
    int prelink = 0;
    switch (string_base) {
        case string_base_oslstring:
            base = oslstring_base;
            size = oslstring_size;
            break;
        case string_base_pstring:
            base = pstring_base;
            size = pstring_size;
            prelink = 1;
            break;
        case string_base_cstring:
        default:
            base = cstring_base;
            size = cstring_size;
            break;
    }
    str = boyermoore_horspool_memmem(kernel + base, size, (uint8_t *)string, strlen(string));
    if (!str) {
        return 0;
    }
    return find_reference(str - kernel + kerndumpbase, n, prelink);
}

addr_t
find_gPhysBase(void)
{
    addr_t ret, val;
    addr_t ref = find_strref("\"pmap_map_high_window_bd: insufficient pages", true, false);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    ret = step64(kernel, ref, 64, INSN_RET);
    if (!ret) {
        // iOS 11
        ref = step64(kernel, ref, 1024, INSN_RET);
        if (!ref) {
            return 0;
        }
        ret = step64(kernel, ref + 4, 64, INSN_RET);
        if (!ret) {
            return 0;
        }
    }
    val = calc64(kernel, ref, ret, 8);
    if (!val) {
        return 0;
    }
    return val + kerndumpbase;
}

addr_t
find_kernel_pmap(void)
{
    addr_t call, bof, val;
    addr_t ref = find_strref("\"pmap_map_bd\"", true, false);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    call = step64_back(kernel, ref, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    bof = bof64(kernel, xnucore_base, call);
    if (!bof) {
        return 0;
    }
    val = calc64(kernel, bof, call, 2);
    if (!val) {
        return 0;
    }
    return val + kerndumpbase;
}

addr_t
find_amfiret(void)
{
    addr_t ret;
    addr_t ref = find_strref("AMFI: hook..execve() killing pid %u: %s\n", true, true);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    ret = step64(kernel, ref, 512, INSN_RET);
    if (!ret) {
        return 0;
    }
    return ret + kerndumpbase;
}

addr_t
find_ret_0(void)
{
    addr_t off;
    uint32_t *k;
    k = (uint32_t *)(kernel + xnucore_base);
    for (off = 0; off < xnucore_size - 4; off += 4, k++) {
        if (k[0] == 0xAA1F03E0 && k[1] == 0xD65F03C0) {
            return off + xnucore_base + kerndumpbase;
        }
    }
    k = (uint32_t *)(kernel + prelink_base);
    for (off = 0; off < prelink_size - 4; off += 4, k++) {
        if (k[0] == 0xAA1F03E0 && k[1] == 0xD65F03C0) {
            return off + prelink_base + kerndumpbase;
        }
    }
    return 0;
}

addr_t
find_amfi_memcmpstub(void)
{
    addr_t call, dest, reg;
    addr_t ref = find_strref("%s: Possible race detected. Rejecting.", true, true);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    call = step64_back(kernel, ref, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    dest = follow_call64(kernel, call);
    if (!dest) {
        return 0;
    }
    reg = calc64(kernel, dest, dest + 8, 16);
    if (!reg) {
        return 0;
    }
    return reg + kerndumpbase;
}

addr_t
find_sbops(void)
{
    addr_t off, what;
    uint8_t *str = boyermoore_horspool_memmem(kernel + pstring_base, pstring_size, (uint8_t *)"Seatbelt sandbox policy", sizeof("Seatbelt sandbox policy") - 1);
    if (!str) {
        return 0;
    }
    what = str - kernel + kerndumpbase;
    for (off = 0; off < kernel_size - prelink_base; off += 8) {
        if (*(uint64_t *)(kernel + prelink_base + off) == what) {
            return *(uint64_t *)(kernel + prelink_base + off + 24);
        }
    }
    return 0;
}

addr_t
find_lwvm_mapio_patch(void)
{
    addr_t call, dest, reg;
    addr_t ref = find_strref("_mapForIO", true, true);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    call = step64(kernel, ref, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    call = step64(kernel, call + 4, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    dest = follow_call64(kernel, call);
    if (!dest) {
        return 0;
    }
    reg = calc64(kernel, dest, dest + 8, 16);
    if (!reg) {
        return 0;
    }
    return reg + kerndumpbase;
}

addr_t
find_lwvm_mapio_newj(void)
{
    addr_t call;
    addr_t ref = find_strref("_mapForIO", true, true);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    call = step64(kernel, ref, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    call = step64(kernel, call + 4, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    call = step64(kernel, call + 4, 64, INSN_CALL);
    if (!call) {
        return 0;
    }
    call = step64_back(kernel, call, 64, INSN_B);
    if (!call) {
        return 0;
    }
    return call + 4 + kerndumpbase;
}

addr_t
find_cpacr_write(void)
{
    addr_t off;
    uint32_t *k;
    k = (uint32_t *)(kernel + xnucore_base);
    for (off = 0; off < xnucore_size - 4; off += 4, k++) {
        if (k[0] == 0xd5181040) {
            return off + xnucore_base + kerndumpbase;
        }
    }
    return 0;
}

addr_t
find_str(const char *string)
{
    uint8_t *str = boyermoore_horspool_memmem(kernel, kernel_size, (uint8_t *)string, strlen(string));
    if (!str) {
        return 0;
    }
    return str - kernel + kerndumpbase;
}

addr_t
find_entry(void)
{
    /* XXX returns an unslid address */
    return kernel_entry;
}

const unsigned char *
find_mh(void)
{
    return kernel_mh;
}

addr_t
find_amfiops(void)
{
    addr_t off, what;
    uint8_t *str = boyermoore_horspool_memmem(kernel + pstring_base, pstring_size, (uint8_t *)"Apple Mobile File Integrity", sizeof("Apple Mobile File Integrity") - 1);
    if (!str) {
        return 0;
    }
    what = str - kernel + kerndumpbase;
    /* XXX will only work on a dumped kernel */
    for (off = 0; off < kernel_size - prelink_base; off += 8) {
        if (*(uint64_t *)(kernel + prelink_base + off) == what) {
            return *(uint64_t *)(kernel + prelink_base + off + 0x18);
        }
    }
    return 0;
}

addr_t
find_sysbootnonce(void)
{
    addr_t off, what;
    uint8_t *str = boyermoore_horspool_memmem(kernel + cstring_base, cstring_size, (uint8_t *)"com.apple.System.boot-nonce", sizeof("com.apple.System.boot-nonce") - 1);
    if (!str) {
        return 0;
    }
    what = str - kernel + kerndumpbase;
    for (off = 0; off < kernel_size - xnucore_base; off += 8) {
        if (*(uint64_t *)(kernel + xnucore_base + off) == what) {
            return xnucore_base + off + 8 + 4 + kerndumpbase;
        }
    }
    return 0;
}

addr_t
find_trustcache(void)
{
    addr_t cbz, call, func, val;
    addr_t ref = find_strref("amfi_prevent_old_entitled_platform_binaries", true, true);
    if (!ref) {
        // iOS 11
        ref = find_strref("com.apple.MobileFileIntegrity", false, true);
        if (!ref) {
            return 0;
        }
        ref -= kerndumpbase;
        call = step64(kernel, ref, 64, INSN_CALL);
        if (!call) {
            return 0;
        }
        call = step64(kernel, call + 4, 64, INSN_CALL);
        goto okay;
    }
    ref -= kerndumpbase;
    cbz = step64(kernel, ref, 32, INSN_CBZ);
    if (!cbz) {
        return 0;
    }
    call = step64(kernel, follow_cbz(kernel, cbz), 4, INSN_CALL);
okay:
    if (!call) {
        return 0;
    }
    func = follow_call64(kernel, call);
    if (!func) {
        return 0;
    }
    val = calc64(kernel, func, func + 16, 8);
    if (!val) {
        ref = find_strref("%s: only allowed process can check the trust cache", true, true); // Trying to find AppleMobileFileIntegrityUserClient::isCdhashInTrustCache
        if (!ref) {
            return 0;
        }
        ref -= kerndumpbase;
        call = step64_back(kernel, ref, 11 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        func = follow_call64(kernel, call);
        if (!func) {
            return 0;
        }
        call = step64(kernel, func, 8 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        func = follow_call64(kernel, call);
        if (!func) {
            return 0;
        }
        call = step64(kernel, func, 8 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        call = step64(kernel, call + 4, 8 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        func = follow_call64(kernel, call);
        if (!func) {
            return 0;
        }
        call = step64(kernel, func, 12 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        
        val = calc64(kernel, call, call + 6 * 4, 21);
    }
    return val + kerndumpbase;
}

addr_t
find_amficache(void)
{
    addr_t cbz, call, func, val;
    addr_t ref = find_strref("amfi_prevent_old_entitled_platform_binaries", true, true);
    if (!ref) {
        // iOS 11
        ref = find_strref("com.apple.MobileFileIntegrity", false, true);
        if (!ref) {
            return 0;
        }
        ref -= kerndumpbase;
        call = step64(kernel, ref, 64, INSN_CALL);
        if (!call) {
            return 0;
        }
        call = step64(kernel, call + 4, 64, INSN_CALL);
        goto okay;
    }
    ref -= kerndumpbase;
    cbz = step64(kernel, ref, 32, INSN_CBZ);
    if (!cbz) {
        return 0;
    }
    call = step64(kernel, follow_cbz(kernel, cbz), 4, INSN_CALL);
okay:
    if (!call) {
        return 0;
    }
    func = follow_call64(kernel, call);
    if (!func) {
        return 0;
    }
    val = calc64(kernel, func, func + 16, 8);
    if (!val) {
        ref = find_strref("%s: only allowed process can check the trust cache", true, true); // Trying to find AppleMobileFileIntegrityUserClient::isCdhashInTrustCache
        if (!ref) {
            return 0;
        }
        ref -= kerndumpbase;
        call = step64_back(kernel, ref, 11 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        func = follow_call64(kernel, call);
        if (!func) {
            return 0;
        }
        call = step64(kernel, func, 8 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        func = follow_call64(kernel, call);
        if (!func) {
            return 0;
        }
        call = step64(kernel, func, 8 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        call = step64(kernel, call + 4, 8 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        func = follow_call64(kernel, call);
        if (!func) {
            return 0;
        }
        call = step64(kernel, func, 12 * 4, INSN_CALL);
        if (!call) {
            return 0;
        }
        
        val = calc64(kernel, call, call + 6 * 4, 21);
    }
    return val + kerndumpbase;
}

/* extra_recipe **************************************************************/

#define INSN_STR8 0xF9000000 | 8, 0xFFC00000 | 0x1F
#define INSN_POPS 0xA9407BFD, 0xFFC07FFF

addr_t
find_AGXCommandQueue_vtable(void)
{
    addr_t val, str8;
    addr_t ref = find_strref("AGXCommandQueue", true, true);
    if (!ref) {
        return 0;
    }
    val = find_register_value(ref, 0);
    if (!val) {
        return 0;
    }
    ref = find_reference(val, 1, 1);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    str8 = step64(kernel, ref, 32, INSN_STR8);
    if (!str8) {
        return 0;
    }
    val = calc64(kernel, ref, str8, 8);
    if (!val) {
        return 0;
    }
    return val + kerndumpbase;
}

addr_t
find_allproc(void)
{
    addr_t val, bof, str8;
    addr_t ref = find_strref("\"pgrp_add : pgrp is dead adding process\"", true, false);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    bof = bof64(kernel, xnucore_base, ref);
    if (!bof) {
        return 0;
    }
    str8 = step64_back(kernel, ref, ref - bof, INSN_STR8);
    if (!str8) {
        // iOS 11
        addr_t ldp = step64(kernel, ref, 1024, INSN_POPS);
        if (!ldp) {
            return 0;
        }
        str8 = step64_back(kernel, ldp, ldp - bof, INSN_STR8);
        if (!str8) {
            return 0;
        }
    }
    val = calc64(kernel, bof, str8, 8);
    if (!val) {
        return 0;
    }
    return val + kerndumpbase;
}

addr_t
find_call5(void)
{
    addr_t bof;
    uint8_t gadget[] = { 0x95, 0x5A, 0x40, 0xF9, 0x68, 0x02, 0x40, 0xF9, 0x88, 0x5A, 0x00, 0xF9, 0x60, 0xA2, 0x40, 0xA9 };
    uint8_t *str = boyermoore_horspool_memmem(kernel + prelink_base, prelink_size, gadget, sizeof(gadget));
    if (!str) {
        return 0;
    }
    bof = bof64(kernel, prelink_base, str - kernel);
    if (!bof) {
        return 0;
    }
    return bof + kerndumpbase;
}

addr_t
find_realhost(addr_t priv)
{
    addr_t val;
    if (!priv) {
        return 0;
    }
    priv -= kerndumpbase;
    val = calc64(kernel, priv, priv + 12, 0);
    if (!val) {
        return 0;
    }
    return val + kerndumpbase;
}

/*
 *
 * @ninjaprawn's patches
 *
 */ 

addr_t find_vfs_context_current(void) {
    addr_t error_str = find_strref("\"vnode_put(%p): iocount < 1\"", true, false);
    
    if (!error_str) {
        return 0;
    }
    
    error_str -= kerndumpbase;

    addr_t call_to_target = step64_back(kernel, error_str, 10*4, INSN_CALL);
    
    if (!call_to_target) {
        return 0;
    }
    
    addr_t offset_to_target = follow_call64(kernel, call_to_target);
    
    if (!offset_to_target) {
        return 0;
    }

    return offset_to_target + kerndumpbase;
}

addr_t find_vnode_lookup(void) {
    addr_t hfs_str = find_strref("hfs: journal open cb: error %d looking up device %s (dev uuid %s)\n", true, true);
    
    if (!hfs_str) {
        return 0;
    }
    
    hfs_str -= kerndumpbase;

    addr_t call_to_stub = step64_back(kernel, hfs_str, 10*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }
    
    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}

addr_t find_vnode_put(void) {
    addr_t err_str = find_strref("KBY: getparent(%p) != parent_vp(%p)", true, true);
    if (!err_str)
	    err_str = find_strref("getparent(%p) != parent_vp(%p)", true, true);
    
    if (!err_str) {
        return 0;
    }

    err_str -= kerndumpbase;

    addr_t call_to_os_log = step64(kernel, err_str, 20*4, INSN_CALL);
    
    if (!call_to_os_log) {
        return 0;
    }
    
    addr_t call_to_vn_getpath = step64(kernel, call_to_os_log + 4, 20*4, INSN_CALL);
    
    if (!call_to_vn_getpath) {
        return 0;
    }
    
    addr_t call_to_stub = step64(kernel, call_to_vn_getpath + 4, 20*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }

    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}

addr_t find_vnode_getfromfd(void) {
    addr_t call1, call2, call3, call4, call5, call6, call7;
    addr_t func1;

    addr_t ent_str = find_strref("rootless_storage_class_entitlement", true, true);
    
    if (!ent_str) {
        return 0;
    }
    
    ent_str -= kerndumpbase;

    addr_t call_to_unk1 = step64(kernel, ent_str, 20*4, INSN_CALL);
    
    if (!call_to_unk1) {
        return 0;
    }
    
    addr_t call_to_strlcpy = step64(kernel, call_to_unk1 + 4, 20*4, INSN_CALL);
    
    if (!call_to_strlcpy) {
        return 0;
    }
    
    addr_t call_to_strlcat = step64(kernel, call_to_strlcpy + 4, 20*4, INSN_CALL);
    
    if (!call_to_strlcat) {
        return 0;
    }
    
    addr_t call_to_unk2 = step64(kernel, call_to_strlcat + 4, 20*4, INSN_CALL);
    
    if (!call_to_unk2) {
        return 0;
    }
    
    addr_t call_to_unk3 = step64(kernel, call_to_unk2 + 4, 20*4, INSN_CALL);
    
    if (!call_to_unk3) {
        return 0;
    }
    
    addr_t call_to_vfs_context_create = step64(kernel, call_to_unk3 + 4, 20*4, INSN_CALL);
    
    if (!call_to_vfs_context_create) {
        return 0;
    }
    
    addr_t call_to_stub = step64(kernel, call_to_vfs_context_create + 4, 20*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }

    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}

addr_t find_vnode_getattr(void) {
    addr_t error_str = find_strref("\"add_fsevent: you can't pass me a NULL vnode ptr (type %d)!\\n\"", true, false);
    
    if (!error_str) {
        return 0;
    }
    
    error_str -= kerndumpbase;
    error_str += 12; // Jump over the panic call

    addr_t call_to_target = step64(kernel, error_str, 30*4, INSN_CALL);
    
    if (!call_to_target) {
        return 0;
    }
    
    addr_t offset_to_target = follow_call64(kernel, call_to_target);
    
    if (!offset_to_target) {
        return 0;
    }

    return offset_to_target + kerndumpbase;
}

addr_t find_SHA1Init(void) {
    addr_t id_str = find_strref("CrashReporter-ID", true, true);
    
    if (!id_str) {
        return 0;
    }
    
    id_str -= kerndumpbase;

    addr_t call_to_hash_function = step64(kernel, id_str, 10*4, INSN_CALL);
    
    if (!call_to_hash_function) {
        return 0;
    }
    
    addr_t hash_function = follow_call64(kernel, call_to_hash_function);
    
    if (!hash_function) {
        return 0;
    }
    
    addr_t call_to_stub = step64(kernel, hash_function, 20*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }

    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}

addr_t find_SHA1Update(void) {
    addr_t id_str = find_strref("CrashReporter-ID", true, true);
    id_str -= kerndumpbase;

    addr_t call_to_hash_function = step64(kernel, id_str, 10*4, INSN_CALL);
    
    if (!call_to_hash_function) {
        return 0;
    }
    
    addr_t hash_function = follow_call64(kernel, call_to_hash_function);
    
    if (!hash_function) {
        return 0;
    }
    
    addr_t call_to_sha1init = step64(kernel, hash_function, 20*4, INSN_CALL);
    
    if (!call_to_sha1init) {
        return 0;
    }
    
    addr_t call_to_stub = step64(kernel, call_to_sha1init + 4, 20*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }

    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}


addr_t find_SHA1Final(void) {
    addr_t id_str = find_strref("CrashReporter-ID", true, true);
    
    if (!id_str) {
        return 0;
    }
    
    id_str -= kerndumpbase;

    addr_t call_to_hash_function = step64(kernel, id_str, 10*4, INSN_CALL);
    
    if (!call_to_hash_function) {
        return 0;
    }
    
    addr_t hash_function = follow_call64(kernel, call_to_hash_function);
    
    if (!hash_function) {
        return 0;
    }
    
    addr_t call_to_sha1init = step64(kernel, hash_function, 20*4, INSN_CALL);
    
    if (!call_to_sha1init) {
        return 0;
    }
    
    addr_t call_to_sha1update = step64(kernel, call_to_sha1init + 4, 20*4, INSN_CALL);
    
    if (!call_to_sha1update) {
        return 0;
    }
    
    addr_t call_to_stub = step64(kernel, call_to_sha1update + 4, 20*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }

    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}

addr_t find_csblob_entitlements_dictionary_set(void) {
    addr_t ent_str = find_strref("entitlements are not a dictionary", true, true);
    
    if (!ent_str) {
        return 0;
    }
    
    ent_str -= kerndumpbase;

    addr_t call_to_lck_mtx_lock = step64(kernel, ent_str, 20*4, INSN_CALL);
    
    if (!call_to_lck_mtx_lock) {
        return 0;
    }
    
    addr_t call_to_csblob_entitlements_dictionary_copy = step64(kernel, call_to_lck_mtx_lock + 4, 20*4, INSN_CALL);
    
    if (!call_to_csblob_entitlements_dictionary_copy) {
        return 0;
    }
    
    addr_t call_to_stub = step64(kernel, call_to_csblob_entitlements_dictionary_copy + 4, 20*4, INSN_CALL);
    
    if (!call_to_stub) {
        return 0;
    }

    addr_t stub_function = follow_call64(kernel, call_to_stub);
    
    if (!stub_function) {
        return 0;
    }
    
    addr_t target_function_offset = calc64(kernel, stub_function, stub_function+12, 16);
    
    if (!target_function_offset) {
        return 0;
    }
    
    addr_t target_function = *(addr_t*)(kernel+target_function_offset);
    
    if (!target_function) {
        return 0;
    }

    return target_function;
}

addr_t find_kernel_task(void) {
    addr_t term_str = find_strref("\"thread_terminate\"", true, false);
    
    if (!term_str) {
        return 0;
    }
    
    term_str -= kerndumpbase;

    addr_t thread_terminate = bof64(kernel, xnucore_base, term_str);
    
    if (!thread_terminate) {
        return 0;
    }
    
    addr_t call_to_unk1 = step64(kernel, thread_terminate, 20*4, INSN_CALL);
    
    if (!call_to_unk1) {
        return 0;
    }

    addr_t kern_task = calc64(kernel, thread_terminate, call_to_unk1, 9);
    
    if (!kern_task) {
        return 0;
    }
    
    return kern_task + kerndumpbase;
}


addr_t find_kernproc(void) {
    addr_t ret_str = find_strref("\"returning child proc which is not cur_act\"", true, false);
    
    if (!ret_str) {
        return 0;
    }
    
    ret_str -= kerndumpbase;

    addr_t end_of_function = step64(kernel, ret_str, 20*4, INSN_RET);
    
    if (!end_of_function) {
        return 0;
    }

    addr_t kernproc = calc64(kernel, ret_str, end_of_function, 19);
    
    if (!kernproc) {
        return 0;
    }
    
    return kernproc + kerndumpbase;
}

addr_t find_vnode_recycle(void) {
    addr_t error_str = find_strref("\"vnode_put(%p): iocount < 1\"", true, false);
    
    if (!error_str) {
        return 0;
    }
    
    error_str -= kerndumpbase;
    
    addr_t call_to_lck_mtx_unlock = step64(kernel, error_str + 4, 40*4, INSN_CALL);
    
    if (!call_to_lck_mtx_unlock) {
        return 0;
    }
    
    addr_t call_to_unknown1 = step64(kernel, call_to_lck_mtx_unlock + 4, 40*4, INSN_CALL);
    
    if (!call_to_unknown1) {
        return 0;
    }
    
    addr_t offset_to_unknown1 = follow_call64(kernel, call_to_unknown1);
    
    if (!offset_to_unknown1) {
        return 0;
    }
    
    addr_t call_to_target = step64(kernel, offset_to_unknown1 + 4, 40*4, INSN_CALL);
    
    if (!call_to_target) {
        return 0;
    }
    
    addr_t offset_to_target = follow_call64(kernel, call_to_target);
    
    if (!offset_to_target) {
        return 0;
    }

    return offset_to_target + kerndumpbase;
}

addr_t find_lck_mtx_lock(void) {
    addr_t details_str = find_strref("Details", true, false);
    
    if (!details_str) {
        return 0;
    }
    
    details_str -= kerndumpbase;

    addr_t call_to_target = step64(kernel, details_str + 4, 40*4, INSN_CALL);
    
    if (!call_to_target) {
        return 0;
    }
    
    addr_t offset_to_target = follow_call64(kernel, call_to_target);
    
    if (!offset_to_target) {
        return 0;
    }

    return offset_to_target + kerndumpbase;
}

addr_t find_lck_mtx_unlock(void) {
    addr_t details_str = find_strref("Details", true, false);
    
    if (!details_str) {
        return 0;
    }
    
    details_str -= kerndumpbase;

    addr_t call_to_lck_mtx_lock = step64(kernel, details_str + 4, 40*4, INSN_CALL);
    
    if (!call_to_lck_mtx_lock) {
        return 0;
    }
    
    addr_t call_to_sysctl_register_oid = step64(kernel, call_to_lck_mtx_lock+4, 40*4, INSN_CALL);
    
    if (!call_to_sysctl_register_oid) {
        return 0;
    }
    
    addr_t call_to_strlcat1 = step64(kernel, call_to_sysctl_register_oid + 4, 40*4, INSN_CALL);
    
    if (!call_to_strlcat1) {
        return 0;
    }
    
    addr_t call_to_strlcat2 = step64(kernel, call_to_strlcat1+4, 40*4, INSN_CALL);

    if (!call_to_strlcat2) {
        return 0;
    }
    
    addr_t call_to_target = step64(kernel, call_to_strlcat2 + 4, 40*4, INSN_CALL);
    
    if (!call_to_target) {
        return 0;
    }
    
    addr_t offset_to_target = follow_call64(kernel, call_to_target);
    
    if (!offset_to_target) {
        return 0;
    }

    return offset_to_target + kerndumpbase;
}

addr_t find_strlen(void) {
    addr_t xnu_str = find_strref("AP-xnu", true, false);
    
    if (!xnu_str) {
        return 0;
    }
    
    xnu_str -= kerndumpbase;

    addr_t call_to_target = step64(kernel, xnu_str, 40*4, INSN_CALL);
    
    if (!call_to_target) {
        return 0;
    }
    
    addr_t offset_to_target = follow_call64(kernel, call_to_target);
    
    if (!offset_to_target) {
        return 0;
    }

    return offset_to_target + kerndumpbase;
}

addr_t find_add_x0_x0_0x40_ret(void)
{
    addr_t off;
    uint32_t* k;
    k = (uint32_t*)(kernel + xnucore_base);
    for (off = 0; off < xnucore_size - 4; off += 4, k++) {
        if (k[0] == 0x91010000 && k[1] == 0xD65F03C0) {
            return off + xnucore_base + kerndumpbase;
        }
    }
    k = (uint32_t*)(kernel + prelink_base);
    for (off = 0; off < prelink_size - 4; off += 4, k++) {
        if (k[0] == 0x91010000 && k[1] == 0xD65F03C0) {
            return off + prelink_base + kerndumpbase;
        }
    }
    return 0;
}

/*
 *
 * 
 *
 */

/*
 *
 * @Cryptiiiic's patches
 *
 */ 

addr_t find_boottime(void) {
    addr_t ref = find_strref("%s WARNING: PMU offset is less then sys PMU", 1, string_base_oslstring);
    
    if (!ref) {
        return 0;
    }
    
    ref -= kerndumpbase;

    // ADRP Xm, #_boottime@PAGE
    ref = step64(kernel, ref, 0x4D, INSN_ADRP);
    
    if (!ref) {
        return 0;
    }

    // pc base
    uint64_t val = kerndumpbase;

    uint32_t* insn = (uint32_t*)(kernel + ref);
    // add pc (well, offset)
    val += ((uint8_t*)(insn)-kernel) & ~0xfff;
    uint8_t xm = *insn & 0x1f;

    // add imm: immhi(bits 23-5)|immlo(bits 30-29)
    val += (*insn << 9 & 0x1ffffc000) | (*insn >> 17 & 0x3000);

    ++insn;
    // STR Xn, [Xm,_boottime@PAGEOFF]
    if ((*insn & 0xF9090000) != 0xF9000000) {
        return 0;
    }
    if (((*insn >> 5) & 0x1f) != xm) {
        return 0;
    }
    // add pageoff
    val += ((*insn >> 10) & 0xFFF) << 3;
    uint8_t xn = (*insn & 0x1f);

    return val;
}

/*
 *
 * 
 *
 */

/*
 *
 * @stek29's patches
 *
 */

addr_t find_zone_map_ref(void)
{
    // \"Nothing being freed to the zone_map. start = end = %p\\n\"
    uint64_t val = kerndumpbase;
    
    addr_t ref = find_strref("\"Nothing being freed to the zone_map. start = end = %p\\n\"", true, false);
    
    if (!ref) {
        return 0;
    }
    
    ref -= kerndumpbase;
    
    // skip add & adrp for panic str
    ref -= 8;
    
    // adrp xX, #_zone_map@PAGE
    ref = step64_back(kernel, ref, 30, INSN_ADRP);
    
    if (!ref) {
        return 0;
    }
    
    uint32_t* insn = (uint32_t*)(kernel + ref);
    // get pc
    val += ((uint8_t*)(insn)-kernel) & ~0xfff;
    uint8_t xm = *insn & 0x1f;
    
    // don't ask, I wrote this at 5am
    val += (*insn << 9 & 0x1ffffc000) | (*insn >> 17 & 0x3000);
    
    // ldr x, [xX, #_zone_map@PAGEOFF]
    ++insn;
    if ((*insn & 0xF9C00000) != 0xF9400000) {
        return 0;
    }
    
    // xd == xX, xn == xX,
    if ((*insn & 0x1f) != xm || ((*insn >> 5) & 0x1f) != xm) {
        return 0;
    }
    
    val += ((*insn >> 10) & 0xFFF) << 3;
    
    return val;
}

addr_t find_OSBoolean_True(void)
{
    addr_t val;
    addr_t ref = find_strref("Delay Autounload", false, false);
    if (!ref) {
        return 0;
    }
    ref -= kerndumpbase;
    
    addr_t weird_instruction = 0;
    for (int i = 4; i < 4 * 0x100; i += 4) {
        uint32_t op = *(uint32_t*)(kernel + ref + i);
        if (op == 0x320003E0) {
            weird_instruction = ref + i;
            break;
        }
    }
    if (!weird_instruction) {
        return 0;
    }
    
    val = calc64(kernel, ref, weird_instruction, 8);
    if (!val) {
        return 0;
    }
    
    return val + kerndumpbase;
}

addr_t find_osunserializexml(void)
{
    addr_t ref = find_strref("OSUnserializeXML: %s near line %d\n", true, false);
    
    if (!ref) {
        return 0;
    }
    
    ref -= kerndumpbase;
    
    uint64_t start = bof64(kernel, xnucore_base, ref);
    
    if (!start) {
        return 0;
    }
    
    return start + kerndumpbase;
}

addr_t find_smalloc(void)
{
    addr_t ref = find_strref("sandbox memory allocation failure", true, true);
    
    if (!ref) {
        return 0;
    }
    
    ref -= kerndumpbase;
    
    uint64_t start = bof64(kernel, prelink_base, ref);
    
    if (!start) {
        return 0;
    }

    return start + kerndumpbase;
}

addr_t find_shenanigans(void)
{
    addr_t ref = find_strref("\"shenanigans!", true, true);
    
    if (!ref) {
        return 0;
    }
    
    ref -= kerndumpbase;
    
    // find sb_evaluate
    ref = bof64(kernel, prelink_base, ref);
    
    if (!ref) {
        return 0;
    }
    
    // ADRP Xm, #_is_kernel_cred_kerncred@PAGE
    ref = step64(kernel, ref, 0x100, INSN_ADRP);
    
    if (!ref) {
        return 0;
    }
    
    // pc base
    uint64_t val = kerndumpbase;
    
    uint32_t* insn = (uint32_t*)(kernel + ref);
    // add pc (well, offset)
    val += ((uint8_t*)(insn)-kernel) & ~0xfff;
    uint8_t xm = *insn & 0x1f;
    
    // add imm: immhi(bits 23-5)|immlo(bits 30-29)
    val += (*insn << 9 & 0x1ffffc000) | (*insn >> 17 & 0x3000);
    
    ++insn;
    // LDR Xn, [Xm,#_is_kernel_cred_kerncred@PAGEOFF]
    if ((*insn & 0xF9C00000) != 0xF9400000) {
        return 0;
    }
    if (((*insn >> 5) & 0x1f) != xm) {
        return 0;
    }
    // add pageoff
    val += ((*insn >> 10) & 0xFFF) << 3;
    uint8_t xn = (*insn & 0x1f);
    
    ++insn;
    // CBNZ Xn, ...
    if ((*insn & 0xFC000000) != 0xB4000000) {
        return 0;
    }
    if ((*insn & 0x1f) != xn) {
        return 0;
    }
    
    return val;
}

/*
 *
 *
 *
 */

#ifdef HAVE_MAIN

#ifndef NOT_DARWIN
#include <mach-o/nlist.h>
#else
#include "mach-o_nlist.h"
#endif


addr_t
find_symbol(const char *symbol)
{
    if (!symbol) {
        return 0;
    }
    
    unsigned i;
    const struct mach_header *hdr = kernel_mh;
    const uint8_t *q;
    int is64 = 0;

    if (IS64(hdr)) {
        is64 = 4;
    }

/* XXX will only work on a decrypted kernel */
    if (!kernel_delta) {
        return 0;
    }

    /* XXX I should cache these.  ohwell... */
    q = (uint8_t *)(hdr + 1) + is64;
    for (i = 0; i < hdr->ncmds; i++) {
        const struct load_command *cmd = (struct load_command *)q;
        if (cmd->cmd == LC_SYMTAB) {
            const struct symtab_command *sym = (struct symtab_command *)q;
            const char *stroff = (const char *)kernel + sym->stroff + kernel_delta;
            if (is64) {
                uint32_t k;
                const struct nlist_64 *s = (struct nlist_64 *)(kernel + sym->symoff + kernel_delta);
                for (k = 0; k < sym->nsyms; k++) {
                    if (s[k].n_type & N_STAB) {
                        continue;
                    }
                    if (s[k].n_value && (s[k].n_type & N_TYPE) != N_INDR) {
                        if (!strcmp(symbol, stroff + s[k].n_un.n_strx)) {
                            /* XXX this is an unslid address */
                            return s[k].n_value;
                        }
                    }
                }
            }
        }
        q = q + cmd->cmdsize;
    }
    return 0;
}

/* test **********************************************************************/

int
main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: patchfinder64 _decompressed_kernel_image_\n");
        printf("iOS ARM64 kernel patchfinder\n");
        exit(EXIT_FAILURE);
    }
    if (access(argv[1], F_OK) != 0) {
        printf("%s: %s\n", argv[1], strerror(errno));
        exit(EXIT_FAILURE);
    }
    int rv;
    addr_t kernel_base = 0;
    const addr_t vm_kernel_slide = 0;
    if (init_kernel(NULL, kernel_base, argv[1]) != 0) {
        printf("Failed to prepare kernel\n");
        exit(EXIT_FAILURE);
    }

#define CHECK(name, symbol_name) do { \
    addr_t patchfinder_offset = find_ ##name (); \
    addr_t actual_offset = find_symbol(symbol_name); \
    printf("%s: PF=0x%llx - AS=0x%llx - %s\n", symbol_name, patchfinder_offset, actual_offset, (patchfinder_offset == actual_offset ? "PASS" : "FAIL")); \
} while(false)
#define FIND(name) do { \
    addr_t patchfinder_offset = find_ ##name (); \
    printf("%s: PF=0x%llx - %s\n", #name, patchfinder_offset, (patchfinder_offset != 0 ? "PASS" : "FAIL")); \
} while(false)
    
    CHECK(vfs_context_current, "_vfs_context_current");
    CHECK(vnode_lookup, "_vnode_lookup");
    CHECK(vnode_put, "_vnode_put");
    CHECK(vnode_getfromfd, "_vnode_getfromfd");
    CHECK(vnode_getattr, "_vnode_getattr");
    CHECK(SHA1Init, "_SHA1Init");
    CHECK(SHA1Update, "_SHA1Update");
    CHECK(SHA1Final, "_SHA1Final");
    CHECK(csblob_entitlements_dictionary_set, "_csblob_entitlements_dictionary_set");
    CHECK(kernel_task, "_kernel_task");
    CHECK(kernproc, "_kernproc");
    CHECK(vnode_recycle, "_vnode_recycle");
    CHECK(lck_mtx_lock, "_lck_mtx_lock");
    CHECK(lck_mtx_unlock, "_lck_mtx_unlock");
    CHECK(strlen, "_strlen");
    FIND(add_x0_x0_0x40_ret);
    FIND(trustcache);
    FIND(boottime);
    FIND(zone_map_ref);
    FIND(OSBoolean_True);
    FIND(osunserializexml);
    FIND(smalloc);
    FIND(shenanigans);

    term_kernel();
    return EXIT_SUCCESS;
}

#endif	/* HAVE_MAIN */
