/**
 * @file fbt_actions.c
 * This module defines some generic default actions that are used to translate
 * specific machine codes.
 *
 * Copyright (c) 2011-2015 ETH Zurich
 * 
 * @author Mathias Payer <mathias.payer@nebelwelt.net>
 * @author Antonio Barresi <antonio.barresi@inf.ethz.ch>
 *
 * $Date: 2013-02-13 23:03:12 +0100 (Wed, 13 Feb 2013) $
 * $LastChangedDate: 2013-02-13 23:03:12 +0100 (Wed, 13 Feb 2013) $
 * $LastChangedBy: payerm $
 * $Revision: 1591 $
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include "fbt_actions.h"

#if defined(ASSERTIONS)
#include <assert.h>
#endif

#include <stddef.h> /* offsetof */

#include "fbt_asm_macros.h"
#include "fbt_code_cache.h"
#include "fbt_debug.h"
#include "fbt_datatypes.h"
#include "fbt_dso.h"
#include "fbt_libc.h"
#include "fbt_llio.h"
#include "fbt_syscalls_impl.h"
#include "fbt_translate.h"
#include "fbt_mem_mgmt.h"
#include "fbt_x86_opcode.h"

#if defined(SHADOWSTACK_DEBUG)
#include "fbt_trampoline.h"
#endif /* SHADOWSTACK_DEBUG */

#if defined(TRACK_CFTX)
#include "fbt_restart_transaction.h"
#endif /* TRACK_CFTX */

#if defined(VERIFY_CFTX)
extern ulong_t sl_resolve_plt_call(ulong_t addr, struct dso_chain *dso_objects);
#endif

enum translation_state action_none(struct translate *ts) {
  PRINT_DEBUG_FUNCTION_START("action_none(*ts=%p)", ts);
  /* do nothing */
  PRINT_DEBUG_FUNCTION_END("-> neutral");
  return NEUTRAL;
}

enum translation_state action_copy(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;
  unsigned char* transl_addr = ts->transl_instr;
  long length = ts->next_instr - ts->cur_instr;

  PRINT_DEBUG_FUNCTION_START("action_copy(*addr=%p, *transl_addr=%p, "
                             "length=%i)", addr, transl_addr, length);
  /* copy instruction verbatim to translated version */
  fbt_memcpy(transl_addr, addr, length);

  PRINT_DEBUG_FUNCTION_END("-> neutral, transl_length=%i", length);
  ts->transl_instr += length;
  if (*(ts->cur_instr) == 0xcc || *(ts->cur_instr) == 0xcd ||
      *(ts->cur_instr) == 0xce) {
#if defined(AUTHORIZE_SYSCALLS)
    if (*(ts->cur_instr) == 0xcd) {
      if (*(ts->first_byte_after_opcode) != 0x80) {
        fbt_suicide_str("Illegal interrupt encountered (fbt_actions.c)\n");
      }
      /* undo copy of the interrupt */
      ts->transl_instr -= length;
      transl_addr = ts->transl_instr;
      /* store location of this syscall */
      MOVL_IMM32_MEM32(transl_addr, 0x05, addr, &(ts->tld->syscall_location));
      MOVL_IMM32_MEM32(transl_addr, 0x05, 0x00c0ff33, &(ts->tld->ind_target));

      /* pointer to after int80 */
      ulong_t *ptr = (ulong_t*)(transl_addr-sizeof(void*));
      /* write: jump instruction to trampoline */
      JMP_REL32(transl_addr, (ulong_t)(ts->tld->int80_trampoline));
      *ptr = (ulong_t)(transl_addr);
      ts->transl_instr = transl_addr;
    }
#endif
    /*
     * we just copied an interrupt
     * 0xcc - int3 (debug)
     * 0xcd - int imm8
     * 0xce - into (int 4) if overflow flag is set to 1
     * because we might execute a kernel routine (which might not return or
     * might do something funny to our stack) we finish the TU here and
     * issue some glue code to restart the translation if we resume after the
     * int itself
     */
    PRINT_DEBUG("Encountered an interrupt - closing TU with some glue code\n");
    return CLOSE_GLUE;
  }
  return NEUTRAL;
}

enum translation_state action_warn(struct translate *ts) {
  PRINT_DEBUG_FUNCTION_START("action_warn(*ts=%p)", ts);
  PRINT_DEBUG("unhandled opcode encountered in TU at %p: %s", ts->cur_instr,
              MEMDUMP(ts->cur_instr, ts->next_instr - ts->cur_instr));
  PRINT_DEBUG("Will try if it works to simply copy the instruction into the "
              "code cache, but something bad could happen now...");
  PRINT_DEBUG_FUNCTION_END("-> ???");
  return action_copy(ts);
}

enum translation_state action_fail(struct translate *ts) {
  PRINT_DEBUG_FUNCTION_START("action_fail(*ts=%p)", ts);
  PRINT_DEBUG("unhandled opcode encountered in TU at %p: %s", ts->cur_instr,
              MEMDUMP(ts->cur_instr, ts->next_instr - ts->cur_instr));
  PRINT_DEBUG("giving up!!!");
  PRINT_DEBUG_FUNCTION_END("-> FAIL");

#ifdef DEBUG
  llprintf("\n\n\t\tERROR: unhandled opcode encountered in TU at %p: %s\n\n",
              ts->cur_instr, MEMDUMP(ts->cur_instr,
              ts->next_instr - ts->cur_instr));
  llprintf("\nERROR: unhandled opcode encountered in TU at %p\nBtOX will "
           "exit now!\n", ts->cur_instr);
#endif

#if defined(SLEEP_ON_FAIL)
  llprintf("Something bad happened (action_fail). Attach a debugger!\n");
  while (1);
#endif
  fbt_suicide(255);
  return CLOSE;
}

enum translation_state action_jmp(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;

  unsigned char *original_addr = addr;
  
  PRINT_DEBUG("original_addr=%x / addr=%x\n", original_addr, addr);  
  
  unsigned char* transl_addr = ts->transl_instr;
  int length = ts->next_instr - ts->cur_instr;

  PRINT_DEBUG_FUNCTION_START("action_jmp(*addr=%p, *transl_addr=%p, length=%i)",
                             addr, transl_addr, length);
#if defined(FBT_STATISTIC)
  fbt_nr_translated_jmp++;
#endif

  /* read call argument (either 8bit or 32bit offset) and add EIP (EIP = addr +
     length) to argument --> absolute target address = addr + length + offset */
  #if defined(ASSERTIONS)
  assert(!HAS_PREFIX(*addr)); /* no prefixes allowed */
  #endif

  int32_t jump_target=0;
  if (*addr == 0xE9) {
    /* 32bit offset */
    jump_target = *((int32_t*)(addr + 1)) + (int32_t)original_addr + length;
    PRINT_DEBUG("jump_target = %x + %x + %x", *(int32_t*)(addr + 1), original_addr, length);
  } else {
    /* our argument is only an 8bit offset */
    jump_target = (int32_t)(*((char*)(addr + 1)) + original_addr + length);
  }
  
  PRINT_DEBUG("original jmp_target: %p", (void*)jump_target);

/* It seems that some binaries like /bin/ls uses direct jmp's into plt instead of direct calls. */
/* We need to handle and inline this jmps correctly. */
#if defined(INLINE_PLT_CALLS)
/* Check if call targets plt section and resolve destination address */
unsigned long resolved_addr = sl_resolve_plt_call(jump_target, ts->tld->dso_objects);
if (resolved_addr != jump_target) {
  /* PLT call, check transfer and inline it */
  #if defined(VERIFY_CFTX)

  #if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
  struct sh_symbol* symbol = fbt_find_symbol(ts->tld, addr);
  fbt_check_transfer(ts->tld, original_addr, (unsigned char*)&resolved_addr, CFTX_CALL_IND, symbol); /* treat like an indirect call */
  #else
  fbt_check_transfer(ts->tld, original_addr, (unsigned char*)&resolved_addr, CFTX_CALL_IND); /* treat like an indirect call */
  #endif
  
  #endif /* VERIFY_CFTX */
  
  jump_target = resolved_addr;
}
#endif
/* End - PLT Jmps inlining! */

  /* check if the target is already translated; if it is not, do so now */
  void *transl_target = fbt_ccache_find(ts->tld, (void*)jump_target);
  if (transl_target == NULL) {
    /* we still have to translate the call target */
    PRINT_DEBUG_FUNCTION_END("-> open, transl_length=0");
    /* no need to actually jump
      simply change the next instr pointer to the first instr of the function
      this will put the body of the function right as the next instr in the
      translated code */
    ts->next_instr = (unsigned char*)jump_target;
    /* put the target into the tcache so later jumps can use the translated
       code */
    #ifndef TRACK_BASIC_BLOCKS
    //fbt_ccache_add_entry(ts->tld, (void*)jump_target, ts->transl_instr);
    #endif
    
    return OPEN;
  }

  PRINT_DEBUG("translated jmp_target: %p", transl_target);

  /* write: jmp */
  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft = {
    .location = transl_addr + 1,
    .original = addr
  };
  fbt_store_cftx(ts->tld, &cft);
  #endif /* TRACK_CFTX */

  JMP_REL32(transl_addr, (int32_t)transl_target);

  PRINT_DEBUG_FUNCTION_END("-> close, transl_length=%i",
                           transl_addr - ts->transl_instr);
  ts->transl_instr = transl_addr;
  return CLOSE;
}

enum translation_state action_jmp_indirect(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;
  unsigned char* transl_addr = ts->transl_instr;
  unsigned char *first_byte_after_opcode = ts->first_byte_after_opcode;
  int length = ts->next_instr - ts->cur_instr;

  PRINT_DEBUG_FUNCTION_START("action_jmp_indirect(*addr=%p, *transl_addr=%p, " \
                             "length=%i)", addr, transl_addr, length);

#if defined(FBT_STATISTIC)
  fbt_nr_translated_jmp_ind++;
#endif

  #if defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX)
  	struct dso_chain *cur = ts->tld->dso_objects;
  	long ret;
  	
		while (cur != 0) {
			/* is the pointer in the current dso? */
            if (PTR_IN_REGION(addr, cur->baseaddr, cur->endaddr-cur->baseaddr)) {
			    FBT_UNPROT_DATA(cur, sizeof(struct dso_chain), ret, "action_jmp_indirect: unprotect dso failed.");
			    if(cur->nr_ijmps >= ICF_TABLES_MAX_ENTRIES) {
						llprintf("ERROR: ijmps table out of space (fbt_actions.c), nr of entries: %d\n", cur->nr_ijmps);
						fbt_exit_suicide(45);
				}
				
				int i = 0;
				int found = 0;
				/* let's see if we already have this ICF in the list */
			    for(i = 0; i < cur->nr_ijmps; i++) {
					if(addr == cur->ijmps[i]) {
						found = 1;
						break;
					}
				}
				if(!found) {
					cur->ijmps[cur->nr_ijmps] = addr;
					cur->nr_ijmps++;
				}
				
				/* TODO: make this thread safe! */
				//FBT_PROT_DATA(cur, sizeof(struct dso_chain), ret, "action_jmp_indirect: protect dso failed.");
			   break;
			}
			cur = cur->next;
		}
	
  #endif /* defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX) */
  
  if (ts->num_prefixes != 0) {
    /* no prefixes allowed */
    fbt_suicide_str("No prefixes handled in action_jmp_indirect! " \
                    "(fbt_actions.c)\n");
  }

  /* this is a fast version of the ind jmp - handoptimized assembler code
   * which does a fast lookup in the hashtable and dispatches if it hits
   * otherwise it recovers to an indirect jump
   */

  /**
   * pushl $target
   * jmpl  tld->ind_jump_trampoline
   */

  /* write: push indirect target */
  *transl_addr++ = 0xFF;

  /*
   * 0xFF expects a ModR/M byte following the opcode
   * The bits 3-5 are part of the opcode (opcode extension into ModR/M byte),
   * so we copy the ModR/M byte, but modify the opcode extension to 110
   */
  *transl_addr++ = (*first_byte_after_opcode & 0xC7) | 0x30;

  //llprintf("action_jmp_indirect: %x\n", addr);

  /* if there follows a displacement copy this to the ccf */
  if (length > 2) {
    fbt_memcpy(transl_addr, (addr + 2), length - 2);
    //llprintf("action_jmp_indirect length > 2: %x\n", addr + 2);
    transl_addr += length - 2;
  }

#if defined(VERIFY_CFTX)

#if defined(VERIFY_CFTX_ENABLE_IJMP_LSYMOPT) || defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
struct sh_symbol* symbol = fbt_find_symbol(ts->tld, addr);
#endif

#if defined(VERIFY_CFTX_ENABLE_IJMP_LSYMOPT)
if(symbol && symbol->start && symbol->size) {

  BEGIN_ASM(transl_addr)
    movl %esp, {ts->tld->stack-2}
    movl ${ts->tld->stack-2}, %esp
    //pushfl
    pushl %eax
	pushl %ecx
	
    movl 8(%esp), %eax
    movl (%eax), %eax
    movl ${symbol->start}, %ecx
    cmpl %ecx, %eax
    jl optnohit
    addl ${symbol->size}, %ecx
    cmpl %ecx, %eax
    jg optnohit
	jmp opthit
	
  optnohit:
    pushl %edx
    //pusha
#if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
    pushl ${symbol}
    pushl ${CFTX_JMP_IND}
    pushl 20(%esp) // target
    pushl ${addr}
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
    leal 20(%esp), %esp
#else
    pushl ${CFTX_JMP_IND}
    pushl 16(%esp) // target
    pushl ${addr}
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
    leal 16(%esp), %esp
#endif    
    //popa
    popl %edx
    
  opthit:
    popl %ecx
    popl %eax
    //popfl
    popl %esp
  END_ASM 

} else {

#endif /* VERIFY_CFTX_ENABLE_IJMP_LSYMOPT */

  BEGIN_ASM(transl_addr)
    movl %esp, {ts->tld->stack-2}
    movl ${ts->tld->stack-2}, %esp
    //pushfl
    pushl %eax
    pushl %ecx
    pushl %edx
    //pusha
#if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
    pushl ${symbol}
    pushl ${CFTX_JMP_IND}
    pushl 20(%esp) // target
    pushl ${addr}
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
    leal 20(%esp), %esp
#else    
    pushl ${CFTX_JMP_IND}
    pushl 16(%esp) // target
    pushl ${addr}
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
    leal 16(%esp), %esp
#endif

    //popa
    popl %edx
    popl %ecx
    popl %eax
    //popfl
    popl %esp
  END_ASM 

#if defined(VERIFY_CFTX_ENABLE_IJMP_LSYMOPT)
}
#endif

#endif /* VERIFY_CFTX */

#if !defined(ICF_PREDICT)
  JMP_REL32(transl_addr, (int32_t)(ts->tld->opt_ijump_trampoline));
  
  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft = {
    .location = transl_addr - 4,
    .original = addr
  };    
  fbt_store_cftx(ts->tld, &cft);
  #endif /* TRACK_CFTX */  

#else  /* ICF_PREDICT */
  if (ts->tld->icf_predict == NULL)
    fbt_allocate_new_icf_predictors(ts->tld);
  struct icf_prediction *pred = ts->tld->icf_predict;
  ts->tld->icf_predict = pred->pred.next;

  pred->pred.src = transl_addr;
  
  BEGIN_ASM(transl_addr)
    pushfl
    cmpl $l0x0, 4(%esp)
  END_ASM

  pred->origin1 = (ulong_t*)(transl_addr - 4);

  BEGIN_ASM(transl_addr)
    jne nohit
    popfl
    leal 4(%esp), %esp
    jmp_abs {transl_addr-4}
  nohit:
    // NO HIT, we need to fix the prediction
  END_ASM

  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft = {
    .location = transl_addr - 4,
    .original = addr
  };  
  fbt_store_cftx(ts->tld, &cft);
  #endif /* TRACK_CFTX */  

  pred->dst1 = (ulong_t*)(transl_addr - 4);
  
  BEGIN_ASM(transl_addr)
    movl ${pred}, {ts->tld->stack-11}    
    jmp_abs {ts->tld->opt_ijump_predict_fixup}
  END_ASM

  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft2 = {
    .location = transl_addr - 4,
    .original = addr
  };    
  fbt_store_cftx(ts->tld, &cft2);
  #endif /* TRACK_CFTX */

#endif  /* ICF_PREDICT */
  
  PRINT_DEBUG_FUNCTION_END("-> close, transl_length=%i",
                           transl_addr - ts->transl_instr);
  ts->transl_instr = transl_addr;
  
  return CLOSE;
}

enum translation_state action_jcc(struct translate *ts) {
  
  unsigned char *addr = ts->cur_instr;
  unsigned char* transl_addr = ts->transl_instr;
  int length = ts->next_instr - ts->cur_instr;

  unsigned char *original_addr = addr;

  unsigned char *virtual_fallthrough = ts->next_instr;

  PRINT_DEBUG_FUNCTION_START("action_jcc(*addr=%p, *transl_addr=%p, length=%i)",
                             addr, transl_addr, length);
#if defined(FBT_STATISTIC)
  fbt_nr_translated_jcc++;
#endif
  ulong_t jump_target;
  ulong_t fallthru_target;
  void *transl_target;

  if (ts->num_prefixes != 0) {
    llprintf("Instruction at %p uses prefixes (len: %d)!\n", addr, length);
    if (*addr == PREFIX_FS_SEG_OVR) {
      addr++;
      length--;
    } else
      fbt_suicide_str("No prefixes handled in action_jcc! (fbt_actions.c)\n");
  }  
  #if defined(ASSERTIONS)
  assert(((*addr == 0x0F && length == 6) || (length == 2)));
  #endif
  /* no prefixes handled */

  /* check if we have jecxz (jump if ecx register is zero) */
  if (*addr == 0xE3) {
    PRINT_DEBUG("processing jecxz");

    /* This is a little tricky. because JECXZ has only a 8bit offset we can not
       jmp directly to the trampoline therefore two unconditional jumps are
       inserted. first the unconditional jmp for the fall through target and
       then the jmp for the original jump target. A JECXZ inst is then used to
       jump over the jump of the fall through target if the RCX register is
       zero */

    /* calculate the jump targets */
    fallthru_target = ((ulong_t)original_addr) + length;
    jump_target = *((char*) (addr + 1)) + fallthru_target;

    /* insert a jecxz to jump over the fall through jump if CX is 0 */
    JECXZ_I8(transl_addr, 0x05);

    /* write: jump to trampoline for fallthrough address */
    /* create trampoline if one is needed, otherwise lookup and go */
    #if defined(TRACK_CFTX)
    struct control_flow_transfer cft = {
      .location = transl_addr - 4,
      .original = addr
    };    
    fbt_store_cftx(ts->tld, &cft);
    #endif /* TRACK_CFTX */

    transl_target = fbt_ccache_find(ts->tld, (void*)virtual_fallthrough);
    if ( transl_target != NULL ) {
      BEGIN_ASM(transl_addr)
        jmp_abs {transl_target}
      END_ASM
    } else { 
      struct trampoline *trampo =
        fbt_create_trampoline(ts->tld, (void*)virtual_fallthrough,
                              (void*)((ulong_t)(transl_addr)+1), ORIGIN_RELATIVE);
      BEGIN_ASM(transl_addr)
        jmp_abs {trampo->code}
      END_ASM
    }

    /* if we have an jecxz then the jump target comes second (switchted)
       but we can use the code from the normal jcc, so we don't need to copy
       the jmp_rel32 and all */
    fallthru_target = jump_target;
  } else {
    int16_t jcc_type;
    if (*(unsigned char*) addr != 0x0F) {
      /* find out if we have a one-byte or a two-byte opcode */
      PRINT_DEBUG("processing one-byte jcc");
      /* write two-byte jcc equivalent to the one-byte jcc */
      /* the trick is: opcode + 0x10 = second byte of twobyte Jcc instruction */
      jcc_type = 0x0F + ((*addr+0x10)<<8);

      fallthru_target = ((ulong_t)original_addr) + length;
      jump_target = *((char*) (addr + 1)) + fallthru_target;

    } else {
      PRINT_DEBUG("processing two-byte jcc");
      /* write: copy of two-byte jcc */
      jcc_type = *((int16_t*)addr);

      fallthru_target = ((ulong_t)original_addr) + length;
      jump_target = *((ulong_t*) (addr + 2)) + fallthru_target;

    }
  
    /* write: jump address to trampoline; create trampoline if one is needed,
       otherwise lookup and go */         
    transl_target = fbt_ccache_find(ts->tld, (void*)jump_target);
    if ( transl_target != NULL ) {
      JCC_2B(transl_addr, jcc_type, (ulong_t)transl_target);
    } else {
      struct trampoline *trampo =
        fbt_create_trampoline(ts->tld, (void*)jump_target,
                              (void*)(((ulong_t)transl_addr)+2), ORIGIN_RELATIVE);
      JCC_2B(transl_addr, jcc_type, (ulong_t)(trampo->code));
    }

    #if defined(TRACK_CFTX)
    struct control_flow_transfer cft = {
      .location = transl_addr - 4,
      .original = addr
    };    
    fbt_store_cftx(ts->tld, &cft);
    #endif /* TRACK_CFTX */


  }
  
  /* write: jump to trampoline for fallthrough address */

  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft = {
    .location = transl_addr - 4,
    .original = addr
  };    
  fbt_store_cftx(ts->tld, &cft);
  #endif /* TRACK_CFTX */

  transl_target = fbt_ccache_find(ts->tld, (void*)fallthru_target);
  if ( transl_target != NULL ) {
    JMP_REL32(transl_addr, (ulong_t)transl_target);
  } else {
    struct trampoline *trampo =
      fbt_create_trampoline(ts->tld, (void*)fallthru_target,
                            (void*)((ulong_t)(transl_addr)+1), ORIGIN_RELATIVE);
    BEGIN_ASM(transl_addr)
        jmp_abs {trampo->code}
    END_ASM
  }

  PRINT_DEBUG_FUNCTION_END("-> close, transl_length=%i",
                           transl_addr - ts->transl_instr);
  ts->transl_instr = transl_addr;
  return CLOSE;
}

enum translation_state action_call(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;

  unsigned char *original_addr = addr;
  
  unsigned char* transl_addr = ts->transl_instr;
  int length = ts->next_instr - ts->cur_instr;

  unsigned char *return_addr = ts->next_instr;
  unsigned char *next_instr = ts->next_instr;

  PRINT_DEBUG_FUNCTION_START("action_call(*addr=%p, *transl_addr=%p," \
                             " length=%i)", addr, transl_addr, length);
#if defined(FBT_STATISTIC)
  fbt_nr_translated_call++;
#endif

  /* total length of a call we handle must be 5, otherwise we have prefixes and
     such in our stream */
  #if defined(ASSERTIONS)
  assert(length == 5 && !HAS_PREFIX(*addr));
  #endif
  
  /* our opcode should be 0xE8, a near relative call */

  /* read call argument (32bit immediate) and add EIP (EIP = addr + length) to
     argument --> absolute target address */
  ulong_t call_target = *((ulong_t*) (addr + 1)) + (ulong_t) original_addr + length;
  PRINT_DEBUG("original call_target: %p", (void*)call_target);

  /* let's check if this call only wants to get the EIP, following conditions
   * must hold:
   *  - the next instruction is called (imm. following, so rel. addr is 0x0
   *  - the next instruction is a pop %reg.
   * asm code: call next_instr, pop %reg
   */
  if (*((ulong_t*) (addr + 1)) == 0x0 && *(next_instr) >= 0x58 && *(next_instr) <= 0x5F) {
    PUSHL_IMM32(transl_addr, (int32_t)return_addr);
    PRINT_DEBUG_FUNCTION_END("-> open, transl_length=%i",
                             transl_addr - ts->transl_instr);
    ts->transl_instr = transl_addr;

    return OPEN;
  }
  
  /* write: push original EIP (we have to do this either way) */
  // TODO: 64bit needs a 64bit push!
  int32_t return_address = (int32_t)return_addr;
  PRINT_DEBUG("return_address = %x\n", return_address);
  PUSHL_IMM32(transl_addr, return_address);

#if defined(FBT_STATISTIC)
  INCL_M64(transl_addr, (int32_t)&fbt_nr_call);
#endif

#if defined(INLINE_CALLS)
  if (ts->inline_call_RIP != NULL) {
    /* are we inlining the current function? if so, then we bail out and
        * let the translate function handle the recursion */
    PRINT_DEBUG_FUNCTION_END("-> open, inlined transl_length=%i", transl_addr -
                             ts->transl_instr);
    ts->transl_instr = transl_addr;
    /* No need to actually call the function.
       Simply change the next instr pointer to the first instr of the function.
       This will put the body of the function right as the next instr in the
       translated code */
    ts->next_instr = (unsigned char*)call_target;
    return OPEN;
  }
#endif

#if defined(SHADOWSTACK)
  /*		(pushl eip)				# see before #if
   *        ...
   * 		([jmp ...])				# see after #endif
   */
  BEGIN_ASM(transl_addr)
    pushl %ebx
    movl {&ts->tld->top_of_shadowstack}, %ebx
  END_ASM
  
  /* If the next instruction has already been translated, we use the address
  of the translated code segment, otherwise we create a trampoline in BT space
  and use its address */
  
  void *transl_next = fbt_ccache_find(ts->tld, ts->next_instr);
  
  /* We want to take advantage of MODRM, so we hardcode the offset */
  #if defined(ASSERTIONS)
  assert(offsetof(struct shadowstack_entry, translated_return_address) == 0);
  #endif
  
  if (transl_next == NULL) {
    /* next_instr + 2 as movl + modrm is 2 bytes long */  
    struct trampoline *trampo =
      fbt_create_trampoline(ts->tld, (void*)(ts->next_instr),
                            (void*)((ulong_t)(transl_addr)+2),
                            ORIGIN_SHADOWSTACK);
    BEGIN_ASM(transl_addr)
        movl ${trampo->code}, (%ebx) // addr backpatched later on
    END_ASM
  } else {
    BEGIN_ASM(transl_addr)  
        movl ${transl_next}, (%ebx) // addr backpatched later on
    END_ASM
  }

  BEGIN_ASM(transl_addr)
    //movl %ebp, SHADOWSTACK_ENTRY_OFFSETOF_BASE_POINTER(%ebx)
    leal 8(%esp), %esp
    movl %esp, SHADOWSTACK_ENTRY_OFFSETOF_STACK_POINTER(%ebx)
    leal -8(%esp), %esp
    movl ${ts->next_instr}, SHADOWSTACK_ENTRY_OFFSETOF_RETURN_ADDRESS(%ebx)
    addl $ SHADOWSTACK_ENTRY_SIZE, {&ts->tld->top_of_shadowstack}
    popl %ebx
  END_ASM
  
  #if defined(SHADOWSTACK_DEBUG)
  BEGIN_ASM(transl_addr)
    movl %esp, {ts->tld->stack-1}
    movl ${ts->tld->stack-1}, %esp
    
    pusha
    pushl ${call_target}
    pushl ${ts->cur_instr}
    pushl ${ts->tld}
    
    call_abs {fbt_shadowstack_debug_call}
    
    leal 12(%esp), %esp
    popa

    popl %esp
  END_ASM
  #endif /* SHADOWSTACK_DEBUG */
  
#endif /* SHADOWSTACK */

#if defined(INLINE_PLT_CALLS)
/* Check if call targets plt section and resolve destination address */
unsigned long resolved_addr = sl_resolve_plt_call(call_target, ts->tld->dso_objects);
if (resolved_addr != call_target) {
  /* PLT call, check transfer and inline it */
  #if defined(VERIFY_CFTX)
  
  #if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
  struct sh_symbol* symbol = fbt_find_symbol(ts->tld, addr);
  fbt_check_transfer(ts->tld, original_addr, (unsigned char*)&resolved_addr, CFTX_CALL_IND, symbol); /* treat like an indirect call */
  #else
  fbt_check_transfer(ts->tld, original_addr, (unsigned char*)&resolved_addr, CFTX_CALL_IND); /* treat like an indirect call */
  #endif  

  #endif /* VERIFY_CFTX */
  call_target = resolved_addr;
}
#endif

  /* check if target is already translated; if not, do so now */
  void *transl_target = fbt_ccache_find(ts->tld, (void*)call_target);
  
  if (transl_target == NULL) {
    /* we still have to translate the call target */
    PRINT_DEBUG_FUNCTION_END("-> open, transl_length=%i",
                             transl_addr - ts->transl_instr);
    ts->transl_instr = transl_addr;
    /* No need to actually call the function.
       Simply change the next instr pointer to the first instr of the function.
       This will put the body of the function right as the next instr in the
       translated code */
    ts->next_instr = (unsigned char*)call_target;
    
    /* put the target into the tcache so later calls can use the translated
       code */
    #ifndef TRACK_BASIC_BLOCKS
    //fbt_ccache_add_entry(ts->tld, (void*)call_target, ts->transl_instr);
    #endif
    
    return OPEN;
  }

  /* target is already translated */
  PRINT_DEBUG("translated call_target: %p", transl_target);

  /* write: jump instruction to translated target */    
  BEGIN_ASM(transl_addr)
    jmp_abs {transl_target}
  END_ASM
  
  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft = {
    .location = transl_addr - 4,
    .original = addr
  };    
  fbt_store_cftx(ts->tld, &cft);
  #endif /* TRACK_CFTX */

  PRINT_DEBUG_FUNCTION_END("-> close, transl_length=%i",
                           transl_addr - ts->transl_instr);
  ts->transl_instr = transl_addr;
  return CLOSE;
}

enum translation_state action_call_indirect(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;
  unsigned char *first_byte_after_opcode = ts->first_byte_after_opcode;
  unsigned char* transl_addr = ts->transl_instr;
  int length = ts->next_instr - ts->cur_instr;
  int prefix_length = 0;

  #if defined(TRACK_CFTX) || defined(SHADOWSTACK_DEBUG) || defined(VERIFY_CFTX)
  unsigned char *original_addr = addr;
  #endif
  
  #if defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX)
  	struct dso_chain *cur = ts->tld->dso_objects;
  	long ret;
  	
		while (cur != 0) {
			/* is the pointer in the current dso? */
            if (PTR_IN_REGION(addr, cur->baseaddr, cur->endaddr-cur->baseaddr)) {
			    FBT_UNPROT_DATA(cur, sizeof(struct dso_chain), ret, "action_call_indirect: unprotect dso failed.");
			    if(cur->nr_icalls >= ICF_TABLES_MAX_ENTRIES) {
						llprintf("ERROR: icalls table out of space (fbt_actions.c), nr of entries: %d\n", cur->nr_icalls);
						fbt_exit_suicide(45);
				}
				
				int i = 0;
				int found = 0;
				/* let's see if we already have this ICF in the list */
			    for(i = 0; i < cur->nr_icalls; i++) {
					if(addr == cur->icalls[i]) {
						found = 1;
						break;
					}
				}
				if(!found) {
					cur->icalls[cur->nr_icalls] = addr;
					cur->nr_icalls++;
				}
				/* TODO: make this thread safe! */
				//FBT_PROT_DATA(cur, sizeof(struct dso_chain), ret, "action_call_indirect: protect dso failed.");
			   break;
			}
			cur = cur->next;
		}
	
  #endif /* defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX) */

  unsigned char *return_addr = ts->next_instr;

  PRINT_DEBUG_FUNCTION_START("action_call_indirect(*addr=%p, *transl_addr=%p," \
                             " length=%i)", ts->cur_instr, ts->transl_instr,
                             ts->next_instr - ts->cur_instr);
  
#if defined(FBT_STATISTIC)
  fbt_nr_translated_call_ind++;
  /* WARNING: this statement destroys the FLAGS */
  INCL_M64(ts->transl_addr, (int32_t)&fbt_nr_ind_calls);
#endif

#if defined(SHADOWSTACK)
  BEGIN_ASM(transl_addr)
    pushl %ebx
    movl {&ts->tld->top_of_shadowstack}, %ebx
  END_ASM
  
  /* If the next instruction has already been translated, we use the address
  of the translated code segment, otherwise we create a trampoline in BT space
  and use its address */
  void *transl_next = fbt_ccache_find(ts->tld, return_addr);
  
  /* We want to take advantage of MODRM, so we hardcode the offset */
  #if defined(ASSERTIONS)
  assert(offsetof(struct shadowstack_entry, translated_return_address) == 0);
  #endif
  
  if (transl_next == NULL) {
    /* next_instr + 2 as movl + modrm is 2 bytes long */  
    struct trampoline *trampo =
      fbt_create_trampoline(ts->tld, (void*)(return_addr),
                            (void*)((ulong_t)(transl_addr)+2),
                            ORIGIN_SHADOWSTACK);
    BEGIN_ASM(transl_addr)
        movl ${trampo->code}, (%ebx) // addr backpatched later on
    END_ASM
  } else { /* TODO: BUG FOUND, to be verified */
    BEGIN_ASM(transl_addr)  
        movl ${transl_next}, (%ebx)
    END_ASM
    /* Old buggy version, uses return_addr instead of transl_next, when is the backpatching supposed to happen? */
    /*BEGIN_ASM(transl_addr)  
        movl ${return_addr}, (%ebx) // addr backpatched later on
    END_ASM*/
  }

  BEGIN_ASM(transl_addr)
    //movl %ebp, SHADOWSTACK_ENTRY_OFFSETOF_BASE_POINTER(%ebx)
    leal 4(%esp), %esp
    movl %esp, SHADOWSTACK_ENTRY_OFFSETOF_STACK_POINTER(%ebx)
    leal -4(%esp), %esp
    movl ${return_addr}, SHADOWSTACK_ENTRY_OFFSETOF_RETURN_ADDRESS(%ebx)
    addl $ SHADOWSTACK_ENTRY_SIZE, {&ts->tld->top_of_shadowstack}
    popl %ebx
  END_ASM
#endif /* SHADOWSTACK */

  /* write: push original EIP */
  PUSHL_IMM32(transl_addr, (ulong_t)return_addr);
  PRINT_DEBUG("original eip: %p", return_addr);

  /*
   * check for prefixes:
   * we handle only segment override prefixes, all others produce an error
   */
  while (HAS_PREFIX(*addr)) {
    /* we don't handle 16bit calls and strange prefixes */
    #if defined(ASSERTIONS)
    assert(*addr!=PREFIX_LOCK && *addr!=PREFIX_REPNEZ && *addr!=PREFIX_MISC &&
           *addr!=PREFIX_ADDR_SZ_OVR && *addr!=PREFIX_OP_SZ_OVR);
    #endif
    /* copy the freaking prefix! */
    PRINT_DEBUG("Copying prefix: 0x%x\n", *addr);
    *transl_addr++ = *addr++;
    prefix_length++;
  }

  /* write: push indirect target */
  *transl_addr++ = 0xFF;

  /*
   * 0xFF expects a ModR/M byte following the opcode
   * The bits 3-5 are part of the opcode (opcode extension into ModR/M byte),
   * so we copy the ModR/M byte, but modify the opcode extension to 110
   */
  *transl_addr++ = (*(first_byte_after_opcode) & 0xC7) | 0x30;

  /* if there follows a SIB byte and/or displacement, copy this to the CCF */
  if ((length - prefix_length) > 2) {
    fbt_memcpy(transl_addr, (first_byte_after_opcode + 1), length - 2 -
               prefix_length);
    transl_addr += length - 2 - prefix_length;

    /**
     * DANGER: we have a SIB replacement
     * if we have e.g. call *0x44(%esp) or call *(%esp) then we have a
     * problem, because we change %esp by pushing the original EIP
     * onto it beforehand!
     * So we must introduce an additional displacement of 4 bytes to
     * get the right offset
     */
    char modrm = (*(first_byte_after_opcode) & 0xC7) | 0x30;
    char sib = *(first_byte_after_opcode+1);
    
    if (((modrm&0x7)==4) && ((sib&0x7)==4)) {
      /* ModR/M SIB replacement (R/M==0x4) */
      /* SIB Byte (Base) == 0x4 -> ESP */
      /* if we do not need ESP then we are lucky and can continue, otherwise, we
         are screwed and have to take counter measures. For 1B and 4B
         displacement we ensure that we can add 4B to the final displacement
         value. 0B displacements are re-written to 1B displacements with 0x4 as
         a total displacement.
      */
      /* 0B displacement with %esp -> call *0xff(%esp) */
      if (((modrm>>6)&0x3)==0) {
        /* just change the displacement ModRM to 1B and make it 4! */
        *(transl_addr-2) = (*(transl_addr-2) & 0x3f) | 0x40;
        *(transl_addr++) = 0x4;
      }
      /* 1B displacement with %esp -> call *0xff(%esp) */
      /* NOTE: there is the special case *0x7c(%esp)
       * 		- 0x7c+4 = 0x80 = 128 
       * 		- 0x80 has to be extended to a 4b displacement, else it will be translated to a "pushl  -0x80(%esp)"
       * 		- see examples below
       * 80483bf:	ff b4 24 80 00 00 00 	pushl  0x80(%esp)
	   * 80483c6:	ff 74 24 80          	pushl  -0x80(%esp) */
      if (((modrm>>6)&0x3)==1) {
        /* just change the following displacement and add 4 to it! */
        #if defined(ASSERTIONS)
        assert((*(transl_addr-1)) < ((*(transl_addr-1)) + 4));
        #endif
        if(*(transl_addr-1) == 0x7c) {
			*(transl_addr-1) = (*(transl_addr-1))+4;
			/* Add the additional 3x0x00 bytes */
			*transl_addr++ = 0x00;
			*transl_addr++ = 0x00;
			*transl_addr++ = 0x00;
			/* As we extended the displacement from 1B to 4B we need to adapt the ModRM byte
			 * i.e. Mod = 01 to Mod = 10 so that ModRM == 0xb4 (0x74 before) */
			*(transl_addr-6) = 0xb4;
		}
        else {
			*(transl_addr-1) = (*(transl_addr-1))+4;
		}
      }
      /* 4B displacement with %esp -> call *0xaabbccdd(%esp) */
      else if (((modrm>>6)&0x3)==2) {
        /* just change the following displacement and add 4 to it! */
        #if defined(ASSERTIONS)
        assert((*((long*)(transl_addr-4))) < ((*((long*)(transl_addr-4)))+4));
        #endif
        *((long*)(transl_addr-4)) = (*((long*)(transl_addr-4)))+4;
      }
      /* Mod=11 with SIB not handled for calls -> assert & die */
      #if defined(ASSERTIONS)
      assert(((modrm>>6)&0x3)!=3);
      #endif
    }
  }

#if defined(VERIFY_CFTX)

#if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
struct sh_symbol* symbol = fbt_find_symbol(ts->tld, addr);
#endif

#if defined(VERIFY_CFTX_ENABLE_CACHE)

  #if !(defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX))
    struct dso_chain *cur;
  #endif

  	cur = ts->tld->dso_objects;
  	char* cache_ptr = NULL;

	while (cur != 0) {
		/* is the pointer in the current dso? */
        if (PTR_IN_REGION(addr, cur->baseaddr, cur->endaddr-cur->baseaddr)) {
			cache_ptr = cur->cft_cache_pool_next;
			if(cache_ptr + sizeof(char*) < (cur->cft_cache_pool + CFT_CACHE_SIZE)) {
				cur->cft_cache_pool_next = cache_ptr + sizeof(char*);
			} else {
				llprintf("ERROR: cft cache pool is full!\n");
				fbt_exit_suicide(1);
			}
			break;
		}
		cur = cur->next;
	}

  /* switch to secured stack */
  BEGIN_ASM(transl_addr)
    // cmp value at %esp with value in cache
	// if equal, skip fbt_check_transfer i.e. go to cfihit
    movl %esp, {ts->tld->stack-2} // this value is referenced later by 16(%esp)
    movl ${ts->tld->stack-2}, %esp
    pushl %eax
    pushl %ecx
	movl {cache_ptr}, %eax
	movl 8(%esp), %ecx
	movl (%ecx), %ecx
	cmpl %eax, %ecx
    je cfihit
    pushl %edx
    //pusha
#if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
    pushl ${symbol}
    pushl ${CFTX_CALL_IND}
    pushl 20(%esp) // target, dest addr -> 16(%esp) is the old %esp and on top of it the indirect target is found i.e. the pointer to the real call target! 
    pushl ${original_addr} // source addr
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
	movl 32(%esp), %eax
	movl (%eax), %eax
	movl %eax, {cache_ptr}
    leal 20(%esp), %esp
#else    
    pushl ${CFTX_CALL_IND}
    pushl 16(%esp) // target, dest addr -> 16(%esp) is the old %esp and on top of it the indirect target is found i.e. the pointer to the real call target! 
    pushl ${original_addr} // source addr
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
	movl 28(%esp), %eax
	movl (%eax), %eax
	movl %eax, {cache_ptr}
    leal 16(%esp), %esp
#endif
    //popa
    popl %edx
 
   cfihit:
    popl %ecx
    popl %eax
    popl %esp
 
  END_ASM
  
#else /* WITHOUT CFTX CACHE */

  /* switch to secured stack */
  BEGIN_ASM(transl_addr)
    movl %esp, {ts->tld->stack-2} // this value is referenced later by 16(%esp)
    movl ${ts->tld->stack-2}, %esp
    pushl %eax
    pushl %ecx
    pushl %edx
    //pusha
#if defined(ENABLE_TRANSLATION_TIME_SYMBOL_LOOKUP)
    pushl ${symbol}
    pushl ${CFTX_CALL_IND}
    pushl 20(%esp) // target, dest addr -> 16(%esp) is the old %esp and on top of it the indirect target is found i.e. the pointer to the real call target! 
    pushl ${original_addr} // source addr
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
    leal 20(%esp), %esp
#else
    pushl ${CFTX_CALL_IND}
    pushl 16(%esp) // target, dest addr -> 16(%esp) is the old %esp and on top of it the indirect target is found i.e. the pointer to the real call target! 
    pushl ${original_addr} // source addr
    pushl ${ts->tld}
    call_abs {&fbt_check_transfer}
    leal 16(%esp), %esp
#endif
    //popa
    popl %edx
    popl %ecx
    popl %eax
    
    popl %esp
  END_ASM
  
#endif

#endif /* VERIFY_CFTX */

#if defined(SHADOWSTACK_DEBUG)
  BEGIN_ASM(transl_addr)
    movl %esp, {ts->tld->stack-2}
    movl ${ts->tld->stack-2}, %esp

    pusha

    pushl 32(%esp)
    pushl ${original_addr}
    pushl ${ts->tld}
    
    call_abs {fbt_shadowstack_debug_call}
    
    leal 12(%esp), %esp
    popa
    
    popl %esp
  END_ASM
#endif /* SHADOWSTACK_DEBUG */  

#if !defined(ICF_PREDICT)
  /* write: jump instruction to trampoline */
  BEGIN_ASM(transl_addr)
    jmp_abs {ts->tld->opt_icall_trampoline}
  END_ASM

#else  /* ICF_PREDICT */
  if (ts->tld->icf_predict == NULL)
    fbt_allocate_new_icf_predictors(ts->tld);
  struct icf_prediction *pred = ts->tld->icf_predict;
  ts->tld->icf_predict = pred->pred.next;

  pred->pred.src = transl_addr;
  
  BEGIN_ASM(transl_addr)
    pushfl
    cmpl $l0x0, 4(%esp)
  END_ASM

  pred->origin1 = (ulong_t*)(transl_addr - 4);

  BEGIN_ASM(transl_addr)
    jne nohit
    popfl
    leal 4(%esp), %esp
    jmp_abs {transl_addr+4}
  nohit:
    // NO HIT, we need to fix the prediction
  END_ASM

  #if defined(TRACK_CFTX)
  struct control_flow_transfer cft = {
    .location = transl_addr - 4,
    .original = original_addr
  };
  fbt_store_cftx(ts->tld, &cft);
  #endif /* TRACK_CFTX */

  pred->dst1 = (ulong_t*)(transl_addr - 4);

  BEGIN_ASM(transl_addr)
    movl ${pred}, {ts->tld->stack-11}    
    jmp_abs {ts->tld->opt_icall_predict_fixup}
  END_ASM
  
  #if defined(TRACK_CFTX)
  {
    struct control_flow_transfer cft = {
      .location = transl_addr - 4,
      .original = original_addr
    };    
    fbt_store_cftx(ts->tld, &cft);
  }
  #endif /* TRACK_CFTX */  

#endif  /* ICF_PREDICT */
  
  ts->transl_instr = transl_addr;
  PRINT_DEBUG_FUNCTION_END("-> close");
  return CLOSE;
}

enum translation_state action_sysenter(struct translate *ts) {
  unsigned char* transl_addr = ts->transl_instr;
#ifdef DEBUG
  unsigned char *addr = ts->cur_instr;
  int length = ts->next_instr - ts->cur_instr;
  PRINT_DEBUG_FUNCTION_START("action_sysenter(*addr=%p, *transl_addr=%p, " \
                             "length=%i)", addr, transl_addr, length);
#endif        
#if defined(AUTHORIZE_SYSCALLS)
  /* store location of this syscall */
  MOVL_IMM32_MEM32(transl_addr, 0x05, ts->cur_instr,
                   &(ts->tld->syscall_location));
#endif
  /* write: jump instruction to trampoline */
  JMP_REL32(transl_addr, (ulong_t)(ts->tld->sysenter_trampoline));
  
  ts->transl_instr = transl_addr;
  PRINT_DEBUG_FUNCTION_END("-> close");
  return CLOSE;
}

enum translation_state action_ret(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;
  unsigned char* transl_addr = ts->transl_instr;
  unsigned char *first_byte_after_opcode = ts->first_byte_after_opcode;
#ifdef DEBUG
  int length = ts->next_instr - ts->cur_instr;
  PRINT_DEBUG_FUNCTION_START("action_ret(*addr=%p, *transl_addr=%p, length=%i)",
                             addr, transl_addr, length);

  /* ret plus evtl imm16 - no opcode prefixes */
  #if defined(ASSERTIONS)
  assert((!HAS_PREFIX(*addr) && ((*addr==0xC2 && length==3) ||          \
                                 (*addr==0xC3 && length==1))) ||        \
         (*addr==PREFIX_MISC &&  ((*(addr+1)==0xC2 && length==4) ||     \
                                  (*(addr+1)==0xC3 && length==2))));
  #endif
  /* see: http://readlist.com/lists/gcc.gnu.org/gcc-help/1/8765.html */
  if (!(!HAS_PREFIX(*addr) && ((*addr==0xC2 && length==3) || \
                               (*addr==0xC3 && length==1))) \
      && *addr==PREFIX_MISC) {
    PRINT_DEBUG("Useless REPZ Prefix found (and removed) in RET (ugly gcc " \
                "hack for Athlon and K8)");
  }
#endif
  // useless prefix found, skip to the real ret instr
  if (*addr == PREFIX_MISC) {
    addr++;
  }

  #if defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX)
  	struct dso_chain *cur = ts->tld->dso_objects;
  	long ret;
  	
		while (cur != 0) {
			/* is the pointer in the current dso? */
            if (PTR_IN_REGION(addr, cur->baseaddr, cur->endaddr-cur->baseaddr)) {
			    FBT_UNPROT_DATA(cur, sizeof(struct dso_chain), ret, "action_ret: unprotect dso failed.");
			    if(cur->nr_retinstrs >= ICF_TABLES_MAX_ENTRIES) {
						llprintf("ERROR: ijmps table out of space (fbt_actions.c), nr of entries: %d\n", cur->nr_retinstrs);
						fbt_exit_suicide(45);
				}
				
				int i = 0;
				int found = 0;
				/* let's see if we already have this ICF in the list */
			    for(i = 0; i < cur->nr_retinstrs; i++) {
					if(addr == cur->retinstrs[i]) {
						found = 1;
						break;
					}
				}
				if(!found) {
					cur->retinstrs[cur->nr_retinstrs] = addr;
					cur->nr_retinstrs++;
				}
				
				/* TODO: make this thread safe! */
				//FBT_PROT_DATA(cur, sizeof(struct dso_chain), ret, "action_ret: protect dso failed.");
			   break;
			}
			cur = cur->next;
		}
	
  #endif /* defined(SECURITY_METRICS_AND_STATS) && defined(VERIFY_CFTX) */

#if defined(INLINE_CALLS)
  /* are we currently inlining a function
   * and do we need to unwrap the current stack frame
   * (e.g. fast return to the callee instead of going
   * through the ind_jump feature)
   */
  if (ts->inline_call_RIP != NULL) {
    /* restore the stack to the point before the call */
    if (*addr == 0xC2) {
      /* leal 4+$imm(%esp), %esp */
      int16_t rem_bytes = *((int16_t*)first_byte_after_opcode);
      PRINT_DEBUG("we must remove additional bytes: %d\n", rem_bytes);
      if (rem_bytes < 0) {
        fbt_suicide_str("Ret removes a negative amount of bytes, this is "
                        "illegal! (fbt_actions.c)\n");
      }
      LEAL_IMM32RM32_R(transl_addr, 0xa4, 0x24, 0x04+(int32_t)(rem_bytes));
    } else {
      BEGIN_ASM(transl_addr)
        leal 4(%esp), %esp
      END_ASM
    }

    ts->next_instr = ts->inline_call_RIP;
    ts->inline_call_RIP = NULL;

#if defined(FBT_STATISTIC)
    fbt_nr_trans_inlined_calls++;
    INCL_M64(transl_addr, (int32_t)&fbt_nr_inlined_calls);
#endif
    PRINT_DEBUG_FUNCTION_END("-> open, inlined, transl_length=%i",
                             transl_addr - ts->transl_instr);
    
    ts->transl_instr = transl_addr;
    return OPEN;
  }
#endif

/* check if we have to dump valid ret targets */
#if defined(LOG_VALID_RET_TARGETS)
#if defined(LOG_RET_INSTR)
  if(addr == LOG_RET_INSTR) {
#endif
  BEGIN_ASM(transl_addr)
      movl %esp, {ts->tld->stack-2}
      movl ${ts->tld->stack-2}, %esp
      pusha
      pushl ${CFTX_RET}
      pushl 36(%esp) // target
      pushl ${addr}
      pushl ${ts->tld}
      call_abs {&fbt_log_valid_ret_targets}
      leal 16(%esp), %esp
      popa
      popl %esp
  END_ASM
#if defined(LOG_RET_INSTR)
  }
#endif
#endif

  /*
   * This is the simple case, we just replace the ret
   * with an indirect jump that translates the control flow
   * back to the callee (no optimization)
   */
  int32_t jmp_target = 0;
  if (*addr == 0xc2) {
    /* this ret wants to pop some bytes of the stack */
    int16_t rem_bytes = *((int16_t*)first_byte_after_opcode);
    PRINT_DEBUG("we must remove additional bytes: %d\n", rem_bytes);
    if (rem_bytes < 0) {
      fbt_suicide_str("Ret removes a negative amount of bytes, this is "
                      "illegal! (fbt_actions.c)\n");
    }
    PUSHL_IMM32(transl_addr, (int32_t)rem_bytes);
    jmp_target = (int32_t)(ts->tld->opt_ret_remove_trampoline);
  }

  if (*addr == 0xc3) {
    jmp_target = (int32_t)(ts->tld->opt_ret_trampoline);
  }
  
#if defined(SHADOWSTACK) & defined(SHADOWSTACK_IGNORE_LOADER)
  /* To give us more information when verifying return instructions,
     we store the address we are returning from */
  BEGIN_ASM(transl_addr)
    movl ${ts->cur_instr}, {&ts->tld->shadowstack_return_origin}
  END_ASM
#endif /* SHADOWSTACK */

  /* write: jump instruction to trampoline */
  BEGIN_ASM(transl_addr)
    jmp_abs {jmp_target}
  END_ASM

  PRINT_DEBUG_FUNCTION_END("-> close, transl_length=%i",
                           transl_addr - ts->transl_instr);
  ts->transl_instr = transl_addr;
  return CLOSE;
}

enum translation_state action_lea(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;

  PRINT_DEBUG_FUNCTION_START("action_lea(*addr=%p, *transl_addr=%p, "
                             "length=%i)", addr, transl_addr, length);
  
  #if defined(VERIFY_CFTX) && defined(CALLBACK_LEA_DETECTION)      
  /* we are looking for: "lea disp(%ebx),%eax" instructions */
  /*
  12c8a3:	8d 83 e7 be fb ff    	lea    -0x44119(%ebx),%eax
  12eec3:	8d bb f1 d3 fb ff    	lea    -0x42c0f(%ebx),%edi
  131468:	8d 8b fc 73 fb ff    	lea    -0x48c04(%ebx),%ecx
  14c4fe:	8d 93 2c 65 fa ff    	lea    -0x59ad4(%ebx),%edx
  14c95e:	8d ab 2b bc fb ff    	lea    -0x443d5(%ebx),%ebp
  14ccd0:	8d b3 e4 e1 ff ff    	lea    -0x1e1c(%ebx),%esi
  */
  if(*ts->first_byte_after_opcode == 0x83 || *ts->first_byte_after_opcode == 0xbb || *ts->first_byte_after_opcode == 0x8b || *ts->first_byte_after_opcode == 0x93
   || *ts->first_byte_after_opcode == 0xab || *ts->first_byte_after_opcode == 0xb3) {
	  
	    struct dso_chain *cur = ts->tld->dso_objects;
		unsigned char *disp_addr = ts->first_byte_after_opcode;
		disp_addr++;
		
		/* check if it points to any executable memory within the same DSO */
		while (cur != 0) {
			
			/* is the pointer in the current dso? */
            if (PTR_IN_REGION(addr, cur->baseaddr, cur->endaddr-cur->baseaddr)) {
				  long disp = *((long*)disp_addr);
				  unsigned char *possible_callback = (unsigned char*)((long)cur->gotplt + disp);
				  
				  /* check if the obtained address points to executable memory (potential callback target) */
				  if (PTR_IN_REGION(possible_callback, cur->text, cur->text_size)
						|| PTR_IN_REGION(possible_callback, cur->init, cur->init_size)
						|| PTR_IN_REGION(possible_callback, cur->fini, cur->fini_size)) {
					
						fbt_add_callback(cur, (void*)possible_callback);
					
				  }
				break;
			}
              
			cur = cur->next;
		}
  }

  #endif /* defined(VERIFY_CFTX) && defined(CALLBACK_LEA_DETECTION) */
  
  PRINT_DEBUG_FUNCTION_END("-> neutral, transl_length=%i", length);

  return action_copy(ts);
}

enum translation_state action_mov_imm_to_r(struct translate *ts) {
  unsigned char *addr = ts->cur_instr;

  PRINT_DEBUG_FUNCTION_START("action_mov_imm_to_r(*addr=%p, *transl_addr=%p, "
                             "length=%i)", addr, transl_addr, length);
  
  #if defined(VERIFY_CFTX) && defined(CALLBACK_MOV_IMM_TO_REG_DETECTION)           
  /* we are looking for: "movl   $0x805d440,0x*(%esp)" instructions */
  /*
  80547c6:	c7 04 24 40 d4 05 08 	movl   $0x805d440,(%esp)
  8053a2d:	c7 44 24 5c d5 e4 05 	movl   $0x805e4d5,0x5c(%esp)
  */
  if(*ts->first_byte_after_opcode == 0x04 || *ts->first_byte_after_opcode == 0x44) {
	  
	    struct dso_chain *cur = ts->tld->dso_objects;
		unsigned char *disp_addr = ts->first_byte_after_opcode;
		disp_addr++;
		disp_addr++;
		
		/* check if it points to any executable memory within the same DSO */
		while (cur != 0) {
			
			/* is the pointer in the current dso? */
            if (PTR_IN_REGION(addr, cur->baseaddr, cur->endaddr-cur->baseaddr)) {
				
				  /* There is another 8bit displacement so move to the next byte where the actual value starts. */
				  if(*ts->first_byte_after_opcode == 0x44) {
					  disp_addr++;
				  }
				  
				  long disp = *((long*)disp_addr);
				  unsigned char *possible_callback = (unsigned char*)(disp);
				  
				  /* check if the obtained address points to executable memory (potential callback target) */
				  if (PTR_IN_REGION(possible_callback, cur->text, cur->text_size)
						|| PTR_IN_REGION(possible_callback, cur->init, cur->init_size)
						|| PTR_IN_REGION(possible_callback, cur->fini, cur->fini_size)) {
												
						fbt_add_callback(cur, (void*)possible_callback);
					
				  }
			   break;
			}
              
			cur = cur->next;
		}
  }

  #endif /* defined(VERIFY_CFTX) && defined(CALLBACK_MOV_IMM_TO_REG_DETECTION) */
  
  PRINT_DEBUG_FUNCTION_END("-> neutral, transl_length=%i", length);

  return action_copy(ts);
}
