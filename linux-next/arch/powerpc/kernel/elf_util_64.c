/*
 * Utility functions to work with ELF files.
 *
 * Copyright (C) 2016, IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/ppc-opcode.h>
#include <asm/elf_util.h>

/*
 * We just need to use the functions defined in <asm/module.h>, so just declare
 * struct module here and avoid having to import <linux/module.h>.
 */
struct module;
#include <asm/module.h>

#ifdef PPC64_ELF_ABI_v2
/* PowerPC64 specific values for the Elf64_Sym st_other field.  */
#define STO_PPC64_LOCAL_BIT	5
#define STO_PPC64_LOCAL_MASK	(7 << STO_PPC64_LOCAL_BIT)
#define PPC64_LOCAL_ENTRY_OFFSET(other)					\
 (((1 << (((other) & STO_PPC64_LOCAL_MASK) >> STO_PPC64_LOCAL_BIT)) >> 2) << 2)

static unsigned int local_entry_offset(const Elf64_Sym *sym)
{
	/* sym->st_other indicates offset to local entry point
	 * (otherwise it will assume r12 is the address of the start
	 * of function and try to derive r2 from it). */
	return PPC64_LOCAL_ENTRY_OFFSET(sym->st_other);
}
#else
static unsigned int local_entry_offset(const Elf64_Sym *sym)
{
	return 0;
}
#endif

#ifdef CC_USING_MPROFILE_KERNEL
/*
 * In case of _mcount calls, do not save the current callee's TOC (in r2) into
 * the original caller's stack frame. If we did we would clobber the saved TOC
 * value of the original caller.
 */
static void squash_toc_save_inst(const char *name, unsigned long addr)
{
	struct ppc64_stub_entry *stub = (struct ppc64_stub_entry *)addr;

	/* Only for calls to _mcount */
	if (strcmp("_mcount", name) != 0)
		return;

	stub->jump[2] = PPC_INST_NOP;
}
#else
static void squash_toc_save_inst(const char *name, unsigned long addr) { }
#endif

/**
 * elf64_apply_relocate_add - apply 64 bit RELA relocations
 * @elf_info:		Support information for the ELF binary being relocated.
 * @strtab:		String table for the associated symbol table.
 * @rela:		Contents of the section with the relocations to apply.
 * @num_rela:		Number of relocation entries in the section.
 * @syms_base:		Contents of the associated symbol table.
 * @loc_base:		Contents of the section to which relocations apply.
 * @addr_base:		The address where the section will be loaded in memory.
 * @relative_symbols:	Are the symbols' st_value members relative?
 * @check_symbols:	Fail if an unexpected symbol is found?
 * @obj_name:		The name of the ELF binary, for information messages.
 *
 * Applies RELA relocations to an ELF file already at its final location
 * in memory (in which case loc_base == addr_base), or still in a temporary
 * buffer.
 */
int elf64_apply_relocate_add(const struct elf_info *elf_info,
			     const char *strtab, const Elf64_Rela *rela,
			     unsigned int num_rela, void *syms_base,
			     void *loc_base, Elf64_Addr addr_base,
			     bool relative_symbols, bool check_symbols,
			     const char *obj_name)
{
	unsigned int i;
	unsigned long *location;
	unsigned long address;
	unsigned long sec_base;
	unsigned long value;
	int reloc_type;
	const char *name;
	Elf64_Sym *sym;

	for (i = 0; i < num_rela; i++) {
		/*
		 * rels[i].r_offset contains the byte offset from the beginning
		 * of section to the storage unit affected.
		 *
		 * This is the location to update in the temporary buffer where
		 * the section is currently loaded. The section will finally
		 * be loaded to a different address later, pointed to by
		 * addr_base.
		 */
		location = loc_base + rela[i].r_offset;

		/* Final address of the location. */
		address = addr_base + rela[i].r_offset;

		/* This is the symbol the relocation is referring to. */
		sym = (Elf64_Sym *) syms_base + ELF64_R_SYM(rela[i].r_info);

		if (sym->st_name)
			name = strtab + sym->st_name;
		else
			name = "<unnamed symbol>";

		reloc_type = ELF64_R_TYPE(rela[i].r_info);

		pr_debug("RELOC at %p: %i-type as %s (0x%lx) + %li\n",
		       location, reloc_type, name, (unsigned long)sym->st_value,
		       (long)rela[i].r_addend);

		if (check_symbols) {
			/*
			 * TOC symbols appear as undefined but should be
			 * resolved as well, so allow them to be processed.
			 */
			if (sym->st_shndx == SHN_UNDEF &&
					strcmp(name, ".TOC.") != 0 &&
					reloc_type != R_PPC64_TOC) {
				pr_err("Undefined symbol: %s\n", name);
				return -ENOEXEC;
			} else if (sym->st_shndx == SHN_COMMON) {
				pr_err("Symbol '%s' in common section.\n",
				       name);
				return -ENOEXEC;
			}
		}

		if (relative_symbols && sym->st_shndx != SHN_ABS) {
			if (sym->st_shndx >= elf_info->ehdr->e_shnum) {
				pr_err("Invalid section %d for symbol %s\n",
				       sym->st_shndx, name);
				return -ENOEXEC;
			}

			sec_base = elf_info->sechdrs[sym->st_shndx].sh_addr;
		} else
			sec_base = 0;

		/* `Everything is relative'. */
		value = sym->st_value + sec_base + rela[i].r_addend;

		switch (reloc_type) {
		case R_PPC64_ADDR32:
			/* Simply set it */
			*(u32 *)location = value;
			break;

		case R_PPC64_ADDR64:
			/* Simply set it */
			*(unsigned long *)location = value;
			break;

		case R_PPC64_REL32:
			*(uint32_t *) location =
				value - (uint32_t)(uint64_t) location;
			break;

		case R_PPC64_TOC:
			*(unsigned long *)location = my_r2(elf_info);
			break;

		case R_PPC64_TOC16:
			/* Subtract TOC pointer */
			value -= my_r2(elf_info);
			if (value + 0x8000 > 0xffff) {
				pr_err("%s: bad TOC16 relocation (0x%lx)\n",
				       obj_name, value);
				return -ENOEXEC;
			}
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xffff)
				| (value & 0xffff);
			break;

		case R_PPC64_TOC16_LO:
			/* Subtract TOC pointer */
			value -= my_r2(elf_info);
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xffff)
				| (value & 0xffff);
			break;

		case R_PPC64_TOC16_DS:
			/* Subtract TOC pointer */
			value -= my_r2(elf_info);
			if ((value & 3) != 0 || value + 0x8000 > 0xffff) {
				pr_err("%s: bad TOC16_DS relocation (0x%lx)\n",
				       obj_name, value);
				return -ENOEXEC;
			}
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xfffc)
				| (value & 0xfffc);
			break;

		case R_PPC64_TOC16_LO_DS:
			/* Subtract TOC pointer */
			value -= my_r2(elf_info);
			if ((value & 3) != 0) {
				pr_err("%s: bad TOC16_LO_DS relocation (0x%lx)\n",
				       obj_name, value);
				return -ENOEXEC;
			}
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xfffc)
				| (value & 0xfffc);
			break;

		case R_PPC64_TOC16_HI:
			/* Subtract TOC pointer */
			value -= my_r2(elf_info);
			value = value >> 16;
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xffff)
				| (value & 0xffff);

		case R_PPC64_TOC16_HA:
			/* Subtract TOC pointer */
			value -= my_r2(elf_info);
			value = ((value + 0x8000) >> 16);
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xffff)
				| (value & 0xffff);
			break;

		case R_PPC64_REL14:
			/* Convert value to relative */
			value -= address;
			if (value + 0x8000 > 0xffff || (value & 3) != 0) {
				pr_err("%s: REL14 %li out of range!\n",
				       obj_name, (long int) value);
				return -ENOEXEC;
			}

			/* Only replace bits 2 through 16 */
			*(uint32_t *)location
				= (*(uint32_t *)location & ~0xfffc)
				| (value & 0xfffc);
			break;

		case R_PPC_REL24:
			/* FIXME: Handle weak symbols here --RR */
			if (sym->st_shndx == SHN_UNDEF) {
				/* External: go via stub */
				value = stub_for_addr(elf_info, value, obj_name);
				if (!value)
					return -ENOENT;
				if (!restore_r2((u32 *)location + 1, obj_name))
					return -ENOEXEC;

				squash_toc_save_inst(strtab + sym->st_name, value);
			} else
				value += local_entry_offset(sym);

			/* Convert value to relative */
			value -= address;
			if (value + 0x2000000 > 0x3ffffff || (value & 3) != 0){
				pr_err("%s: REL24 %li out of range!\n",
				       obj_name, (long int)value);
				return -ENOEXEC;
			}

			/* Only replace bits 2 through 26 */
			*(uint32_t *)location
				= (*(uint32_t *)location & ~0x03fffffc)
				| (value & 0x03fffffc);
			break;

		case R_PPC64_REL64:
			/* 64 bits relative (used by features fixups) */
			*location = value - address;
			break;

		case R_PPC64_TOCSAVE:
			/*
			 * Marker reloc indicates we don't have to save r2.
			 * That would only save us one instruction, so ignore
			 * it.
			 */
			break;

		case R_PPC64_ENTRY:
			/*
			 * Optimize ELFv2 large code model entry point if
			 * the TOC is within 2GB range of current location.
			 */
			value = my_r2(elf_info) - address;
			if (value + 0x80008000 > 0xffffffff)
				break;
			/*
			 * Check for the large code model prolog sequence:
			 *	ld r2, ...(r12)
			 *	add r2, r2, r12
			 */
			if ((((uint32_t *)location)[0] & ~0xfffc)
			    != 0xe84c0000)
				break;
			if (((uint32_t *)location)[1] != 0x7c426214)
				break;
			/*
			 * If found, replace it with:
			 *	addis r2, r12, (.TOC.-func)@ha
			 *	addi r2, r12, (.TOC.-func)@l
			 */
			((uint32_t *)location)[0] = 0x3c4c0000 + PPC_HA(value);
			((uint32_t *)location)[1] = 0x38420000 + PPC_LO(value);
			break;

		case R_PPC64_ADDR16_LO:
			*(uint16_t *)location = value & 0xffff;
			break;

		case R_PPC64_ADDR16_HI:
			*(uint16_t *)location = (value >> 16) & 0xffff;
			break;

		case R_PPC64_ADDR16_HA:
			*(uint16_t *)location = (((value + 0x8000) >> 16) &
							0xffff);
			break;

		case R_PPC64_ADDR16_HIGHER:
			*(uint16_t *)location = (((uint64_t)value >> 32) &
							0xffff);
			break;

		case R_PPC64_ADDR16_HIGHEST:
			*(uint16_t *)location = (((uint64_t)value >> 48) &
							0xffff);
			break;

		case R_PPC64_REL16_HA:
			/* Subtract location pointer */
			value -= address;
			value = ((value + 0x8000) >> 16);
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xffff)
				| (value & 0xffff);
			break;

		case R_PPC64_REL16_LO:
			/* Subtract location pointer */
			value -= address;
			*((uint16_t *) location)
				= (*((uint16_t *) location) & ~0xffff)
				| (value & 0xffff);
			break;

		default:
			pr_err("%s: Unknown ADD relocation: %d\n", obj_name,
			       reloc_type);
			return -ENOEXEC;
		}
	}

	return 0;
}
