#ifndef ELF_H
#define ELF_H

#include <uapi/types.h>
#include <kernel/mm/vm.h>

#define ELF_MAGIC    0x464C457FU   /* 0x7f 'E' 'L' 'F' */
#define ELF_CLASS64  2
#define ELF_LSB      1
#define ET_EXEC      2
#define ET_DYN       3
#define EM_X86_64    62
#define EM_AARCH64   183
#define PT_LOAD      1
#define PT_INTERP    3

typedef struct {
	uint8_t  magic[4];
	uint8_t  class;
	uint8_t  data;
	uint8_t  version;
	uint8_t  osabi;
	uint8_t  pad[8];
	uint16_t type;
	uint16_t machine;
	uint32_t version2;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
} __attribute__((packed)) elf64_hdr_t;

typedef struct {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
} __attribute__((packed)) elf64_phdr_t;

#define ELF_PF_X  0x1
#define ELF_PF_W  0x2
#define ELF_PF_R  0x4

typedef struct {
	uint64_t entry;
	uint64_t load_end;
	uint64_t phdr_va;    /* AT_PHDR: virtual address of program headers */
	uint64_t load_bias;  /* 0 for ET_EXEC; base for ET_DYN */
	uint16_t phnum;
	uint16_t phentsize;
	char     interp[128]; /* PT_INTERP path, "" if statically linked */
} elf_info_t;

/* load_bias_hint: base for an ET_DYN image (0 = pick a default); ignored for
 * ET_EXEC. Used to place the dynamic linker away from the main executable. */
int elf_load(vm_space_t *vm, const void *data, size_t len,
             uint64_t load_bias_hint, elf_info_t *info);

#endif
