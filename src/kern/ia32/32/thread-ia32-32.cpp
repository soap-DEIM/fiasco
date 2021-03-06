IMPLEMENTATION[ia32 || ux]:

PUBLIC template<typename T> inline
void FIASCO_NORETURN
Thread::fast_return_to_user(Mword ip, Mword sp, T arg)
{
  assert_kdb(cpu_lock.test());
  assert_kdb(current() == this);
  assert_kdb(Config::Is_ux || (regs()->cs() & 3 == 3));

  regs()->ip(ip);
  regs()->sp(sp);
  regs()->flags(EFLAGS_IF);
  asm volatile
    ("mov %0, %%esp \t\n"
     "iret         \t\n"
     :
     : "r" (static_cast<Return_frame*>(regs())), "a" (arg)
    );
  __builtin_trap();
}

IMPLEMENT inline
Mword
Thread::user_sp() const
{ return regs()->sp(); }

IMPLEMENT inline
void
Thread::user_sp(Mword sp)
{ regs()->sp(sp); }

PROTECTED inline
int
Thread::do_trigger_exception(Entry_frame *r, void *ret_handler)
{
  if (!exception_triggered())
    {
      _exc_cont.activate(r, ret_handler);
      return 1;
    }
  // else ignore change of IP because triggered exception already pending
  return 0;
}


PUBLIC inline
void
Thread::restore_exc_state()
{
  assert (cpu_lock.test());
  _exc_cont.restore(regs());
#if 0

#ifdef CONFIG_PF_UX
  r->cs (exception_cs() & ~1);
#else
  r->cs (exception_cs());
#endif
  r->ip (_exc_ip);
  r->flags (_exc_flags);
  _exc_ip = ~0UL;
#endif
}

PRIVATE static inline
Return_frame *
Thread::trap_state_to_rf(Trap_state *ts)
{
  char *im = reinterpret_cast<char*>(ts + 1);
  return reinterpret_cast<Return_frame*>(im)-1;
}

PRIVATE static inline NEEDS[Thread::trap_state_to_rf]
bool FIASCO_WARN_RESULT
Thread::copy_utcb_to_ts(L4_msg_tag const &tag, Thread *snd, Thread *rcv,
                        unsigned char rights)
{
  Trap_state *ts = (Trap_state*)rcv->_utcb_handler;
  Mword       s  = tag.words();
  Unsigned32  cs = ts->cs();
  Utcb *snd_utcb = snd->utcb().access();

  // XXX: check that gs and fs point to valid user_entry only, for gdt and
  // ldt!
  if (EXPECT_FALSE(rcv->exception_triggered()))
    {
      // triggered exception pending
      Mem::memcpy_mwords(&ts->_gs, snd_utcb->values, s > 12 ? 12 : s);
      if (EXPECT_TRUE(s > 15))
	{
	  Continuation::User_return_frame const *s
	    = reinterpret_cast<Continuation::User_return_frame const *>((char*)&snd_utcb->values[12]);

	  rcv->_exc_cont.set(trap_state_to_rf(ts), s);
	}
    }
  else
    Mem::memcpy_mwords (&ts->_gs, snd_utcb->values, s > 16 ? 16 : s);

  // reset segments
  rcv->_gs = rcv->_fs = 0;

  if (tag.transfer_fpu() && (rights & L4_fpage::W))
    snd->transfer_fpu(rcv);

  // sanitize eflags
  ts->flags((ts->flags() & ~(EFLAGS_IOPL | EFLAGS_NT)) | EFLAGS_IF);

  // don't allow to overwrite the code selector!
  ts->cs(cs);

  bool ret = transfer_msg_items(tag, snd, snd_utcb,
                                rcv, rcv->utcb().access(), rights);

  rcv->state_del(Thread_in_exception);
  return ret;
}

PRIVATE static inline
bool FIASCO_WARN_RESULT
Thread::copy_ts_to_utcb(L4_msg_tag const &, Thread *snd, Thread *rcv,
                        unsigned char rights)
{
  Utcb *rcv_utcb = rcv->utcb().access();
  Trap_state *ts = (Trap_state*)snd->_utcb_handler;
  Mword        r = Utcb::Max_words;

  {
    Lock_guard <Cpu_lock> guard (&cpu_lock);
    if (EXPECT_FALSE(snd->exception_triggered()))
      {
	Mem::memcpy_mwords (rcv_utcb->values, &ts->_gs, r > 12 ? 12 : r);
	Continuation::User_return_frame *d
	    = reinterpret_cast<Continuation::User_return_frame *>((char*)&rcv_utcb->values[12]);

	snd->_exc_cont.get(d, trap_state_to_rf(ts));
      }
    else
      Mem::memcpy_mwords (rcv_utcb->values, &ts->_gs, r > 16 ? 16 : r);

    if (rcv_utcb->inherit_fpu() && (rights & L4_fpage::W))
	snd->transfer_fpu(rcv);
  }
  return true;
}


//----------------------------------------------------------------------------
IMPLEMENTATION [ia32 && !ux]:

IMPLEMENT inline NEEDS[Thread::exception_triggered]
void
Thread::user_ip(Mword ip)
{
  if (exception_triggered())
    _exc_cont.ip(ip);
  else
    regs()->ip(ip);
}


PRIVATE inline
int
Thread::check_trap13_kernel(Trap_state *ts)
{
  if (EXPECT_FALSE(ts->_trapno == 13 && (ts->_err & 3) == 0))
    {
      // First check if user loaded a segment register with 0 because the
      // resulting exception #13 can be raised from user _and_ kernel. If
      // the user tried to load another segment selector, the thread gets
      // killed.
      // XXX Should we emulate this too? Michael Hohmuth: Yes, we should.
      if (EXPECT_FALSE(!(ts->_ds & 0xffff)))
	{
	  Cpu::set_ds(Gdt::data_segment());
	  return 0;
	}
      if (EXPECT_FALSE(!(ts->_es & 0xffff)))
	{
	  Cpu::set_es(Gdt::data_segment());
	  return 0;
	}
      if (EXPECT_FALSE(ts->_ds & 0xfff8) == Gdt::gdt_code_user)
	{
	  WARN("%p eip=%08lx: code selector ds=%04lx",
               this, ts->ip(), ts->_ds & 0xffff);
	  Cpu::set_ds(Gdt::data_segment());
	  return 0;
	}
      if (EXPECT_FALSE(ts->_es & 0xfff8) == Gdt::gdt_code_user)
	{
	  WARN("%p eip=%08lx: code selector es=%04lx",
               this, ts->ip(), ts->_es & 0xffff);
	  Cpu::set_es(Gdt::data_segment());
	  return 0;
	}
      if (EXPECT_FALSE(ts->_fs & 0xfff8) == Gdt::gdt_code_user)
	{
	  WARN("%p eip=%08lx: code selector fs=%04lx",
               this, ts->ip(), ts->_fs & 0xffff);
	  ts->_fs = 0;
	  return 0;
	}
      if (EXPECT_FALSE(ts->_gs & 0xfff8) == Gdt::gdt_code_user)
	{
	  WARN("%p eip=%08lx: code selector gs=%04lx",
               this, ts->ip(), ts->_gs & 0xffff);
	  ts->_gs = 0;
	  return 0;
	}
    }

  return 1;
}


IMPLEMENT
void
Thread::user_invoke()
{
  user_invoke_generic();

  asm volatile
    ("  movl %%eax,%%esp \n"    // set stack pointer to regs structure
     "  movl %%ecx,%%es  \n"
     "  movl %%ecx,%%ds  \n"
     "  xorl %%eax,%%eax \n"    // clean out user regs
     "  xorl %%ecx,%%ecx \n"
     "  xorl %%edx,%%edx \n"
     "  xorl %%esi,%%esi \n"
     "  xorl %%edi,%%edi \n"
     "  xorl %%ebx,%%ebx \n"
     "  xorl %%ebp,%%ebp \n"
     "  iret             \n"
     :                          // no output
     : "a" (nonull_static_cast<Return_frame*>(current()->regs())),
       "c" (Gdt::gdt_data_user | Gdt::Selector_user)
     );

  // never returns here
}

//---------------------------------------------------------------------------
IMPLEMENTATION [ia32]:

#include <feature.h>
KIP_KERNEL_FEATURE("segments");

PROTECTED inline
L4_msg_tag
Thread::invoke_arch(L4_msg_tag tag, Utcb *utcb)
{
  switch (utcb->values[0] & Opcode_mask)
    {
      case Op_gdt_x86:

      // if no words given then return the first gdt entry
      if (EXPECT_FALSE(tag.words() == 1))
        {
          utcb->values[0] = Gdt::gdt_user_entry1 >> 3;
          return Kobject_iface::commit_result(0, 1);
        }

        {
          unsigned entry_number = utcb->values[1];
          unsigned idx = 2;

          for (; entry_number < Gdt_user_entries
                 && idx < tag.words()
               ; idx += 2, ++entry_number)
            {
              Gdt_entry *d = (Gdt_entry *)&utcb->values[idx];
              if (!d->unsafe())
                _gdt_user_entries[entry_number] = *d;
            }

          if (this == current_thread())
            switch_gdt_user_entries(this);

          return Kobject_iface::commit_result((utcb->values[1] << 3) + Gdt::gdt_user_entry1 + 3);
        }

    default:
      return commit_result(-L4_err::ENosys);
    };
}

//---------------------------------------------------------------------------
IMPLEMENTATION [ia32 & (debug | kdb)]:

#include "ipi.h"
#include "kernel_task.h"

/** Call the nested trap handler (either Jdb::enter_kdebugger() or the
 * gdb stub. Setup our own stack frame */
PRIVATE static
int
Thread::call_nested_trap_handler(Trap_state *ts)
{
  unsigned long phys_cpu = Cpu::phys_id_direct();
  unsigned log_cpu = Cpu::p2l(phys_cpu);
  if (log_cpu == ~0U)
    {
      printf("Trap on unknown CPU phys_id=%lx\n", phys_cpu);
      log_cpu = 0;
    }

  unsigned long &ntr = nested_trap_recover.cpu(log_cpu);

#if 0
  printf("%s: lcpu%u sp=%p t=%lu nested_trap_recover=%ld\n",
         __func__, log_cpu, (void*)Proc::stack_pointer(), ts->_trapno, ntr);
#endif

  int ret;

  unsigned dummy1, dummy2, dummy3;

  struct
  {
    Mword pdir;
    FIASCO_FASTCALL int (*handler)(Trap_state*, unsigned);
    void *stack;
  } p;

  if (!ntr)
    p.stack = dbg_stack.cpu(log_cpu).stack_top;
  else
    p.stack = 0;

  p.pdir = Kernel_task::kernel_task()->virt_to_phys((Address)Kmem::dir());
  p.handler = nested_trap_handler;


  // don't set %esp if gdb fault recovery to ensure that exceptions inside
  // kdb/jdb don't overwrite the stack
  asm volatile
    ("mov    %%esp,%[d2]	\n\t"
     "cmpl   $0,(%[ntr])	\n\t"
     "jne    1f			\n\t"
     "mov    8(%[p]),%%esp	\n\t"
     "1:			\n\t"
     "incl   (%[ntr])		\n\t"
     "mov    %%cr3, %[d1]	\n\t"
     "push   %[d2]		\n\t"
     "push   %[p]		\n\t"
     "push   %[d1]		\n\t"
     "mov    (%[p]), %[d1]	\n\t"
     "mov    %[d1], %%cr3	\n\t"
     "call   *4(%[p])		\n\t"
     "pop    %[d1]		\n\t"
     "mov    %[d1], %%cr3	\n\t"
     "pop    %[p]		\n\t"
     "pop    %%esp		\n\t"
     "cmpl   $0,(%[ntr])	\n\t"
     "je     1f			\n\t"
     "decl   (%[ntr])		\n\t"
     "1:			\n\t"

     : [ret] "=a"  (ret),
       [d1]  "=&c" (dummy1),
       [d2]  "=&r" (dummy2),
             "=d"  (dummy3)
     : [ts]      "a" (ts),
       [cpu]     "d" (log_cpu),
       [p]       "r" (&p),
       [ntr]     "r" (&ntr)
     : "memory");

  return ret == 0 ? 0 : -1;
}
