#include <poll.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>

#include "klee/breadcrumb.h"
#include "klee/Internal/ADT/KTest.h"
#include "guestcpustate.h"
#include "guest.h"

#include "Crumbs.h"
#include "SyscallsKTest.h"

#define KREPLAY_NOTE	"[kmc-replay] "
#define KREPLAY_SC	"[kmc-sc] "

extern "C"
{
#include <valgrind/libvex_guest_amd64.h>
}

SyscallsKTest* SyscallsKTest::create(
	Guest* in_g,
	const char* fname_ktest,
	Crumbs* in_crumbs)
{
	SyscallsKTest	*skt;

	assert (in_g->getArch() == Arch::X86_64);

	skt = new SyscallsKTest(in_g, fname_ktest, in_crumbs);
	if (skt->ktest == NULL || skt->crumbs == NULL) {
		delete skt;
		skt = NULL;
	}

	return skt;
}

SyscallsKTest::SyscallsKTest(
	Guest* in_g,
	const char* fname_ktest,
	Crumbs* in_crumbs)
: Syscalls(in_g)
, ktest(NULL)
, sc_retired(0)
, crumbs(in_crumbs)
, bcs_crumb(NULL)
{
	ktest = kTest_fromFile(fname_ktest);
	if (ktest == NULL) return;
	next_ktest_obj = 0;
}

SyscallsKTest::~SyscallsKTest(void)
{
	if (ktest) kTest_free(ktest);
	if (bcs_crumb) delete bcs_crumb;
}

void SyscallsKTest::loadSyscallEntry(SyscallParams& sp)
{
	uint64_t sys_nr = sp.getSyscall();
	
	assert (bcs_crumb == NULL && "Last crumb should be freed before load");
	bcs_crumb = reinterpret_cast<struct bc_syscall*>(
		crumbs->next(BC_TYPE_SC));
	if (!bcs_crumb) {
		fprintf(stderr,
			KREPLAY_NOTE
			"Could not read sclog entry #%d. Out of entries.\n"
			KREPLAY_NOTE
			"sys_nr=%d. arg[0]=%p arg[1]=%p arg[2]=%p\n",
			sc_retired,
			sys_nr,
			sp.getArg(0),
			sp.getArg(1),
			sp.getArg(2));
		abort();
	}

	if (bcs_crumb->bcs_sysnr != sys_nr) {
		fprintf(stderr,
			KREPLAY_NOTE
			"Mismatched: Got sysnr=%d. Expected sysnr=%d\n",
			sys_nr, bcs_crumb->bcs_sysnr);
	}

	assert (bcs_crumb->bcs_sysnr == sys_nr && "sysnr mismatch with log");
	if (bc_sc_is_newregs(bcs_crumb)) {
		bool	reg_ok;

		reg_ok = copyInRegMemObj();
		if (!reg_ok) {
			fprintf(stderr,
				KREPLAY_NOTE
				"Could not copy in regmemobj\n");
			exited = true;
			fprintf(stderr,
				KREPLAY_NOTE
				"Wrong guest sshot for this ktest?\n");
			abort();
			return;
		}
	}

}

//fprintf(stderr, "Faking syscall "#x" (%p,%p,%p)\n",
//	sp.getArg(0), sp.getArg(1), sp.getArg(2));


#define FAKE_SC(x) 		\
	case SYS_##x:		\
	setRet(0);		\
	break;


uint64_t SyscallsKTest::apply(SyscallParams& sp)
{
	uint64_t	ret;
	uint64_t	sys_nr;

	ret = 0;
	sys_nr = sp.getSyscall();

	fprintf(stderr, KREPLAY_NOTE"Applying: sys=%d\n", sys_nr);
	loadSyscallEntry(sp);
	/* load in extras */


	/* extra thunks */
	switch(sys_nr) {
	case SYS_close: break;
	case SYS_read:
		if (getRet() != -1) {
			bool	ok;
			ok = copyInMemObj(sp.getArg(1), sp.getArg(2));
			assert (ok && "OOPS BAD READ");
		}
		break;
	case SYS_brk:
		setRet(-1);
		break;
	case SYS_open:
		fprintf(stderr, KREPLAY_SC "OPEN \"%s\" ret=%p\n",
			sp.getArg(0), getRet());
		break;
	case SYS_write:
		fprintf(stderr, KREPLAY_SC "WRITING %d bytes to fd=%d \"%s\"\n",
			sp.getArg(2), sp.getArg(0), sp.getArg(1));
		break;
	FAKE_SC(rt_sigaction)
	FAKE_SC(rt_sigprocmask)
	FAKE_SC(fadvise64)
	case SYS_tgkill: break;
	case SYS_lseek:
		assert ((int64_t)getRet() >= -1 &&
			(int64_t)getRet() <= 4096 && "mismatches model");
		break;

	case SYS_readlink: {
		bool ok;
		ok = copyInMemObj(sp.getArg(1), sp.getArg(2));
		assert (ok && "OOPS BAD READ");
	}

	case SYS_recvmsg: {
		struct msghdr	*mhdr;
		bool		ok;

		mhdr = (struct msghdr*)sp.getArg(1);
		ok = copyInMemObj(
			(intptr_t)(&mhdr->msg_iov[0].iov_base),
			mhdr->msg_iov[0].iov_len);
		assert (ok && "BAD RECVMSG");
		mhdr->msg_controllen = 0;
	}
	break;

	case SYS_poll: {
		struct pollfd	*fds;
		uint64_t	poll_addr;
		unsigned int	i, nfds;

		poll_addr = sp.getArg(0);
		fds = (struct pollfd*)poll_addr;
		nfds = sp.getArg(1);

		for (i = 0; i < nfds; i++) fds[i].revents = fds[i].events;
		break;
	}
	case SYS_sendto:
	case SYS_connect:
	case SYS_socket:
	case SYS_getgid:
	case SYS_getuid:
	case SYS_geteuid:
	case SYS_getpid:
	case SYS_gettid:
	case SYS_ioctl:
	case SYS_setgid:
	case SYS_setuid:
		break;
	case SYS_munmap:
		sc_munmap(sp);
		break;
	case SYS_mmap:
		sc_mmap(sp);
		break;
	case SYS_fstat:
	case SYS_stat:
		sc_stat(sp);
		break;
	case SYS_exit_group:
	case SYS_exit:
		exited = true;
		break;
	default:
		if (!bc_sc_is_thunk(bcs_crumb)) break;
		fprintf(stderr,
			KREPLAY_NOTE "No thunk for syscall %d\n",
			sys_nr);
		assert (0 == 1 && "TRICKY SYSCALL");
	}

	fprintf(stderr,
		KREPLAY_NOTE "Retired: sys=%d. ret=%p\n",
		sys_nr, getRet());
	
	sc_retired++;
	delete bcs_crumb;
	bcs_crumb = NULL;

	return ret;
}

void SyscallsKTest::sc_mmap(SyscallParams& sp)
{
	VexGuestAMD64State	*guest_cpu;
	void			*ret;
	bool			copied_in;

	if ((void*)bcs_crumb->bcs_ret != MAP_FAILED) {
		ret = mmap(
			(void*)bcs_crumb->bcs_ret,
			sp.getArg(1),
			PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0);
		assert (ret != MAP_FAILED);
	} else {
		ret = (void*)bcs_crumb->bcs_ret;
	}

	guest_cpu = (VexGuestAMD64State*)guest->getCPUState()->getStateData();
	guest_cpu->guest_RAX = (uint64_t)ret;
	copied_in = copyInMemObj(guest_cpu->guest_RAX, sp.getArg(1));
	assert (copied_in && "BAD MMAP MEMOBJ");
}

void SyscallsKTest::sc_munmap(SyscallParams& sp)
{
	munmap((void*)sp.getArg(0), sp.getArg(1));
}

/* caller should know the size of the object based on
 * the syscall's context */
char* SyscallsKTest::feedMemObj(unsigned int sz)
{
	char			*obj_buf;
	struct KTestObject	*cur_obj;
	
	if (next_ktest_obj >= ktest->numObjects) {
		/* request overflow */
		fprintf(stderr, KREPLAY_NOTE"OF\n");
		return NULL;
	}

	cur_obj = &ktest->objects[next_ktest_obj++];
	if (cur_obj->numBytes != sz) {
		/* out of sync-- how to handle? */
		fprintf(stderr, KREPLAY_NOTE"OOSYNC: Expected: %d. Got: %d\n",
			sz,
			cur_obj->numBytes);
		return NULL;
	}

	obj_buf = new char[sz];
	memcpy(obj_buf, cur_obj->bytes, sz);

	return obj_buf;
}

/* XXX the vexguest stuff needs to be pulled into guestcpustate */
void SyscallsKTest::sc_stat(SyscallParams& sp)
{
	if (getRet() != 0) return;

	if (!copyInMemObj(sp.getArg(1), sizeof(struct stat))) {
		fprintf(stderr, KREPLAY_NOTE"failed to copy in memobj\n");
		exited = true;
		fprintf(stderr,
			"Do you have the right guest sshot for this ktest?");
		abort();
		return;
	}
}

bool SyscallsKTest::copyInMemObj(uint64_t guest_addr, unsigned int sz)
{
	char	*buf;

	/* first, grab mem obj */
	if ((buf = feedMemObj(sz)) == NULL)
		return false;

	guest->getMem()->memcpy(guest_ptr(guest_addr), buf, sz);

	delete [] buf;
	return true;
}

void SyscallsKTest::setRet(uint64_t r)
{
	VexGuestAMD64State	*guest_cpu;
	guest_cpu = (VexGuestAMD64State*)guest->getCPUState()->getStateData();
	guest_cpu->guest_RAX = r;
}

uint64_t SyscallsKTest::getRet(void) const
{
	VexGuestAMD64State	*guest_cpu;
	guest_cpu = (VexGuestAMD64State*)guest->getCPUState()->getStateData();
	return guest_cpu->guest_RAX;
}

bool SyscallsKTest::copyInRegMemObj(void)
{
	char			*partial_reg_buf;
	VexGuestAMD64State	*partial_cpu, *guest_cpu;
	unsigned int		reg_sz;

	reg_sz = guest->getCPUState()->getStateSize();
	if ((partial_reg_buf = feedMemObj(reg_sz)) == NULL) {
		return false;
	}
	partial_cpu = (VexGuestAMD64State*)partial_reg_buf;

	/* load RAX, RCX, R11 */
	guest_cpu = (VexGuestAMD64State*)guest->getCPUState()->getStateData();
	guest_cpu->guest_RAX = partial_cpu->guest_RAX;
	guest_cpu->guest_RCX = partial_cpu->guest_RCX;
	guest_cpu->guest_R11 = partial_cpu->guest_R11;

	delete [] partial_reg_buf;
	return true;
}
