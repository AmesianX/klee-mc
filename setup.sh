#!/bin/bash

VEXLLVMDIR=${VEXLLVMDIR:-"/home/chz/src/vex/"}
LLVMSRCDIR=${LLVMSRCDIR:-"/home/chz/src/llvm/llvm-3.8.0.src/"}
LLVMOBJDIR=${LLVMOBJDIR:-"/home/chz/src/llvm/llvm-3.8.0-build/"}
STPDIR=${STPDIR:-"/home/chz/src/stp-fast/stp/"}
#BOOLECTORDIR=${BOOLECTORDIR:-"/home/chz/src/boolector/"}
#Z3DIR=${Z3DIR:-"/home/chz/src/z3/"}
UCLIBDIR=${UCLIBDIR:-"/home/chz/src/uclibc-pruning"}
VEXLIBDIR=${VEXLIBDIR:-"/usr/lib/valgrind/"}

if [ -z "$QUICK" ]; then
LLVM_CFLAGS_EXTRA="$EXTRAHEADERS"			\
LLVM_CXXFLAGS_EXTRA="$EXTRAHEADERS"			\
CFLAGS="-g -O3 -I${STPDIR}/include $EXTRAHEADERS"	\
CXXFLAGS="-g -O2 -std=c++14  $EXTRAHEADERS"		\
	./configure					\
		--with-llvmsrc="$LLVMSRCDIR"		\
		--with-llvmobj="$LLVMOBJDIR"		\
		--with-libvex="$VEXLIBDIR"		\
		--with-vexllvm="$VEXLLVMDIR"		\
		--with-stp="$STPDIR"			\
		--enable-posix-runtime			\
		--with-uclibc="$UCLIBDIR"		\
		--with-runtime=Release 			\
		--with-boolector="$BOOLECTORDIR"	\
		--with-z3="$Z3DIR"			\

ret=$?
if [ "$ret" -ne 0 ]; then
	echo failed to config
	exit 1
fi

fi

repohash=`git log -1 | grep commit  | cut -f2 -d' '`
binhash=`klee-mc 2>&1 | grep commit | cut -f2 -d':' | xargs echo`
if [ "$repohash" != "$binhash" ]; then
	rm tools/klee-mc/{Release,Release+Asserts}/*.o
fi


make -j6 REQUIRES_RTTI=1
if [ ! -z "$QUICK" ]; then
	exit
fi

pushd "$VEXLLVMDIR"
make install-klee
popd

make mc-clean && GUEST_ARCH=arm make -j6
make mc-clean && GUEST_ARCH=x86 make -j6
make mc-clean && GUEST_ARCH=amd64 make -j6
make nt-clean && GUEST_WIN=xp make -j6
make nt-clean && GUEST_WIN=win7 make -j6

BASEDIR="Release+Asserts"
if [ ! -x $BASEDIR ]; then
	BASEDIR="Release"
fi

if [ -z "$VEXLLVM_HELPER_PATH" ]; then
	echo "Can't find vex bitcode path. Not copying libkleeRuntimeMC.bc"
	exit 1
fi

echo "Copying runtimes..."
for a in arm amd64 x86 xp win7 fdt sysnone; do
	ls -l "$BASEDIR"/lib/libkleeRuntimeMC-$a.bc
	echo "COPYING $a to $VEXLLVM_HELPER_PATH"
	cp "$BASEDIR"/lib/libkleeRuntimeMC-$a.bc "$VEXLLVM_HELPER_PATH"/
done


cp "$VEXLLVM_HELPER_PATH"/softfloat.bc "$BASEDIR"/lib/
cp "$VEXLLVM_HELPER_PATH"/softfloat-fpu.bc "$BASEDIR"/lib/
