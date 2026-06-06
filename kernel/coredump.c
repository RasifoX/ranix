/*
 * ELF core dump. On a core-generating fatal signal the crashing process's
 * register state and memory are written to an ET_CORE file (core_pattern,
 * default "core" in the cwd) before task_exit. The layout is standard enough
 * for a host gdb to open: a PT_NOTE (NT_PRSTATUS registers + NT_PRPSINFO name)
 * followed by one PT_LOAD per VMA. Unmapped pages within a VMA are written as
 * zeros so file offsets stay consistent; the total is capped to bound runaway
 * dumps of huge mappings.
 */
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/hal.h>
#include <asm/context.h>

char core_pattern[128] = "core";

#define ET_CORE     4
#define PT_LOAD     1
#define PT_NOTE     4
#define NT_PRSTATUS 1
#define NT_PRPSINFO 3
#define PF_X 1
#define PF_W 2
#define PF_R 4

#if defined(__x86_64__)
#define CORE_EM      62      /* EM_X86_64 */
#define ELF_NGREG    27
#elif defined(__aarch64__)
#define CORE_EM      183     /* EM_AARCH64 */
#define ELF_NGREG    34
#endif

#define CORE_MAX_BYTES (8u * 1024 * 1024)
#define PRSTATUS_REG_OFF 112                       /* pr_reg offset in elf_prstatus */
#define PRSTATUS_SIZE   (PRSTATUS_REG_OFF + ELF_NGREG * 8 + 8)
#define PRPSINFO_SIZE   136

typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

static int core_signal(int sig)
{
	switch (sig) {
	case 3:  /* SIGQUIT */
	case 4:  /* SIGILL  */
	case 5:  /* SIGTRAP */
	case 6:  /* SIGABRT */
	case 7:  /* SIGBUS  */
	case 8:  /* SIGFPE  */
	case 11: /* SIGSEGV */
		return 1;
	default:
		return 0;
	}
}

static void fill_pr_reg(uint64_t *r, arch_context_t *c)
{
#if defined(__x86_64__)
	/* user_regs_struct order */
	r[0]=c->r15; r[1]=c->r14; r[2]=c->r13; r[3]=c->r12; r[4]=c->rbp; r[5]=c->rbx;
	r[6]=c->r11; r[7]=c->r10; r[8]=c->r9;  r[9]=c->r8;  r[10]=c->rax; r[11]=c->rcx;
	r[12]=c->rdx; r[13]=c->rsi; r[14]=c->rdi; r[15]=c->rax; /* orig_rax */
	r[16]=c->rip; r[17]=c->cs; r[18]=c->rflags; r[19]=c->rsp; r[20]=c->ss;
	/* r[21..26] fs_base/gs_base/ds/es/fs/gs left zero */
#elif defined(__aarch64__)
	for (int i = 0; i < 31; i++) r[i] = c->regs[i];
	r[31] = c->sp; r[32] = c->elr; r[33] = c->spsr;
#endif
}

/* write exactly len bytes from kbuf to fd; returns 0 on success */
static int core_write(int fd, const void *kbuf, size_t len)
{
	const uint8_t *p = kbuf;
	while (len) {
		ssize_t w = vfs_write(fd, p, len);
		if (w <= 0) return -1;
		p += w; len -= (size_t)w;
	}
	return 0;
}

void coredump(process_t *proc, int sig, arch_context_t *tf)
{
	if (!proc || !core_signal(sig) || core_pattern[0] == '\0') return;

	/* count + total of dumpable VMAs (capped) */
	int nv = 0;
	uint64_t total = 0;
	for (vma_t *v = proc->vm->vmas; v; v = v->next) {
		if (v->size == 0) continue;
		if (total + v->size > CORE_MAX_BYTES) break;
		total += v->size; nv++;
	}

	char path[256];
	if (core_pattern[0] == '/') {
		size_t n = strlen(core_pattern);
		if (n >= sizeof(path)) return;
		memcpy(path, core_pattern, n + 1);
	} else {
		const char *cwd = proc->cwd[0] ? proc->cwd : "/";
		int n = 0;
		for (const char *s = cwd; *s && n < 200; s++) path[n++] = *s;
		if (n == 0 || path[n - 1] != '/') path[n++] = '/';
		for (const char *s = core_pattern; *s && n < 250; s++) path[n++] = *s;
		path[n] = '\0';
	}

	int fd = vfs_open(path, 0x41 /*O_CREAT|O_WRONLY*/ | 0x200 /*O_TRUNC*/, 0600);
	if (fd < 0) return;

	int phnum = 1 + nv;  /* PT_NOTE + PT_LOADs */
	uint64_t phoff = sizeof(Elf64_Ehdr);
	uint64_t notesz = (8 + 4 + 4 + PRSTATUS_SIZE) + (8 + 4 + 4 + PRPSINFO_SIZE);
	/* note name "CORE" padded: namesz=5 → 8 bytes; descs padded to 4 (already aligned) */
	uint64_t data_off = phoff + (uint64_t)phnum * sizeof(Elf64_Phdr);
	uint64_t note_off = data_off;
	uint64_t load_off = note_off + notesz;

	Elf64_Ehdr eh; memset(&eh, 0, sizeof(eh));
	eh.e_ident[0]=0x7f; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
	eh.e_ident[4]=2; /* ELFCLASS64 */ eh.e_ident[5]=1; /* LSB */ eh.e_ident[6]=1;
	eh.e_type=ET_CORE; eh.e_machine=CORE_EM; eh.e_version=1;
	eh.e_phoff=phoff; eh.e_ehsize=sizeof(Elf64_Ehdr);
	eh.e_phentsize=sizeof(Elf64_Phdr); eh.e_phnum=(uint16_t)phnum;
	if (core_write(fd, &eh, sizeof(eh))) goto done;

	/* PT_NOTE phdr */
	Elf64_Phdr ph; memset(&ph, 0, sizeof(ph));
	ph.p_type=PT_NOTE; ph.p_offset=note_off; ph.p_filesz=notesz; ph.p_align=4;
	if (core_write(fd, &ph, sizeof(ph))) goto done;

	/* PT_LOAD phdrs */
	uint64_t off = load_off;
	for (vma_t *v = proc->vm->vmas; v; v = v->next) {
		if (v->size == 0) continue;
		if (off + v->size > load_off + total) break;
		memset(&ph, 0, sizeof(ph));
		ph.p_type=PT_LOAD; ph.p_offset=off; ph.p_vaddr=v->vaddr; ph.p_paddr=v->vaddr;
		ph.p_filesz=v->size; ph.p_memsz=v->size; ph.p_align=0x1000;
		ph.p_flags=PF_R | ((v->flags & 0x2) ? PF_W : 0) | ((v->flags & 0x4) ? PF_X : 0);
		if (core_write(fd, &ph, sizeof(ph))) goto done;
		off += v->size;
	}

	/* NOTE section: NT_PRSTATUS then NT_PRPSINFO, name "CORE" */
	{
		uint8_t nb[8 + 4 + 4 + PRSTATUS_SIZE > 8 + 4 + 4 + PRPSINFO_SIZE
		           ? 8 + 4 + 4 + PRSTATUS_SIZE : 8 + 4 + 4 + PRPSINFO_SIZE];
		/* PRSTATUS */
		memset(nb, 0, sizeof(nb));
		*(uint32_t *)(nb + 0) = 5;              /* namesz "CORE\0" */
		*(uint32_t *)(nb + 4) = PRSTATUS_SIZE;  /* descsz */
		*(uint32_t *)(nb + 8) = NT_PRSTATUS;
		memcpy(nb + 12, "CORE", 4);             /* name, 8-byte slot (padded) */
		uint8_t *desc = nb + 20;
		*(int16_t *)(desc + 12) = (int16_t)sig; /* pr_cursig */
		*(int32_t *)(desc + 32) = (int32_t)proc->pid; /* pr_pid */
		fill_pr_reg((uint64_t *)(desc + PRSTATUS_REG_OFF), tf);
		if (core_write(fd, nb, 20 + PRSTATUS_SIZE)) goto done;

		/* PRPSINFO */
		memset(nb, 0, sizeof(nb));
		*(uint32_t *)(nb + 0) = 5;
		*(uint32_t *)(nb + 4) = PRPSINFO_SIZE;
		*(uint32_t *)(nb + 8) = NT_PRPSINFO;
		memcpy(nb + 12, "CORE", 4);
		desc = nb + 20;
		int nl = 0; while (proc->name[nl] && nl < 15) { desc[40 + nl] = proc->name[nl]; nl++; }
		*(int32_t *)(desc + 24) = (int32_t)proc->pid; /* pr_pid */
		if (core_write(fd, nb, 20 + PRPSINFO_SIZE)) goto done;
	}

	/* memory: one PT_LOAD body per VMA, page by page (zeros for unmapped) */
	{
		static uint8_t zero[4096];
		uint64_t written = 0;
		for (vma_t *v = proc->vm->vmas; v && written < total; v = v->next) {
			if (v->size == 0) continue;
			if (written + v->size > total) break;
			for (uint64_t a = 0; a < v->size; a += 4096) {
				void *uaddr = (void *)(v->vaddr + a);
				size_t chunk = (v->size - a) < 4096 ? (size_t)(v->size - a) : 4096;
				if (user_pages_mapped(uaddr, chunk))
					core_write(fd, uaddr, chunk);
				else
					core_write(fd, zero, chunk);
			}
			written += v->size;
		}
	}

done:
	vfs_close(fd);
}
