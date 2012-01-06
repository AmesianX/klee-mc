#include <sys/mman.h>

#include "symbols.h"
#include "guest.h"
#include "guestcpustate.h"

#include "klee/Internal/ADT/Crumbs.h"
#include "klee/Internal/ADT/KTestStream.h"
#include "static/Sugar.h"

#include "../klee-mc/ExeUC.h"

using namespace klee;

#define PAGE_SZ	0x1000UL

typedef std::map<std::string, std::vector<char> > ucbuf_map_ty;

template <typename UCTabEnt>
static void loadUCBuffers(Guest* gs, KTestStream* kts_uc)
{
	std::set<uint64_t>	uc_pages;
	ucbuf_map_ty		ucbufs;
	const UCTabEnt		*uctab;
	char			*lentab;
	const KTestObject	*kto;
	char			*cpu_state;
	unsigned		cpu_len;

	
	lentab = kts_uc->feedObjData(); // XXX need to be smarter
	uctab = (const UCTabEnt*)lentab;

	while ((kto = kts_uc->nextObject()) != NULL) {
		ucbufs[kto->name] = std::vector<char>(
			kto->bytes, kto->bytes+kto->numBytes);
	}

	foreach (it, ucbufs.begin(), ucbufs.end()) {
		std::string	uc_name(it->first);
		uint64_t	base_ptr;
		unsigned	idx;

		std::cerr << uc_name << '\n';

		std::cerr << "BUF SIZE: " << it->second.size() << '\n';

		// uc_buf_n => uc_buf_n + 7 = n
		idx = atoi(uc_name.c_str() + 7);
		std::cerr << "WAHAHA IDX=" << idx << '\n';

		assert (idx < (22060 / sizeof(*uctab)) &&
			"UCBUF OUT OF BOUNDS");

		std::cerr << "WUUUTTTT " << (void*)&uctab[idx] << '\n';
		assert (uctab[idx].len == it->second.size());

		base_ptr = NULL;
		if (uctab[idx].real_ptr) base_ptr = uctab[idx].real_ptr;
		if (uctab[idx].sym_ptr) base_ptr = uctab[idx].sym_ptr;
		assert (base_ptr &&
			"No base pointer for ucbuf given!");

		std::cerr << "REALPTR: " << uctab[idx].real_ptr << '\n';
		std::cerr << "SYMPTR: " <<  uctab[idx].sym_ptr << '\n';
		std::cerr << "BASE_PTR: " << (void*)base_ptr << '\n';

		uc_pages.insert(base_ptr & ~(PAGE_SZ-1));
		uc_pages.insert((base_ptr+uctab[idx].len-1) & ~(PAGE_SZ-1));
		std::cerr << "UC PAGES INSERTED\n";
	}

	std::cerr << "UC PAGES CYCLE\n";

	foreach (it, uc_pages.begin(), uc_pages.end()) {
		void	*new_page, *mapped_addr;
		
		new_page = (void*)*it;
		mapped_addr = mmap(
			new_page,
			PAGE_SZ,
			PROT_READ | PROT_WRITE, 
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0);
		if (mapped_addr != new_page) {
			std::cerr << "FAILED TO MAP " << new_page << '\n';
			assert (0 == 1 && "OOPS");
		}
		std::cerr << "MAPPED: " << mapped_addr << '\n';
	}


	cpu_state = (char*)gs->getCPUState()->getStateData();
	cpu_len = gs->getCPUState()->getStateSize();
	foreach (it, ucbufs.begin(), ucbufs.end()) {
		std::string	uc_name(it->first);
		uint64_t	base_ptr;
		unsigned	idx;

		idx = atoi(uc_name.c_str() + 7);
		base_ptr = NULL;
		if (uctab[idx].real_ptr) base_ptr = uctab[idx].real_ptr;
		if (uctab[idx].sym_ptr) base_ptr = uctab[idx].sym_ptr;

		if (gs->getMem()->is32Bit()) {
			if (idx < cpu_len / 4) {
				memcpy(cpu_state + idx*4, &base_ptr, 4);
			}
		} else if (idx < cpu_len / 8) {
			memcpy(cpu_state + idx*8, &base_ptr, 8);
		}

		/* buffers from klee are actually 8-aligned,
		 * so adjust copy-out to start at 8-byte boundary */
		base_ptr &= ~((uint64_t)0x7);
		for (unsigned i = 0; i < it->second.size(); i++)
			((char*)base_ptr)[i] = it->second[i];

	}

	std::cerr << "Copied in unconstrained buffers\n";
}

KTestStream* setupUCFunc(
	Guest		*gs,
	const char	*func,
	const char	*dirname,
	unsigned	test_num)
{
	KTestStream	*kts_uc, *kts_klee;
	const Symbol	*sym;
	char		fname_kts[256];
	char		*regfile_uc, *regfile_gs;

	/* patch up for UC */
	printf("Using func: %s\n", func);

	snprintf(
		fname_kts,
		256,
		"%s/test%06d.ucktest.gz", dirname, test_num);
	kts_uc = KTestStream::create(fname_kts);

	snprintf(
		fname_kts,
		256,
		"%s/test%06d.ktest.gz", dirname, test_num);
	kts_klee = KTestStream::create(fname_kts);

	assert (kts_uc && kts_klee);

	/* 1. setup environment */

	/* 1.a setup register values */
	Exempts	ex(ExeUC::getRegExempts(gs));
	regfile_uc = kts_uc->feedObjData(gs->getCPUState()->getStateSize());
	regfile_gs = (char*)gs->getCPUState()->getStateData();
	foreach (it, ex.begin(), ex.end()) {
		memcpy(regfile_uc+it->first, regfile_gs+it->first, it->second);
	}
	memcpy(regfile_gs, regfile_uc, gs->getCPUState()->getStateSize());
	delete regfile_uc;

	/* 1.b resteer execution to function */
	sym = gs->getSymbols()->findSym(func);
	if (sym == NULL) {
		std::cerr << "UC Function '" << func << "' not found. ULP\n";
		delete kts_uc;
		delete kts_klee;
		return NULL;
	}

	guest_ptr	func_ptr(guest_ptr(sym->getBaseAddr()));
	std::cerr << "HEY: " << (void*)func_ptr.o << '\n';
	gs->getCPUState()->setPC(func_ptr);

	std::cerr << "WOO: " << (void*)gs->getCPUState()->getPC().o << '\n';

	/* return to 'deadbeef' when done executing */
	if (gs->getArch() == Arch::ARM) {
		/* this is the wrong thing to do; I know */
		((uint32_t*)gs->getCPUState()->getStateData())[14] = 0xdeadbeef;
	} else {
		gs->getMem()->writeNative(
			gs->getCPUState()->getStackPtr(),
			0xdeadbeef);
	}

	/* 2. scan through ktest, allocate buffers */
	if (gs->getMem()->is32Bit())
		loadUCBuffers<UCTabEnt32>(gs, kts_uc);
	else
		loadUCBuffers<UCTabEnt64>(gs, kts_uc);

	/* done setting up buffers, drop UC ktest data */
	delete kts_uc;

	/* return stripped ktest file */
	return kts_klee;
}


