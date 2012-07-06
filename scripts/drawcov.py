#!/usr/bin/python

# This creates visualization of the hits in the memory map
# In the future, it would be nice to get source-level info from this

import sys
import os
import Image
import ImageColor
import colorsys

class MemEnt:
	def __init__(self, l):
	# 0x7f12718c2000-0x7f1271a47000 5 0 /lib64/libc-2.14.1.so
		l = l.replace('-',' ')
		l = l.rstrip('\r\n ')
		l = l.split(' ')
		self.begin = int(l[0],0)
		self.end = int(l[1],0)
		self.prot = int(l[2])
		self.flags = int(l[3])
		if len(l) > 4:
			self.name = l[4].split('/')[-1]
		else:
			self.name = None

	def write(self):
		print "%x-%x %d" % (self.begin, self.end, self.prot)

	def is_exec(self):
		return (self.prot & 0x4) != 0

	@staticmethod
	def loadFromFile(fname):
		maptab = dict()
		mapf=open(fname,'r')
		for l in mapf:
			m = MemEnt(l)
			maptab[m.begin] = m
		mapf.close()
		return maptab

def getFile(fname):
	if fname == '-':
		return sys.stdin
	else:
		return open(fname, 'r')

def loadVisitedByInsAddrFile(insAddrFile):
	visited_tab = dict()
	f = getFile(insAddrFile)
	last_ins_addr=0
	for l in f:
		cur_ins_addr=int(l,0)
		dist=last_ins_addr-cur_ins_addr
		if dist < 0 or dist > 16:
			visited_tab[last_ins_addr] = 16
		else:
			visited_tab[last_ins_addr] = cur_ins_addr - last_ins_addr
		last_ins_addr = cur_ins_addr
	visited_tab.pop(0)
	print "Loaded ins addr file"
	return visited_tab

def loadVisitedByFile(fname):
	visited_tab = dict()
	f = getFile(fname)
	for l in f:
		# 0xaddress-0xaddress  (e.g. [a, b) )
		(addr_begin, addr_end) = l.split('-')
		(addr_begin_i, addr_end_i) = (int(addr_begin, 0), int(addr_end, 0))
		v = l.split(' ')
		visited_tab[addr_begin_i] = addr_end_i - addr_begin_i

	return visited_tab


# symtab is a list sorted by address of (address, func_name, length) tuples
def loadSyms(guestpath):
	symtab = []
	f = open(guestpath + "/syms", 'r')
	# example: xdr_netobj 7f17cac14b70-7f17cac14b81
	for l in f:
		(func_name, addr_range) = l.split(' ')
		(addr_begin, addr_end) = addr_range.split('-')
		addr_begin = int('0x'+addr_begin,0)
		addr_end = int('0x'+addr_end,0)
		symtab.append((addr_begin, func_name, addr_end-addr_begin))
	f.close()
	symtab.sort()
	return symtab

# TODO: Binary search
addr2sym_memo = (0, 'DERP DERP BAD FUNC', 1)
def addr2sym(symtab, addr):
	global addr2sym_memo

	if addr < addr2sym_memo[0]+addr2sym_memo[2] and \
	   addr > addr2sym_memo[0]:
	  	return addr2sym_memo


	last_s = symtab[0]
	for s in symtab:
		if s[0] > addr:
			if addr < (last_s[0]+last_s[2]):
				addr2sym_memo = last_s
				return last_s
			return None
		last_s = s

	if last_s[0] > addr or addr > (last_s[0]+last_s[2]):
		return None

	addr2sym_memo = last_s
	return last_s

# Format
# function <total bytes> <total bytes covered>
# ex: malloc 200 100
def saveFuncCov(symtab, visited_tab, outdir):
	funcs_map = dict()
	print "Assigning coverages (get some coffee)"

	visited_list = list(set(visited_tab.items()))
	visited_list.sort()
	for (v_addr, v_bytes) in visited_list:
		sym = addr2sym(symtab, v_addr)
		if sym is None:
#			print 'NOTFOUND '+ str(v_addr)
			continue

		if sym[1] not in funcs_map:
			funcs_map[sym[1]] = (sym[2], 0)
		(tot,cov) = funcs_map[sym[1]]
		funcs_map[sym[1]] = (tot, cov + v_bytes)

	print "Coverages assigned. Back to work."
	outfname = outdir + '/funcov.txt'
	f = open(outfname, 'w')
	for (func_name, (tot, cov)) in funcs_map.items():
		f.write("%s %d %d\n" % (func_name, tot, cov))
	f.close()

def ent2img(visited_tab, m):
	global red

	target_px_w=640
	target_px_h=480
	target_px=target_px_w*target_px_h
	target_dim = (target_px_w, target_px_h)
	m.write()

	# only care about coverage for regions that are exec
	if not m.is_exec():
		return None

	# ignore empty regions, if any
	if m.end - m.begin == 0:
		return None

	pixels_per_byte = float(target_px)/float(m.end - m.begin)
	if pixels_per_byte == 0:
		print "0 pixels per byte. Whoops"
		return	None

	im = Image.new("RGB", target_dim, ImageColor.getrgb("black"))
	for (k, num_bytes) in visited_tab.items():
		if k < m.begin or k+num_bytes > m.end:
			continue
		offset=int(pixels_per_byte*(k-m.begin))
		for i in range(0, int(num_bytes*pixels_per_byte)):
			x = (offset+i) % target_px_w
			y = (offset+i) / target_px_w
			im.putpixel((x,y), red)
	return im


from optparse import OptionParser
op = OptionParser("usage: %prog guestpath [istats-file]")
op.add_option(
	'-v',
	'--visited-file',
	dest='visitedFile',
        action="store",
	type="string")
op.add_option(
	'-i',
	'--insaddr-file',
	dest='insAddrFile',
	action="store",
	type='string')
op.add_option(
	'-o',
	'--output-dir',
	dest='outputDir',
	action='store',
	default='.',
	type='string')
opts,args = op.parse_args()

#if len(args) == 0:
#	op.error("invalid arguments")

if len(args) == 0:
	guestpath='guest-last'
	opts.visitedFile = '-'
else:
	guestpath=args[0]

print "GUESTPATH: " + guestpath

maptab = MemEnt.loadFromFile(guestpath+"/mapinfo")
symtab = loadSyms(guestpath)

if opts.visitedFile:
	print "Visit file: " + opts.visitedFile
	visited_tab = loadVisitedByFile(opts.visitedFile)
elif opts.insAddrFile:
	visited_tab = loadVisitedByInsAddrFile(opts.insAddrFile)

# XXX this is broken
# print "SaveFuncCov"
# saveFuncCov(symtab, visited_tab, opts.outputDir)

red =  ImageColor.getrgb("red")

for m in maptab.values():
	im = ent2img(visited_tab, m)
	if im is None:
		continue

	outfname = 'cov-0x%x' % m.begin
	if m.name is not None:
		outfname = outfname + '-' + m.name
	outfname = outfname + '.png'
	if opts.outputDir:
		outfname = "%s/%s" % (opts.outputDir,outfname)
	im.save(outfname)
