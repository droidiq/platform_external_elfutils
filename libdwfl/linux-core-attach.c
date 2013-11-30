/* Get Dwarf Frame state for target core file.
   Copyright (C) 2013 Red Hat, Inc.
   This file is part of elfutils.

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.  */

#include "libdwflP.h"
#include <fcntl.h>
#include "system.h"

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct core_arg
{
  Elf *core;
  Elf_Data *note_data;
  size_t thread_note_offset;
  Ebl *ebl;
};

struct thread_arg
{
  struct core_arg *core_arg;
  size_t note_offset;
};

static bool
core_memory_read (Dwfl *dwfl, Dwarf_Addr addr, Dwarf_Word *result,
		  void *dwfl_arg)
{
  Dwfl_Process *process = dwfl->process;
  struct core_arg *core_arg = dwfl_arg;
  Elf *core = core_arg->core;
  assert (core != NULL);
  static size_t phnum;
  if (elf_getphdrnum (core, &phnum) < 0)
    {
      __libdwfl_seterrno (DWFL_E_LIBELF);
      return false;
    }
  for (size_t cnt = 0; cnt < phnum; ++cnt)
    {
      GElf_Phdr phdr_mem, *phdr = gelf_getphdr (core, cnt, &phdr_mem);
      if (phdr == NULL || phdr->p_type != PT_LOAD)
	continue;
      /* Bias is zero here, a core file itself has no bias.  */
      GElf_Addr start = __libdwfl_segment_start (dwfl, phdr->p_vaddr);
      GElf_Addr end = __libdwfl_segment_end (dwfl,
					     phdr->p_vaddr + phdr->p_memsz);
      unsigned bytes = ebl_get_elfclass (process->ebl) == ELFCLASS64 ? 8 : 4;
      if (addr < start || addr + bytes > end)
	continue;
      Elf_Data *data;
      data = elf_getdata_rawchunk (core, phdr->p_offset + addr - start,
				   bytes, ELF_T_ADDR);
      if (data == NULL)
	{
	  __libdwfl_seterrno (DWFL_E_LIBELF);
	  return false;
	}
      assert (data->d_size == bytes);
      /* FIXME: Currently any arch supported for unwinding supports
	 unaligned access.  */
      if (bytes == 8)
	*result = *(const uint64_t *) data->d_buf;
      else
	*result = *(const uint32_t *) data->d_buf;
      return true;
    }
  __libdwfl_seterrno (DWFL_E_ADDR_OUTOFRANGE);
  return false;
}

static pid_t
core_next_thread (Dwfl *dwfl __attribute__ ((unused)), void *dwfl_arg,
		  void **thread_argp)
{
  struct core_arg *core_arg = dwfl_arg;
  Elf *core = core_arg->core;
  GElf_Nhdr nhdr;
  size_t name_offset;
  size_t desc_offset;
  Elf_Data *note_data = core_arg->note_data;
  size_t offset;
  while (offset = core_arg->thread_note_offset, offset < note_data->d_size
	 && (core_arg->thread_note_offset = gelf_getnote (note_data, offset,
							  &nhdr, &name_offset,
							  &desc_offset)) > 0)
    {
      /* Do not check NAME for now, help broken Linux kernels.  */
      const char *name = note_data->d_buf + name_offset;
      const char *desc = note_data->d_buf + desc_offset;
      GElf_Word regs_offset;
      size_t nregloc;
      const Ebl_Register_Location *reglocs;
      size_t nitems;
      const Ebl_Core_Item *items;
      if (! ebl_core_note (core_arg->ebl, &nhdr, name,
			   &regs_offset, &nregloc, &reglocs, &nitems, &items))
	{
	  /* This note may be just not recognized, skip it.  */
	  continue;
	}
      if (nhdr.n_type != NT_PRSTATUS)
	continue;
      const Ebl_Core_Item *item;
      for (item = items; item < items + nitems; item++)
	if (strcmp (item->name, "pid") == 0)
	  break;
      if (item == items + nitems)
	continue;
      uint32_t val32 = *(const uint32_t *) (desc + item->offset);
      val32 = (elf_getident (core, NULL)[EI_DATA] == ELFDATA2MSB
		? be32toh (val32) : le32toh (val32));
      pid_t tid = (int32_t) val32;
      eu_static_assert (sizeof val32 <= sizeof tid);
      struct thread_arg *thread_arg = malloc (sizeof (*thread_arg));
      if (thread_arg == NULL)
	{
	  __libdwfl_seterrno (DWFL_E_NOMEM);
	  return -1;
	}
      thread_arg->core_arg = core_arg;
      thread_arg->note_offset = offset;
      *thread_argp = thread_arg;
      return tid;
    }
  return 0;
}

static bool
core_set_initial_registers (Dwfl_Thread *thread, void *thread_arg_voidp)
{
  struct thread_arg *thread_arg = thread_arg_voidp;
  struct core_arg *core_arg = thread_arg->core_arg;
  Elf *core = core_arg->core;
  size_t offset = thread_arg->note_offset;
  GElf_Nhdr nhdr;
  size_t name_offset;
  size_t desc_offset;
  Elf_Data *note_data = core_arg->note_data;
  size_t nregs = ebl_frame_nregs (core_arg->ebl);
  assert (nregs > 0);
  assert (offset < note_data->d_size);
  size_t getnote_err = gelf_getnote (note_data, offset, &nhdr, &name_offset,
				     &desc_offset);
  /* __libdwfl_attach_state_for_core already verified the note is there.  */
  assert (getnote_err != 0);
  /* Do not check NAME for now, help broken Linux kernels.  */
  const char *name = note_data->d_buf + name_offset;
  const char *desc = note_data->d_buf + desc_offset;
  GElf_Word regs_offset;
  size_t nregloc;
  const Ebl_Register_Location *reglocs;
  size_t nitems;
  const Ebl_Core_Item *items;
  int core_note_err = ebl_core_note (core_arg->ebl, &nhdr, name, &regs_offset,
				     &nregloc, &reglocs, &nitems, &items);
  /* __libdwfl_attach_state_for_core already verified the note is there.  */
  assert (core_note_err != 0);
  assert (nhdr.n_type == NT_PRSTATUS);
  const Ebl_Core_Item *item;
  for (item = items; item < items + nitems; item++)
    if (strcmp (item->name, "pid") == 0)
      break;
  assert (item < items + nitems);
  pid_t tid;
  {
    uint32_t val32 = *(const uint32_t *) (desc + item->offset);
    val32 = (elf_getident (core, NULL)[EI_DATA] == ELFDATA2MSB
	     ? be32toh (val32) : le32toh (val32));
    tid = (int32_t) val32;
    eu_static_assert (sizeof val32 <= sizeof tid);
  }
  /* core_next_thread already found this TID there.  */
  assert (tid == INTUSE(dwfl_thread_tid) (thread));
  desc += regs_offset;
  for (size_t regloci = 0; regloci < nregloc; regloci++)
    {
      const Ebl_Register_Location *regloc = reglocs + regloci;
      if (regloc->regno >= nregs)
	continue;
      assert (regloc->bits == 32 || regloc->bits == 64);
      const char *reg_desc = desc + regloc->offset;
      for (unsigned regno = regloc->regno;
	   regno < MIN (regloc->regno + (regloc->count ?: 1U), nregs);
	   regno++)
	{
	  /* PPC provides DWARF register 65 irrelevant for
	     CFI which clashes with register 108 (LR) we need.
	     LR (108) is provided earlier (in NT_PRSTATUS) than the # 65.
	     FIXME: It depends now on their order in core notes.
	     FIXME: It uses private function.  */
	  if (__libdwfl_frame_reg_get (thread->unwound, regno, NULL))
	    continue;
	  Dwarf_Word val;
	  switch (regloc->bits)
	  {
	    case 32:;
	      uint32_t val32 = *(const uint32_t *) reg_desc;
	      reg_desc += sizeof val32;
	      val32 = (elf_getident (core, NULL)[EI_DATA] == ELFDATA2MSB
		       ? be32toh (val32) : le32toh (val32));
	      /* Do a host width conversion.  */
	      val = val32;
	      break;
	    case 64:;
	      uint64_t val64 = *(const uint64_t *) reg_desc;
	      reg_desc += sizeof val64;
	      val64 = (elf_getident (core, NULL)[EI_DATA] == ELFDATA2MSB
		       ? be64toh (val64) : le64toh (val64));
	      assert (sizeof (*thread->unwound->regs) == sizeof val64);
	      val = val64;
	      break;
	    default:
	      abort ();
	  }
	  /* Registers not valid for CFI are just ignored.  */
	  INTUSE(dwfl_thread_state_registers) (thread, regno, 1, &val);
	  reg_desc += regloc->pad;
	}
    }
  return true;
}

static void
core_detach (Dwfl *dwfl __attribute__ ((unused)), void *dwfl_arg)
{
  struct core_arg *core_arg = dwfl_arg;
  ebl_closebackend (core_arg->ebl);
  free (core_arg);
}

static const Dwfl_Thread_Callbacks core_thread_callbacks =
{
  core_next_thread,
  core_memory_read,
  core_set_initial_registers,
  core_detach,
  NULL, /* core_thread_detach */
};

static Dwfl_Error
attach_state_for_core (Dwfl *dwfl, Elf *core)
{
  Ebl *ebl = ebl_openbackend (core);
  if (ebl == NULL)
    return DWFL_E_LIBEBL;
  size_t nregs = ebl_frame_nregs (ebl);
  if (nregs == 0)
    {
      ebl_closebackend (ebl);
      return DWFL_E_NO_UNWIND;
    }
  GElf_Ehdr ehdr_mem, *ehdr = gelf_getehdr (core, &ehdr_mem);
  if (ehdr == NULL)
    {
      ebl_closebackend (ebl);
      return DWFL_E_LIBELF;
    }
  assert (ehdr->e_type == ET_CORE);
  size_t phnum;
  if (elf_getphdrnum (core, &phnum) < 0)
    {
      ebl_closebackend (ebl);
      return DWFL_E_LIBELF;
    }
  pid_t pid = -1;
  Elf_Data *note_data = NULL;
  for (size_t cnt = 0; cnt < phnum; ++cnt)
    {
      GElf_Phdr phdr_mem, *phdr = gelf_getphdr (core, cnt, &phdr_mem);
      if (phdr != NULL && phdr->p_type == PT_NOTE)
	{
	  note_data = elf_getdata_rawchunk (core, phdr->p_offset,
					    phdr->p_filesz, ELF_T_NHDR);
	  break;
	}
    }
  if (note_data == NULL)
    {
      ebl_closebackend (ebl);
      return DWFL_E_LIBELF;
    }
  size_t offset = 0;
  GElf_Nhdr nhdr;
  size_t name_offset;
  size_t desc_offset;
  while (offset < note_data->d_size
	 && (offset = gelf_getnote (note_data, offset,
				    &nhdr, &name_offset, &desc_offset)) > 0)
    {
      /* Do not check NAME for now, help broken Linux kernels.  */
      const char *name = note_data->d_buf + name_offset;
      const char *desc = note_data->d_buf + desc_offset;
      GElf_Word regs_offset;
      size_t nregloc;
      const Ebl_Register_Location *reglocs;
      size_t nitems;
      const Ebl_Core_Item *items;
      if (! ebl_core_note (ebl, &nhdr, name,
			   &regs_offset, &nregloc, &reglocs, &nitems, &items))
	{
	  /* This note may be just not recognized, skip it.  */
	  continue;
	}
      if (nhdr.n_type != NT_PRPSINFO)
	continue;
      const Ebl_Core_Item *item;
      for (item = items; item < items + nitems; item++)
	if (strcmp (item->name, "pid") == 0)
	  break;
      if (item == items + nitems)
	continue;
      uint32_t val32 = *(const uint32_t *) (desc + item->offset);
      val32 = (elf_getident (core, NULL)[EI_DATA] == ELFDATA2MSB
		? be32toh (val32) : le32toh (val32));
      pid = (int32_t) val32;
      eu_static_assert (sizeof val32 <= sizeof pid);
      break;
    }
  if (pid == -1)
    {
      /* No valid NT_PRPSINFO recognized in this CORE.  */
      ebl_closebackend (ebl);
      return DWFL_E_BADELF;
    }
  struct core_arg *core_arg = malloc (sizeof *core_arg);
  if (core_arg == NULL)
    {
      ebl_closebackend (ebl);
      return DWFL_E_NOMEM;
    }
  core_arg->core = core;
  core_arg->note_data = note_data;
  core_arg->thread_note_offset = 0;
  core_arg->ebl = ebl;
  if (! INTUSE(dwfl_attach_state) (dwfl, core, pid, &core_thread_callbacks,
				   core_arg))
    {
      Dwfl_Error error = dwfl_errno ();
      assert (error != DWFL_E_NOERROR);
      free (core_arg);
      ebl_closebackend (ebl);
      return error;
    }
  return DWFL_E_NOERROR;
}

bool
internal_function
__libdwfl_attach_state_for_core (Dwfl *dwfl, Elf *core)
{
  if (dwfl->process != NULL)
    {
      __libdwfl_seterrno (DWFL_E_ATTACH_STATE_CONFLICT);
      return false;
    }
  Dwfl_Error error = attach_state_for_core (dwfl, core);
  assert ((dwfl->process != NULL) == (error == DWFL_E_NOERROR));
  dwfl->process_attach_error = error;
  if (error != DWFL_E_NOERROR)
    {
      __libdwfl_seterrno (error);
      return false;
    }
  return true;
}
