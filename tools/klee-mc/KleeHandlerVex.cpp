#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_os_ostream.h>
#include "static/Sugar.h"

#include "klee/Internal/Module/KFunction.h"
#include "klee/Internal/System/Time.h"
#include "KleeHandlerVex.h"
#include "ExecutorVex.h"
#include "ExeStateVex.h"

#include "guestcpustate.h"
#include "guestsnapshot.h"

#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <fstream>

using namespace llvm;
using namespace klee;

namespace
{
	llvm::cl::opt<bool> ValidateTestCase(
		"validate-test",
		llvm::cl::desc("Validate tests with concrete replay"));

	llvm::cl::opt<bool> DumpEarlyErr(
		"dump-early-err",
		llvm::cl::desc("Dump early error info"));


	llvm::cl::opt<bool> DumpTestRegs(
		"dump-test-regs",
		llvm::cl::desc(
			"Write out state's registers at time of exit."));
}

static double base_time;

KleeHandlerVex::KleeHandlerVex(const CmdArgs* cmdargs, Guest *_gs)
: KleeHandler(cmdargs), gs(_gs)
{
	base_time = util::getWallTime();

	if (ValidateTestCase) {
		/* can only validate tests when the guest is a snapshot,
		 * otherwise the values get jumbled up */
		assert (gs != NULL);
		assert (dynamic_cast<GuestSnapshot*>(gs) != NULL);
	}
}


unsigned KleeHandlerVex::processTestCase(
	const ExecutionState &state,
	const char *errorMessage,
	const char *errorSuffix)
{
	std::unique_ptr<std::ostream>	f;
	unsigned			id;

	id = KleeHandler::processTestCase(state, errorMessage, errorSuffix);
	if (!id) return 0;

	/* log error if one exists so replay doesn't have sclog issues */
	if (errorMessage != NULL && errorSuffix != NULL) {
		ExeStateVex	*esv;
		esv = const_cast<ExeStateVex*>(
			dynamic_cast<const ExeStateVex*>(&state));
		esv->logError(errorMessage, errorSuffix);
	}
	dumpLog(state, "crumbs", id);

	if (DumpTestRegs) {
		/* ESV::logXferReg is similar, I guess.. */
		const MemoryObject	*mo;
		const ObjectState	*os;
		unsigned		sz;
		std::vector<uint8_t>	buf;

		mo = es2esvc(state).getRegCtx();
		os = GETREGOBJRO(state);

		sz = mo->size;
		buf.resize(sz * 2);

		os->readConcrete(buf.data(), sz);
		for (unsigned i = 0; i < sz; i++)
			buf[i+sz] = (os->isByteConcrete(i)) ? 0xff : 0;

		f = openTestFile("regs", id);
		assert (f != NULL);
		f->write(((const char*)buf.data()), sz);
		f = openTestFile("regsmask", id);
		f->write((const char*)(buf.data()+sz), sz);
	}


	fprintf(stderr, "===DONE WRITING TESTID=%d (es=%p) [%f]===\n",
		id, &state, util::getWallTime() - base_time);
	if (!ValidateTestCase)
		return id;

	if (getStopAfterNTests() && id >= getStopAfterNTests())
		return id;

	f = openTestFile("validate", id);
	if (f == NULL)
		return id;

	if (validateTest(id))
		(*f) << "OK #" << id << '\n';
	else
		(*f) << "FAIL #" << id << '\n';

	return id;
}

void KleeHandlerVex::dumpLog(
	const ExecutionState& state, const char* name, unsigned id)
{
	const ExeStateVex		*esv;
	RecordLog::const_iterator	begin, end;

	esv = dynamic_cast<const ExeStateVex*>(&state);
	assert (esv != NULL);

	begin = esv->crumbBegin();
	end = esv->crumbEnd();
	if (begin == end) return;

	auto f = openTestFileGZ(name, id);
	if (f == NULL) return;

	foreach (it, begin, end) {
		const std::vector<unsigned char>	&r(*it);
		std::copy(
			r.begin(), r.end(),
			std::ostream_iterator<unsigned char>(*f));
	}
}


void KleeHandlerVex::printErrorMessage(
	const ExecutionState& state,
	const char* errorMessage,
	const char* errorSuffix,
	unsigned id)
{
	KleeHandler::printErrorMessage(state, errorMessage, errorSuffix, id);

	/* don't errdump for early */
	if (!DumpEarlyErr && strcmp(errorSuffix, "early") == 0) return;

	if (auto f = openTestFileGZ("errdump", id)) {
		printErrDump(state, *f);
	} else
		LOSING_IT("errdump");
}

void KleeHandlerVex::printErrDump(
	const ExecutionState& state,
	std::ostream& os) const
{
	const Function* top_f;
	os << "Stack:\n";
	m_exevex->printStackTrace(state, os);

	os << "\nRegisters:\n";
	gs->getCPUState()->print(os);

	top_f = state.stack.back().kf->function;
	os << "\nFunc: ";
	if (top_f != NULL) {
		raw_os_ostream ros(os);
		ros << top_f;
	} else
		os << "???";
	os << "\n";

	os << "\nObjects:\n";
	state.addressSpace.printObjects(os);

	os << "\n\nConstraints: \n";
	state.constraints.print(os);
}

bool KleeHandlerVex::validateTest(unsigned id)
{
	pid_t		child_pid, ret_pid;
	int		status;

	child_pid = fork();
	if (child_pid == -1) {
		std::cerr << "ERROR: could not validate test " << id << '\n';
		return false;
	}

	if (child_pid == 0) {
		const char	*argv[5];
		char		idstr[32];

		argv[0] = "kmc-replay";

		snprintf(idstr, 32, "%d", id);
		argv[1] = idstr;
		argv[2] = getOutputDir();
		argv[3] = static_cast<GuestSnapshot*>(gs)->getPath().c_str();
		argv[4] = NULL;

		/* don't write anything please! */
		close(0);
		close(1);
		close(2);
		open("/dev/null", O_RDWR);
		open("/dev/null", O_RDWR);
		open("/dev/null", O_RDWR);

		execvp("kmc-replay", (char**)argv); 
		_exit(-1);
	}

	ret_pid = waitpid(child_pid, &status, 0);
	if (ret_pid != child_pid) {
		std::cerr << "VALDIATE: BAD WAIT\n";
		return false;
	}

	/* found a crash, treat it as a valid exit */
	if (WIFSIGNALED(status)) {
		int	sig = WTERMSIG(status);
		if (sig == SIGSEGV)
			return true;
		if (sig == SIGFPE)
			return true;

		if (sig == SIGABRT) {
			/* This causes false negatives because sometimes
			 * there's an ASLR collision. Retry once to make
			 * sure everything is OK. */
			static int	 depth = 0;
			bool		ret;

			if (depth == 0) {
				depth++;
				std::cerr << "[Validate] Retrying on SIGABRT\n";
				ret = validateTest(id);
				depth--;
				return ret;
			}
		}
	}

	if (!WIFEXITED(status)) {
		std::cerr << "[Validate] DID NOT EXIT. Code=" << status << '\n';
		return false;
	}

	if (WEXITSTATUS(status) != 0) {
		std::cerr << "[Validate] Non-zero exit status.\n";
		return false;
	}

	return true;
}

void KleeHandlerVex::setInterpreter(Interpreter *i)
{
	m_exevex = dynamic_cast<ExecutorVex*>(i);
	assert (m_exevex != NULL && "Expected ExecutorVex interpreter");

	KleeHandler::setInterpreter(i);
}

