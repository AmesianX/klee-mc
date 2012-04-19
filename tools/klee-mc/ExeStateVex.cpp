#include "ExeStateVex.h"
#include "klee/breadcrumb.h"
#include "guest.h"
#include "guestcpustate.h"

using namespace klee;

Guest* ExeStateVex::base_guest = NULL;

void ExeStateVex::recordRegisters(const void* reg, int sz)
{
/* XXX */
}

ExeStateVex::ExeStateVex(const ExeStateVex& src)
: ExecutionState(src)
, syscall_c(0)
{
	const ExeStateVex	*esv;
	
	esv = dynamic_cast<const ExeStateVex*>(&src);
	assert (esv != NULL && "Copying non-ESV?");

	bc_log = esv->bc_log;
	reg_mo = esv->reg_mo;
}

void ExeStateVex::recordBreadcrumb(const struct breadcrumb* bc)
{
	bc_log.push_back(std::vector<unsigned char>(
		(const unsigned char*)bc,
		((const unsigned char*)bc) + bc->bc_sz));
}

ObjectState* ExeStateVex::getRegObj()
{ return addressSpace.findWriteableObject(reg_mo); }

const ObjectState* ExeStateVex::getRegObjRO() const
{ return addressSpace.findObject(reg_mo); }

void ExeStateVex::getGDBRegs(
	std::vector<uint8_t>& v,
	std::vector<bool>& is_conc) const
{
	const ObjectState	*reg_os;
	int			cpu_off, gdb_off;
	GuestCPUState		*cpu;

	reg_os = getRegObjRO();
	gdb_off = 0;
	cpu = base_guest->getCPUState();
	while ((cpu_off = cpu->cpu2gdb(gdb_off)) != -1) {
		gdb_off++;
		if (cpu_off < 0 || reg_os->isByteConcrete(cpu_off) == false) {
			v.push_back(0);
			is_conc.push_back(false);
			continue;
		}

		v.push_back(reg_os->read8c(cpu_off));
		is_conc.push_back(true);
	}
}
