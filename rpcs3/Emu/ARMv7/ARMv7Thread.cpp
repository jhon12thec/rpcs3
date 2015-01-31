#include "stdafx.h"
#include "rpcs3/Ini.h"
#include "Utilities/Log.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/CPU/CPUThreadManager.h"

#include "ARMv7Thread.h"
#include "ARMv7Decoder.h"
#include "ARMv7DisAsm.h"
#include "ARMv7Interpreter.h"

void ARMv7Context::write_pc(u32 value)
{
	thread.SetBranch(value);
}

u32 ARMv7Context::read_pc()
{
	return thread.PC;
}

u32 ARMv7Context::get_stack_arg(u32 pos)
{
	return vm::psv::read32(SP + sizeof(u32) * (pos - 5));
}

void ARMv7Context::fast_call(u32 addr)
{
	return thread.FastCall(addr);
}

#define TLS_MAX 128

u32 g_armv7_tls_start;

std::array<std::atomic<u32>, TLS_MAX> g_armv7_tls_owners;

void armv7_init_tls()
{
	g_armv7_tls_start = Emu.GetTLSMemsz() ? vm::cast(Memory.PSV.RAM.AllocAlign(Emu.GetTLSMemsz() * TLS_MAX, 4096)) : 0;

	for (auto& v : g_armv7_tls_owners)
	{
		v.store(0, std::memory_order_relaxed);
	}
}

u32 armv7_get_tls(u32 thread)
{
	if (!Emu.GetTLSMemsz())
	{
		return 0;
	}

	for (u32 i = 0; i < TLS_MAX; i++)
	{
		if (g_armv7_tls_owners[i] == thread)
		{
			return g_armv7_tls_start + i * Emu.GetTLSMemsz(); // if already initialized, return TLS address
		}
	}

	for (u32 i = 0; i < TLS_MAX; i++)
	{
		u32 old = 0;
		if (g_armv7_tls_owners[i].compare_exchange_strong(old, thread))
		{
			const u32 addr = g_armv7_tls_start + i * Emu.GetTLSMemsz(); // get TLS address
			memset(vm::get_ptr(addr), 0, Emu.GetTLSMemsz()); // fill TLS area with zeros
			memcpy(vm::get_ptr(addr), vm::get_ptr(Emu.GetTLSAddr()), Emu.GetTLSFilesz()); // initialize from TLS image
			return addr;
		}
	}

	throw "Out of TLS memory";
}

void armv7_free_tls(u32 thread)
{
	if (!Emu.GetTLSMemsz())
	{
		return;
	}

	for (auto& v : g_armv7_tls_owners)
	{
		u32 old = thread;
		if (v.compare_exchange_strong(old, 0))
		{
			return;
		}
	}
}

ARMv7Thread::ARMv7Thread()
	: CPUThread(CPU_THREAD_ARMv7)
	, context(*this)
	//, m_arg(0)
	//, m_last_instr_size(0)
	//, m_last_instr_name("UNK")
{
}

ARMv7Thread::~ARMv7Thread()
{
	armv7_free_tls(GetId());
}

void ARMv7Thread::InitRegs()
{
	memset(context.GPR, 0, sizeof(context.GPR[0]) * 15);
	context.APSR.APSR = 0;
	context.IPSR.IPSR = 0;
	context.ISET = Thumb;
	context.ITSTATE.IT = 0;
	context.SP = m_stack_addr + m_stack_size;
	context.TLS = armv7_get_tls(GetId());
}

void ARMv7Thread::InitStack()
{
	if(!m_stack_addr)
	{
		m_stack_size = 0x10000;
		m_stack_addr = (u32)Memory.Alloc(0x10000, 1);
	}
}

std::string ARMv7Thread::RegsToString()
{
	std::string result = "Registers:\n=========\n";
	for(int i=0; i<15; ++i)
	{
		result += fmt::Format("%s\t= 0x%08x\n", g_arm_reg_name[i], context.GPR[i]);
	}

	result += fmt::Format("APSR\t= 0x%08x [N: %d, Z: %d, C: %d, V: %d, Q: %d]\n", 
		context.APSR.APSR,
		fmt::by_value(context.APSR.N),
		fmt::by_value(context.APSR.Z),
		fmt::by_value(context.APSR.C),
		fmt::by_value(context.APSR.V),
		fmt::by_value(context.APSR.Q));
	
	return result;
}

std::string ARMv7Thread::ReadRegString(const std::string& reg)
{
	return "";
}

bool ARMv7Thread::WriteRegString(const std::string& reg, std::string value)
{
	return true;
}

void ARMv7Thread::DoReset()
{
}

void ARMv7Thread::DoRun()
{
	switch(Ini.CPUDecoderMode.GetValue())
	{
	case 0:
		//m_dec = new ARMv7Decoder(*new ARMv7DisAsm());
	break;

	case 1:
	case 2:
		m_dec = new ARMv7Decoder(context);
	break;
	}
}

void ARMv7Thread::DoPause()
{
}

void ARMv7Thread::DoResume()
{
}

void ARMv7Thread::DoStop()
{
}

void ARMv7Thread::DoCode()
{
}

void ARMv7Thread::FastCall(u32 addr)
{
	auto old_status = m_status;
	auto old_PC = PC;
	auto old_stack = context.SP;
	auto old_LR = context.LR;
	auto old_thread = GetCurrentNamedThread();

	m_status = Running;
	PC = addr;
	context.LR = Emu.GetCPUThreadStop();
	SetCurrentNamedThread(this);

	CPUThread::Task();

	m_status = old_status;
	PC = old_PC;
	context.SP = old_stack;
	context.LR = old_LR;
	SetCurrentNamedThread(old_thread);
}

void ARMv7Thread::FastStop()
{
	m_status = Stopped;
}

arm7_thread::arm7_thread(u32 entry, const std::string& name, u32 stack_size, u32 prio)
{
	thread = &Emu.GetCPU().AddThread(CPU_THREAD_ARMv7);

	thread->SetName(name);
	thread->SetEntry(entry);
	thread->SetStackSize(stack_size ? stack_size : Emu.GetInfo().GetProcParam().primary_stacksize);
	thread->SetPrio(prio ? prio : Emu.GetInfo().GetProcParam().primary_prio);

	argc = 0;
}
