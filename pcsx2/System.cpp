/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wx/wx.h"
#include "PrecompiledHeader.h"
#include "Common.h"
#include "IopCommon.h"
#include "VUmicro.h"
#include "newVif.h"
#include "MTVU.h"

#include "Elfheader.h"

#include "System/RecTypes.h"

#include "Utilities/MemsetFast.inl"

// --------------------------------------------------------------------------------------
//  RecompiledCodeReserve  (implementations)
// --------------------------------------------------------------------------------------

// Constructor!
// Parameters:
//   name - a nice long name that accurately describes the contents of this reserve.
RecompiledCodeReserve::RecompiledCodeReserve( const wxString& name, uint defCommit )
	: VirtualMemoryReserve( name, defCommit )
{
	m_prot_mode		= PageAccess_Any();
}

RecompiledCodeReserve::~RecompiledCodeReserve()
{
}

void* RecompiledCodeReserve::Assign( VirtualMemoryManagerPtr allocator, void *baseptr, size_t size )
{
	if (!_parent::Assign(std::move(allocator), baseptr, size)) return NULL;

	Commit();

	return m_baseptr;
}

void RecompiledCodeReserve::Reset()
{
	_parent::Reset();

	Commit();
}

bool RecompiledCodeReserve::Commit()
{ 
   return _parent::Commit();
}

// This error message is shared by R5900, R3000, and microVU recompilers.
void RecompiledCodeReserve::ThrowIfNotOk() const
{
	if (IsOk()) return;

	throw Exception::OutOfMemory(m_name)
		.SetDiagMsg(pxsFmt( L"Recompiled code cache could not be mapped." ))
		.SetUserMsg( L"This recompiler was unable to reserve contiguous memory required for internal caches.  This error can be caused by low virtual memory resources, such as a small or disabled swapfile, or by another program that is hogging a lot of memory."
		);
}

void SysOutOfMemory_EmergencyResponse(uptr blocksize)
{
	// An out of memory error occurred.  All we can try to do in response is reset the various
	// recompiler caches (which can sometimes total over 120megs, so it can be quite helpful).
	// If the user is using interpreters, or if the memory allocation failure was on a very small
	// allocation, then this code could fail; but that's fine.  We're already trying harder than
	// 99.995% of all programs ever written. -- air

	if (Cpu)
	{
		Cpu->SetCacheReserve( (Cpu->GetCacheReserve() * 2) / 3 );
		Cpu->Reset();
	}

	if (CpuVU0)
	{
		CpuVU0->SetCacheReserve( (CpuVU0->GetCacheReserve() * 2) / 3 );
		CpuVU0->Reset();
	}

	if (CpuVU1)
	{
		CpuVU1->SetCacheReserve( (CpuVU1->GetCacheReserve() * 2) / 3 );
		CpuVU1->Reset();
	}

	if (psxCpu)
	{
		psxCpu->SetCacheReserve( (psxCpu->GetCacheReserve() * 2) / 3 );
		psxCpu->Reset();
	}
}


#include "svnrev.h"

Pcsx2Config EmuConfig;

template< typename CpuType >
class CpuInitializer
{
public:
	std::unique_ptr<CpuType> MyCpu;
	ScopedExcept ExThrown;

	CpuInitializer();
	virtual ~CpuInitializer();

	bool IsAvailable() const
	{
		return !!MyCpu;
	}

	CpuType* GetPtr() { return MyCpu.get(); }
	const CpuType* GetPtr() const { return MyCpu.get(); }

	operator CpuType*() { return GetPtr(); }
	operator const CpuType*() const { return GetPtr(); }
};

// --------------------------------------------------------------------------------------
//  CpuInitializer Template
// --------------------------------------------------------------------------------------
// Helper for initializing various PCSX2 CPU providers, and handing errors and cleanup.
//
template< typename CpuType >
CpuInitializer< CpuType >::CpuInitializer()
{
	try {
		MyCpu = std::make_unique<CpuType>();
		MyCpu->Reserve();
	}
	catch( Exception::RuntimeError& ex )
	{
		log_cb(RETRO_LOG_ERROR, "CPU provider error:\n\t%s\n", ex.FormatDiagnosticMessage().c_str() );
		MyCpu = nullptr;
		ExThrown = ScopedExcept(ex.Clone());
	}
	catch( std::runtime_error& ex )
	{
		log_cb(RETRO_LOG_ERROR, "CPU provider error (STL Exception)\n\tDetails:%s\n", fromUTF8( ex.what() ).c_str() );
		MyCpu = nullptr;
		ExThrown = ScopedExcept(new Exception::RuntimeError(ex));
	}
}

template< typename CpuType >
CpuInitializer< CpuType >::~CpuInitializer()
{
	try {
		if (MyCpu)
			MyCpu->Shutdown();
	}
	DESTRUCTOR_CATCHALL
}

// --------------------------------------------------------------------------------------
//  CpuInitializerSet
// --------------------------------------------------------------------------------------
class CpuInitializerSet
{
public:
	CpuInitializer<recMicroVU0>		microVU0;
	CpuInitializer<recMicroVU1>		microVU1;

	CpuInitializer<InterpVU0>		interpVU0;
	CpuInitializer<InterpVU1>		interpVU1;

public:
	CpuInitializerSet() {}
	virtual ~CpuInitializerSet() = default;
};


namespace HostMemoryMap {
	// For debuggers
	uptr EEmem, IOPmem, VUmem, EErec, IOPrec, VIF0rec, VIF1rec, mVU0rec, mVU1rec, bumpAllocator;
}

/// Attempts to find a spot near static variables for the main memory
static VirtualMemoryManagerPtr makeMainMemoryManager() {
	// Everything looks nicer when the start of all the sections is a nice round looking number.
	// Also reduces the variation in the address due to small changes in code.
	// Breaks ASLR but so does anything else that tries to make addresses constant for our debugging pleasure
	uptr codeBase = (uptr)(void*)makeMainMemoryManager / (1 << 28) * (1 << 28);

	// The allocation is ~640mb in size, slighly under 3*2^28.
	// We'll hope that the code generated for the PCSX2 executable stays under 512mb (which is likely)
	// On x86-64, code can reach 8*2^28 from its address [-6*2^28, 4*2^28] is the region that allows for code in the 640mb allocation to reach 512mb of code that either starts at codeBase or 256mb before it.
	// We start high and count down because on macOS code starts at the beginning of useable address space, so starting as far ahead as possible reduces address variations due to code size.  Not sure about other platforms.  Obviously this only actually affects what shows up in a debugger and won't affect performance or correctness of anything.
	for (int offset = 4; offset >= -6; offset--) {
		uptr base = codeBase + (offset << 28);
		if ((sptr)base < 0 || (sptr)(base + HostMemoryMap::Size - 1) < 0) {
			// VTLB will throw a fit if we try to put EE main memory here
			continue;
		}
		auto mgr = std::make_shared<VirtualMemoryManager>("Main Memory Manager", base, HostMemoryMap::Size, /*upper_bounds=*/0, /*strict=*/true);
		if (mgr->IsOk()) {
			return mgr;
		}
	}

	return std::make_shared<VirtualMemoryManager>("Main Memory Manager", 0, HostMemoryMap::Size);
}

// --------------------------------------------------------------------------------------
//  SysReserveVM  (implementations)
// --------------------------------------------------------------------------------------
SysMainMemory::SysMainMemory()
	: m_mainMemory(makeMainMemoryManager())
	, m_bumpAllocator(m_mainMemory, HostMemoryMap::bumpAllocatorOffset, HostMemoryMap::Size - HostMemoryMap::bumpAllocatorOffset)
{
	uptr base = (uptr)MainMemory()->GetBase();
	HostMemoryMap::EEmem   = base + HostMemoryMap::EEmemOffset;
	HostMemoryMap::IOPmem  = base + HostMemoryMap::IOPmemOffset;
	HostMemoryMap::VUmem   = base + HostMemoryMap::VUmemOffset;
	HostMemoryMap::EErec   = base + HostMemoryMap::EErecOffset;
	HostMemoryMap::IOPrec  = base + HostMemoryMap::IOPrecOffset;
	HostMemoryMap::VIF0rec = base + HostMemoryMap::VIF0recOffset;
	HostMemoryMap::VIF1rec = base + HostMemoryMap::VIF1recOffset;
	HostMemoryMap::mVU0rec = base + HostMemoryMap::mVU0recOffset;
	HostMemoryMap::mVU1rec = base + HostMemoryMap::mVU1recOffset;
	HostMemoryMap::bumpAllocator = base + HostMemoryMap::bumpAllocatorOffset;
}

SysMainMemory::~SysMainMemory()
{
	try {
		ReleaseAll();
	}
	DESTRUCTOR_CATCHALL
}

void SysMainMemory::ReserveAll()
{
	pxInstallSignalHandler();

#ifndef NDEBUG
	log_cb(RETRO_LOG_DEBUG, "Mapping host memory for virtual systems...\n" );
#endif
	m_ee.Reserve(MainMemory());
	m_iop.Reserve(MainMemory());
	m_vu.Reserve(MainMemory());
}

void SysMainMemory::CommitAll()
{
	vtlb_Core_Alloc();
	if (m_ee.IsCommitted() && m_iop.IsCommitted() && m_vu.IsCommitted()) return;

#ifndef NDEBUG
	log_cb(RETRO_LOG_DEBUG, "Allocating host memory for virtual systems...\n" );
#endif
	m_ee.Commit();
	m_iop.Commit();
	m_vu.Commit();
}


void SysMainMemory::ResetAll()
{
	CommitAll();

#ifndef NDEBUG
	log_cb(RETRO_LOG_DEBUG, "Resetting host memory for virtual systems...\n" );
#endif
	m_ee.Reset();
	m_iop.Reset();
	m_vu.Reset();

	// Note: newVif is reset as part of other VIF structures.
}

void SysMainMemory::DecommitAll()
{
	if (!m_ee.IsCommitted() && !m_iop.IsCommitted() && !m_vu.IsCommitted()) return;

	log_cb(RETRO_LOG_INFO, "Decommitting host memory for virtual systems...\n" );

	// On linux, the MTVU isn't empty and the thread still uses the m_ee/m_vu memory
	vu1Thread.WaitVU();
	// The EE thread must be stopped here command mustn't be send
	// to the ring. Let's call it an extra safety valve :)
	vu1Thread.Reset();

	m_ee.Decommit();
	m_iop.Decommit();
	m_vu.Decommit();

	closeNewVif(0);
	closeNewVif(1);

	vtlb_Core_Free();
}

void SysMainMemory::ReleaseAll()
{
	DecommitAll();

	log_cb(RETRO_LOG_INFO, "Releasing host memory maps for virtual systems...\n" );

	vtlb_Core_Free();		// Just to be sure... (calling order could result in it getting missed during Decommit).

	releaseNewVif(0);
	releaseNewVif(1);

	m_ee.Decommit();
	m_iop.Decommit();
	m_vu.Decommit();

	safe_delete(Source_PageFault);
}


// --------------------------------------------------------------------------------------
//  SysCpuProviderPack  (implementations)
// --------------------------------------------------------------------------------------
SysCpuProviderPack::SysCpuProviderPack()
{
	log_cb(RETRO_LOG_INFO, "Reserving memory for recompilers...\n" );

	CpuProviders = std::make_unique<CpuInitializerSet>();

	try {
		recCpu.Reserve();
	}
	catch( Exception::RuntimeError& ex )
	{
		m_RecExceptionEE = ScopedExcept(ex.Clone());
		log_cb(RETRO_LOG_ERROR, "EE Recompiler Reservation Failed:\n%s\n", ex.FormatDiagnosticMessage().c_str() );
		recCpu.Shutdown();
	}

	try {
		psxRec.Reserve();
	}
	catch( Exception::RuntimeError& ex )
	{
		m_RecExceptionIOP = ScopedExcept(ex.Clone());
		log_cb(RETRO_LOG_ERROR, "IOP Recompiler Reservation Failed:\n%s\n", ex.FormatDiagnosticMessage().c_str() );
		psxRec.Shutdown();
	}

	// hmm! : VU0 and VU1 pre-allocations should do sVU and mVU separately?  Sounds complicated. :(

	if (newVifDynaRec)
	{
		dVifReserve(0);
		dVifReserve(1);
	}
}

bool SysCpuProviderPack::IsRecAvailable_MicroVU0() const { return CpuProviders->microVU0.IsAvailable(); }
bool SysCpuProviderPack::IsRecAvailable_MicroVU1() const { return CpuProviders->microVU1.IsAvailable(); }
BaseException* SysCpuProviderPack::GetException_MicroVU0() const { return CpuProviders->microVU0.ExThrown.get(); }
BaseException* SysCpuProviderPack::GetException_MicroVU1() const { return CpuProviders->microVU1.ExThrown.get(); }

void SysCpuProviderPack::CleanupMess() noexcept
{
	try
	{
		psxRec.Shutdown();
		recCpu.Shutdown();

		if (newVifDynaRec)
		{
			dVifRelease(0);
			dVifRelease(1);
		}
	}
	DESTRUCTOR_CATCHALL
}

SysCpuProviderPack::~SysCpuProviderPack()
{
	CleanupMess();
}

bool SysCpuProviderPack::HadSomeFailures( const Pcsx2Config::RecompilerOptions& recOpts ) const
{
	return	(recOpts.EnableEE && !IsRecAvailable_EE()) ||
			(recOpts.EnableIOP && !IsRecAvailable_IOP()) ||
			(recOpts.EnableVU0 && !IsRecAvailable_MicroVU0()) ||
			(recOpts.EnableVU1 && !IsRecAvailable_MicroVU1())
			;

}

BaseVUmicroCPU* CpuVU0 = NULL;
BaseVUmicroCPU* CpuVU1 = NULL;

void SysCpuProviderPack::ApplyConfig() const
{
	Cpu		= CHECK_EEREC	? &recCpu : &intCpu;
	psxCpu	= CHECK_IOPREC	? &psxRec : &psxInt;

	CpuVU0 = CpuProviders->interpVU0;
	CpuVU1 = CpuProviders->interpVU1;

	if( EmuConfig.Cpu.Recompiler.EnableVU0 )
		CpuVU0 = (BaseVUmicroCPU*)CpuProviders->microVU0;

	if( EmuConfig.Cpu.Recompiler.EnableVU1 )
		CpuVU1 = (BaseVUmicroCPU*)CpuProviders->microVU1;
}

// Resets all PS2 cpu execution caches, which does not affect that actual PS2 state/condition.
// This can be called at any time outside the context of a Cpu->Execute() block without
// bad things happening (recompilers will slow down for a brief moment since rec code blocks
// are dumped).
// Use this method to reset the recs when important global pointers like the MTGS are re-assigned.
void SysClearExecutionCache()
{
	GetCpuProviders().ApplyConfig();

	Cpu->Reset();
	psxCpu->Reset();

	// mVU's VU0 needs to be properly initialized for macro mode even if it's not used for micro mode!
	if (CHECK_EEREC)
		((BaseVUmicroCPU*)GetCpuProviders().CpuProviders->microVU0)->Reset();

	CpuVU0->Reset();
	CpuVU1->Reset();

	if (newVifDynaRec)
	{
		dVifReset(0);
		dVifReset(1);
	}
}

// Maps a block of memory for use as a recompiled code buffer, and ensures that the
// allocation is below a certain memory address (specified in "bounds" parameter).
// The allocated block has code execution privileges.
// Returns NULL on allocation failure.
u8* SysMmapEx(uptr base, u32 size, uptr bounds, const char *caller)
{
	u8* Mem = (u8*)HostSys::Mmap( base, size );

	if( (Mem == NULL) || (bounds != 0 && (((uptr)Mem + size) > bounds)) )
	{
		if( base )
		{
			log_cb(RETRO_LOG_DEBUG, "First try failed allocating %s at address 0x%x\n", caller, base );

			// Let's try again at an OS-picked memory area, and then hope it meets needed
			// boundschecking criteria below.
			SafeSysMunmap( Mem, size );
			Mem = (u8*)HostSys::Mmap( 0, size );
		}

		if( (bounds != 0) && (((uptr)Mem + size) > bounds) )
		{
			log_cb(RETRO_LOG_WARN, "Second try failed allocating %s, block ptr 0x%x does not meet required criteria.\n", caller, Mem );
			SafeSysMunmap( Mem, size );

			// returns NULL, caller should throw an exception.
		}
	}
	return Mem;
}

wxString SysGetBiosDiscID()
{
	// FIXME: we should return a serial based on
	// the BIOS being run (either a checksum of the BIOS roms, and/or a string based on BIOS
	// region and revision).

	return wxEmptyString;
}

// This function always returns a valid DiscID -- using the Sony serial when possible, and
// falling back on the CRC checksum of the ELF binary if the PS2 software being run is
// homebrew or some other serial-less item.
wxString SysGetDiscID()
{
	if( !DiscSerial.IsEmpty() ) return DiscSerial;

	if( !ElfCRC )
	{
		// system is currently running the BIOS
		return SysGetBiosDiscID();
	}

	return pxsFmt( L"%08x", ElfCRC );
}
