#include <stdio.h>
#include <string.h>
#include <iostream>
#include "klee/Common.h"
#include "klee/Expr.h"
#include "PipeFormat.h"

using namespace klee;

const char* PipeSTP::exec_cmd = "stp";
const char* const PipeSTP::sat_args[] = {"stp", "--SMTLIB1", NULL};
const char* const PipeSTP::mod_args[] = {"stp", "--SMTLIB1",  "-p", NULL};

const char* PipeBoolector::exec_cmd = "boolector";
const char* const PipeBoolector::sat_args[] = {"boolector", NULL};
const char* const PipeBoolector::mod_args[] = {"boolector", "-fm", NULL};

const char* PipeZ3::exec_cmd = "z3";
const char* const PipeZ3::sat_args[] = {"z3", "-smt", "-in", NULL};
const char* const PipeZ3::mod_args[] = {"z3", "-smt", "-in", "-m", NULL};

const char* PipeBoolector15::exec_cmd = "boolector-1.5";
const char* const PipeBoolector15::sat_args[] =
{"boolector-1.5", "--smt1", NULL};
const char* const PipeBoolector15::mod_args[] =
{"boolector-1.5", "--smt1", "-fm", NULL};

const char* PipeCVC3::exec_cmd = "cvc3";
const char* const PipeCVC3::sat_args[] = {
	"cvc3", "-lang", "smt", NULL};
const char* const PipeCVC3::mod_args[] = {
	"cvc3", "-lang", "smt", "-output-lang", "presentation", "+model", NULL};

const char* PipeYices::exec_cmd = "yices";
const char* const PipeYices::sat_args[] = {"yices",  NULL};
const char* const PipeYices::mod_args[] = {"yices", "-f", NULL};

void PipeFormat::readArray(
	const Array* a,
	std::vector<unsigned char>& ret) const
{
	PipeArrayMap::const_iterator	it(arrays.find(a->name));
	unsigned char			default_val;

	assert (a->mallocKey.size < 0x10000000 && "Array too large");

	if (it == arrays.end()) {
		/* may want values of arrays that aren't present;
		 * this is ok-- give the default */
		ret.clear();
	} else {
		ret = it->second;
	}

	default_val = getDefaultVal(a);
	ret.resize(a->mallocKey.size, default_val);
}

unsigned char PipeFormat::getDefaultVal(const Array* a) const
{
	PipeArrayDefaults::const_iterator def_it;

	def_it = defaults.find(a->name);
	if (def_it == defaults.end())
		return 0;

	return def_it->second;
}

bool PipeFormat::parseSAT(std::istream& is)
{
	std::string	s;
	is >> s;
	return parseSAT(s.c_str());
}

bool PipeFormat::parseSAT(const char* s)
{
	if (strncmp(s, "sat", 3) == 0) {
		is_sat = true;
	} else if (strncmp(s, "unsat", 5) == 0) {
		is_sat = false;
	} else {
		return false;
	}

	return true;
}

void PipeFormat::addArrayByte(
	const char* arrName, unsigned int off, unsigned char v)
{
	std::string		arr_s(arrName);
	PipeArrayMap::iterator	it;

	if (off > 0x10000000) {
		klee_warning_once(0, "PipeFormat: Huge Index. Buggy Model?");
		return;
	}

	it = arrays.find(arrName);
	if (it == arrays.end()) {
		std::vector<unsigned char>	vec(off+1);
		vec[off] = v;
		arrays[arrName] = vec;
		return;
	}

	if (it->second.size() < off+1)
		it->second.resize(off+1);
	it->second[off] = v;
}

bool PipeSTP::parseModel(std::istream& is)
{
	char	line[512];

	arrays.clear();
	defaults.clear();

	while (is.getline(line, 512)) {
		char		*cur_buf = line;
		char		arrname[128];
		size_t		sz;
		uint64_t	off, val;

		if (memcmp("ASSERT( ", line, 8) != 0)
			break;
		cur_buf += 8;
		// ASSERT( const_arr1[0x00000054] = 0x00 );
		// Probably the most complicated sscanf I've ever written.
		sz = sscanf(cur_buf, "%[^[][0x%lx] = 0x%lx );", arrname, &off, &val);
		assert (sz == 3);
		addArrayByte(arrname, off, (unsigned char)val);
	}

	return parseSAT(line);
}

static uint64_t bitstr2val(const char* bs)
{
	uint64_t ret = 0;
	for (unsigned int i = 0; bs[i]; i++) {
		ret <<= 1;
		ret |= (bs[i] == '1') ? 1 : 0;
	}
	return ret;
}

bool PipeYices::parseModel(std::istream& is)
{
	char		line[512];
	char		cur_arrname[128];
	unsigned int	cur_max;
	std::set<int>	cur_used;

	arrays.clear();
	defaults.clear();

	if (!parseSAT(is)) return false;
	if (!isSAT()) return true;

	is.getline(line, 512);	/* blank line */
	is.getline(line, 512);	/* blank, again */
	is.getline(line, 512);	/* MODEL */
	if (strcmp(line, "MODEL") != 0) return false;

/*
	sat

	MODEL
	--- const_arr1 ---
	(= (const_arr1 0b00000000000000000000000000001111) 0b00000100)
	default: 0b00101110
	--- qemu_buf7 ---
	(= (qemu_buf7 0b00000000000000000000000000001111) 0b00101110)
	default: 0b01100100
	----
*/
	cur_arrname[0] = '\0';
	cur_max = 0;
	while (is.getline(line, 512)) {
		char		in_arrname[128];
		size_t		sz;
		char		off_bitstr[128], val_bitstr[128];
		uint64_t	off, val;

		sz = sscanf(line, "--- %s ---", in_arrname);
		if (sz == 1) {
			if (defaults.count(cur_arrname)) {
				unsigned char def = defaults[cur_arrname];
				for (unsigned i = 0; i < cur_max; i++) {
					if (cur_used.count(i))
						continue;
					addArrayByte(cur_arrname, i, def);
				}
			}
			strcpy(cur_arrname, in_arrname);
			cur_used.clear();
			cur_max = 0;
			continue;
		}

		sz = sscanf(line, "(= (%s 0b%[01]) 0b%[01])",
			in_arrname, off_bitstr, val_bitstr);
		if (sz == 3) {
			off = bitstr2val(off_bitstr);
			val = bitstr2val(val_bitstr);
			cur_used.insert(off);
			addArrayByte(cur_arrname, off, (unsigned char)val);
			if (cur_max < off) cur_max = off;
			continue;
		}

		sz = sscanf(line, "default: 0b%[01]", val_bitstr);
		if (sz == 1) {
			defaults[cur_arrname] = bitstr2val(val_bitstr);
			continue;
		}
	}

	return true;
}


bool PipeCVC3::parseModel(std::istream& is)
{
	char	line[512];

	arrays.clear();
	defaults.clear();

	is.getline(line, 512);
	if (strcmp(line, "Satisfiable.") != 0) {
		std::cerr << "Not a SAT model??\n";
		return false;
	}
	is_sat = true;

	while (is.getline(line, 511)) {
		char		arrname[128];
		char		off_bitstr[128], val_bitstr[128];
		size_t		sz;
		uint64_t	off, val;

		if (strlen(line) > 256)
			continue;
		// XXX
		// ARGH. IT HAS A FULL GRAMMAR ALA
		// ASSERT (reg4 = (ARRAY (arr_var: BITVECTOR(32)): 0bin01111111));
		// OH WELL.
		sz = sscanf(line, "ASSERT (%[^[][0bin%[01]] = 0bin%[01]);",
			arrname, off_bitstr, val_bitstr);
		if (sz != 3) {
			continue;
		}

		off = bitstr2val(off_bitstr);
		val = bitstr2val(val_bitstr);
		addArrayByte(arrname, off, (unsigned char)val);
	}

	return true;
}

bool PipeBoolector::parseModel(std::istream& is)
{
	char	line[512];

	arrays.clear();
	defaults.clear();

	is.getline(line, 512);

	if (!parseSAT(line)) return false;
	if (!is_sat) return true;

	// const_arr1[00000000000000000000000000000110] 00000100
	while (is.getline(line, 512)) {
		char		arrname[128];
		char		idx_bits[128];
		char		val_bits[128];
		size_t		sz;
		uint64_t	idx, val;

		sz = sscanf(line, "%[^[][%[01]] %[01]",
			arrname, idx_bits, val_bits);
		assert (sz == 3);

		idx = bitstr2val(idx_bits);
		val = bitstr2val(val_bits);
		addArrayByte(arrname, idx, (unsigned char)val);
	}

	return true;
}

bool PipeZ3::parseModel(std::istream& is)
{
	char				line[512];
	std::map<int, std::string>	idx2name;

	arrays.clear();
	defaults.clear();

	//const_arr1 -> as-array[k!0]
	//k!0 -> {
	//  bv10[32] -> bv4[8]
	//  else -> bv4[8]
	//}
	//k!1 -> {
	//  bv2[32] -> bv100[8]
	//}
	//sat
	while (is.getline(line, 512)) {
		char	arrname[128];
		int	default_val;
		int	arrnum;
		size_t	sz;

		// An array declaration,
		//qemu_buf7 -> as-array[k!1]
		sz = sscanf(line, "%s -> as-array[k!%d]", arrname, &arrnum);
		if (sz == 2) {
			idx2name[arrnum] = arrname;
			continue;
		}

		/* beginning of an array model */
		sz = sscanf(line, "k!%d -> {", &arrnum);
		if (sz == 1) {
			// use arrnum to lookup name
			if (idx2name.count(arrnum) == 0) {
				std::cerr
					<< "Unexpected array index "
					<< arrnum
					<< std::endl;
				return false;
			}

			if (!readArrayValues(is, idx2name[arrnum]))
				return false;
			continue;
		}

		/* constant array */
		sz = sscanf(line, "%s -> Array!val!%d", arrname, &default_val);
		if (sz == 2) {
			defaults[arrname] = (unsigned char)default_val;
			continue;
		}

		/* 'sat' expected */
		break;
	}

	return parseSAT(line);
}

bool PipeZ3::readArrayValues(std::istream& is, const std::string& arrname)
{
	char		line[512];
	std::set<int>	used_idx;
	unsigned int	max_idx = 0;
	unsigned char	cur_default = 0;

	while (is.getline(line, 512)) {
		size_t		sz;
		unsigned int	idx, val;

		if (line[0] == '}')
			break;

		sz = sscanf(line, "  bv%u[32] -> bv%u[8]", &idx, &val);
		if (sz == 2) {
			used_idx.insert(idx);
			if (max_idx < idx) max_idx = idx;
			addArrayByte(arrname.c_str(), idx, val);
			continue;
		}

		sz = sscanf(line, "  else -> bv%u[8]", &val);
		if (sz == 1) {
			defaults[arrname] = (unsigned char)val;
			cur_default = val;
			continue;
		}

		return false;
	}

	/* patch up 0's */
	for (unsigned int i = 0; i < max_idx; i++) {
		if (used_idx.count(i))
			continue;
		addArrayByte(arrname.c_str(), i, cur_default);
	}

	return true;
}
