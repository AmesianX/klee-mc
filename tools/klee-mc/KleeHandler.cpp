#include "llvm/Support/Signals.h"
#include "llvm/Support/CommandLine.h"

#include "klee/Common.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/System/Time.h"
#include "klee/util/gzip.h"
#include "klee/Solver.h"
#include "../../lib/Solver/SMTPrinter.h"
#include "static/Sugar.h"
#include "cmdargs.h"

#include "ExecutorVex.h"
#include "ExeStateVex.h"
#include "KleeHandler.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <cerrno>
#include <iostream>
#include <fstream>

using namespace llvm;
using namespace klee;

extern bool WriteTraces;

namespace {
  cl::opt<bool> NoOutput(
    "no-output",
    cl::desc("Don't generate test files"));

  cl::opt<bool> WriteCov(
    "write-cov",
    cl::desc("Write coverage information for each test case"));

  cl::opt<bool> WriteTestInfo(
    "write-test-info",
    cl::desc("Write additional test case information"));

  cl::opt<bool> WritePCs(
    "write-pcs",
    cl::desc("Write .pc files for each test case"));

  cl::opt<bool> WriteSMT(
  	"write-smt",
	cl::desc("Write .smt files for each test case"),
	cl::init(false));

  cl::opt<bool> WritePaths(
    "write-paths",
    cl::desc("Write .path files for each test case"));
  cl::opt<bool> WriteSymPaths(
    "write-sym-paths",
    cl::desc("Write .sym.path files for each test case"));

  cl::opt<unsigned> StopAfterNTests(
    "stop-after-n-tests",
	  cl::desc("Stop execution after generating the given number of tests.  Extra tests corresponding to partially explored paths will also be dumped."),
	  cl::init(0));

  cl::opt<std::string> OutputDir(
    "output-dir",
    cl::desc("Directory to write results in (defaults to klee-out-N)"),
    cl::init(""));

  cl::opt<bool> ExitOnError(
    "exit-on-error",
    cl::desc("Exit if errors occur"));
}

KleeHandler::KleeHandler(const CmdArgs* in_args)
: m_symPathWriter(0)
, m_infoFile(0)
, m_testIndex(0)
, m_pathsExplored(0)
, cmdargs(in_args)
, m_interpreter(0)
{
	std::string theDir;

	theDir = setupOutputDir();
	std::cerr << "KLEE: output directory = \"" << theDir << "\"\n";

	sys::Path p(theDir);
	if (!sys::path::is_absolute(p.str())) {
		sys::Path cwd = sys::Path::GetCurrentDirectory();
		cwd.appendComponent(theDir);
		p = cwd;
	}

	strcpy(m_outputDirectory, p.c_str());

	if (mkdir(m_outputDirectory, 0775) < 0) {
		std::cerr <<
			"KLEE: ERROR: Unable to make output directory: \"" <<
			m_outputDirectory  <<
			"\", refusing to overwrite.\n";
		exit(1);
	}

	setupOutputFiles();
}

bool KleeHandler::scanForOutputDir(const std::string& p, std::string& theDir)
{
	llvm::sys::Path	directory(p);
	int		err;

	std::string dirname = "";
	directory.eraseComponent();
	if (directory.isEmpty()) directory.set(".");

	for (int i = 0; ; i++) {
		char buf[256], tmp[64];
		sprintf(tmp, "klee-out-%d", i);
		dirname = tmp;
		sprintf(buf, "%s/%s", directory.c_str(), tmp);
		theDir = buf;

		if (DIR *dir = opendir(theDir.c_str())) {
			closedir(dir);
		} else {
			break;
		}
	}

	llvm::sys::Path klee_last(directory);
	klee_last.appendComponent("klee-last");

	err = unlink(klee_last.c_str());
	if (err < 0 && (errno != ENOENT))
		return false;

	if (symlink(dirname.c_str(), klee_last.c_str()) < 0)
		return false;

	return true;
}

std::string KleeHandler::setupOutputDir(void)
{
	std::string	theDir;

	if (OutputDir != "") return OutputDir;
	if (scanForOutputDir(cmdargs->getBinaryPath(), theDir)) return theDir;
	if (scanForOutputDir("", theDir)) return theDir;

	assert (0 == 1 && "failed to grab output dir");
	return "";
}

void KleeHandler::setupOutputFiles(void)
{
	char fname[1024];
	snprintf(fname, sizeof(fname), "%s/warnings.txt", m_outputDirectory);
	klee_warning_file = fopen(fname, "w");
	assert(klee_warning_file);

	snprintf(fname, sizeof(fname), "%s/messages.txt", m_outputDirectory);
	klee_message_file = fopen(fname, "w");
	assert(klee_message_file);

	m_infoFile = openOutputFile("info");
}

KleeHandler::~KleeHandler()
{
	if (m_symPathWriter) delete m_symPathWriter;
	delete m_infoFile;

	fclose(klee_message_file);
	fclose(klee_warning_file);
}

void KleeHandler::setInterpreter(Interpreter *i)
{
	m_interpreter = dynamic_cast<ExecutorVex*>(i);
	assert (m_interpreter != NULL && "Expected ExecutorVex interpreter");

	if (!WriteSymPaths) return;

	m_symPathWriter = new TreeStreamWriter(getOutputFilename("symPaths.ts"));
	assert(m_symPathWriter->good());
	m_interpreter->setSymbolicPathWriter(m_symPathWriter);
}

std::string KleeHandler::getOutputFilename(const std::string &filename)
{
	char outfile[1024];
	sprintf(outfile, "%s/%s", m_outputDirectory, filename.c_str());
	return outfile;
}

std::ostream *KleeHandler::openOutputFile(const std::string &filename)
{
  std::ios::openmode io_mode = std::ios::out | std::ios::trunc
                             | std::ios::binary;
  std::string path = getOutputFilename(filename);
  std::ostream* f = new std::ofstream(path.c_str(), io_mode);
  if (!f) {
    klee_error("error opening file \"%s\" (out of memory)", filename.c_str());
  } else if (!f->good()) {
    klee_error("error opening file \"%s\".  KLEE may have run out of file "
                   "descriptors: try to increase the maximum number of open file "
                   "descriptors by using ulimit.", filename.c_str());
    delete f;
    f = NULL;
  }

  return f;
}

std::string KleeHandler::getTestFilename(const std::string &suffix, unsigned id)
{
	char filename[1024];
	sprintf(filename, "test%06d.%s", id, suffix.c_str());
	return getOutputFilename(filename);
}

std::ostream *KleeHandler::openTestFile(const std::string &suffix, unsigned id)
{
	char filename[1024];
	sprintf(filename, "test%06d.%s", id, suffix.c_str());
	return openOutputFile(filename);
}

void KleeHandler::processSuccessfulTest(
	const char	*name,
	unsigned	id,
	out_objs&	out)
{
	KTest		b;
	bool		ktest_ok;
	std::string	fname;

	b.numArgs = cmdargs->getArgc();
	b.args = cmdargs->getArgv();

	b.symArgvs = (cmdargs->isSymbolic())
		? cmdargs->getArgc()
		: 0;

	b.symArgvLen = 0;
	b.numObjects = out.size();
	b.objects = new KTestObject[b.numObjects];

	assert (b.objects);
	for (unsigned i=0; i<b.numObjects; i++) {
		KTestObject *o = &b.objects[i];
		o->name = const_cast<char*>(out[i].first.c_str());
		o->numBytes = out[i].second.size();
		o->bytes = new unsigned char[o->numBytes];
		assert(o->bytes);
		std::copy(out[i].second.begin(), out[i].second.end(), o->bytes);
	}

	errno = 0;
	fname = getTestFilename(name, id);
	ktest_ok = kTest_toFile(&b, fname.c_str());
	if (!ktest_ok) {
		klee_warning(
		"unable to write output test case, losing it (errno=%d: %s)",
		errno,
		strerror(errno));
	}

	for (unsigned i=0; i<b.numObjects; i++)
		delete[] b.objects[i].bytes;
	delete[] b.objects;

	GZip::gzipFile(fname.c_str(), (fname + ".gz").c_str());
}

bool KleeHandler::getStateSymObjs(
	const ExecutionState& state,
	out_objs& out)
{
	return m_interpreter->getSymbolicSolution(state, out);
}

#define LOSING_IT(x)	\
	klee_warning("unable to write " #x " file, losing it (errno=%d: %s)", \
		errno, strerror(errno))

/* Outputs all files (.ktest, .pc, .cov etc.) describing a test case */
void KleeHandler::processTestCase(
	const ExecutionState &state,
	const char *errorMessage,
	const char *errorSuffix)
{
	if (errorMessage && ExitOnError) {
		std::cerr << "EXITING ON ERROR:\n" << errorMessage << "\n";
		exit(1);
	}

	if (NoOutput) return;

	out_objs out;
	bool success;

	success = getStateSymObjs(state, out);
	if (!success)
		klee_warning("unable to get symbolic solution, losing test");

	double		start_time = util::estWallTime();
	unsigned	id = ++m_testIndex;

	if (success) processSuccessfulTest("ktest", id, out);

	if (errorMessage) {
		if (std::ostream* f = openTestFile(errorSuffix, id)) {
			*f << errorMessage;
			delete f;
		} else
			LOSING_IT("error");
	}

	if (WritePaths) {
		if (std::ostream* f = openTestFile("path", id)) {
			foreach(bit, state.branchesBegin(), state.branchesEnd()) {
				(*f) << (*bit).first
#ifdef INCLUDE_INSTR_ID_IN_PATH_INFO
				<< "," << (*bit).second
#endif
				<< "\n";
			}
			delete f;
			GZip::gzipFile(
				getTestFilename("path", id).c_str(),
				(getTestFilename("path", id) + ".gz").c_str());
		} else {
			LOSING_IT(".path");
		}
	}

	if (WriteSMT) {
		Query	query(	state.constraints,
				ConstantExpr::alloc(0, Expr::Bool));
				std::ostream	*f;

		f = openTestFile("smt", id);
		SMTPrinter::print(*f, query);
		delete f;

		/* these things are pretty big */
		GZip::gzipFile(
			getTestFilename("smt", id).c_str(),
			(getTestFilename("smt", id) + ".gz").c_str());
	}

	if (errorMessage || WritePCs)
		dumpPCs(state, id);

	if (m_symPathWriter) {
		std::vector<unsigned char> symbolicBranches;

		m_symPathWriter->readStream(
			m_interpreter->getSymbolicPathStreamID(state),
			symbolicBranches);
		if (std::ostream* f = openTestFile("sym.path", id)) {
			std::copy(
				symbolicBranches.begin(), symbolicBranches.end(),
				std::ostream_iterator<unsigned char>(*f, "\n"));
			delete f;
		} else
			LOSING_IT(".sym.path");
	}

	if (WriteCov) {
		std::map<const std::string*, std::set<unsigned> > cov;
		m_interpreter->getCoveredLines(state, cov);
		if (std::ostream* f = openTestFile("cov", id)) {
			foreach (it, cov.begin(), cov.end()) {
			foreach (it2, it->second.begin(), it->second.end())
				*f << *it->first << ":" << *it2 << "\n";
			}
			delete f;
		} else
			LOSING_IT(".cov");
	}

	if (WriteTraces) {
		if (std::ostream* f = openTestFile("trace", id)) {
			state.exeTraceMgr.printAllEvents(*f);
			delete f;
		} else
			LOSING_IT(".trace");
	}

	if (m_testIndex == StopAfterNTests)
		m_interpreter->setHaltExecution(true);

	if (WriteTestInfo) {
		double elapsed_time = util::estWallTime() - start_time;
		if (std::ostream *f = openTestFile("info", id)) {
			*f	<< "Time to generate test case: "
				<< elapsed_time << "s\n";
			delete f;
		} else
			LOSING_IT(".info");
	}

	const ExeStateVex	*esv = dynamic_cast<const ExeStateVex*>(&state);
	assert (esv != NULL);
	dumpLog("crumbs", id, esv->crumbBegin(), esv->crumbEnd());
	fprintf(stderr, "=====DONE WRITING OUT TESTID=%d (es=%p) ===\n",
		id, (void*)&state);
}

void KleeHandler::dumpPCs(const ExecutionState& state, unsigned id)
{
	std::string constraints;
	std::ostream* f;

	state.getConstraintLog(constraints);
	f = openTestFile("pc", id);
	if (f == NULL) {
		klee_warning(
			"unable to write .pc file, losing it (errno=%d: %s)",
			errno, strerror(errno));
		return;
	}

	*f << constraints;
	delete f;

	/* these things get big too */
	GZip::gzipFile(
		getTestFilename("pc", id).c_str(),
		(getTestFilename("pc", id) + ".gz").c_str());
}

void KleeHandler::dumpLog(
	const char* name,
	unsigned id,
	RecordLog::const_iterator begin, RecordLog::const_iterator end)
{
	if (begin == end) return;

	std::ostream* f = openTestFile(name, id);
	if (f == NULL) return;

	foreach (it, begin, end) {
		std::vector<unsigned char> r = *it;
		std::copy(
			r.begin(), r.end(),
			std::ostream_iterator<unsigned char>(*f));
	}

	delete f;

	GZip::gzipFile(
		getTestFilename(name, id).c_str(),
		(getTestFilename(name, id) + ".gz").c_str());
}

void KleeHandler::getPathFiles(
	std::string path, std::vector<std::string> &results)
{
	llvm::sys::Path p(path);
	std::set<llvm::sys::Path> contents;
	std::string error;

	if (p.getDirectoryContents(contents, &error)) {
		std::cerr << "ERROR: unable to read path directory: "
			<< path << ": " << error << "\n";
		exit(1);
	}

	foreach (it, contents.begin(), contents.end()) {
		std::string f = it->str();
		if (f.substr(f.size() - 5, f.size()) == ".path")
			results.push_back(f);
	}
}

// load a .path file
void KleeHandler::loadPathFile(std::string name, ReplayPathType &buffer)
{
	if (name.substr(name.size() - 3) == ".gz") {
		std::string new_name = name.substr(0, name.size() - 3);
		GZip::gunzipFile(name.c_str(), new_name.c_str());
		name = new_name;
	}

	std::ifstream f(name.c_str(), std::ios::in | std::ios::binary);

	if (!f.good()) {
		assert(0 && "unable to open path file");
		return;
	}

	while (f.good()) {
		unsigned value;
		f >> value;
		if(!f.good())
			break;
		f.get();
		#ifdef INCLUDE_INSTR_ID_IN_PATH_INFO
		unsigned id;
		f >> id;
		f.get();
		buffer.push_back(std::make_pair(value,id));
		#else
		buffer.push_back(value);
		#endif
	}
}

void KleeHandler::getOutFiles(
	std::string path, std::vector<std::string> &results)
{
	llvm::sys::Path			p(path);
	std::set<llvm::sys::Path>	contents;
	std::string			error;

	if (p.getDirectoryContents(contents, &error)) {
		std::cerr
			<< "ERROR: unable to read output directory: " << path
			<< ": " << error << "\n";
		exit(1);
	}

	foreach (it, contents.begin(), contents.end()) {
		std::string f = it->str();
		if (f.substr(f.size()-6,f.size()) == ".ktest")
			results.push_back(f);
	}
}
