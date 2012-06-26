#!/bin/bash
CURDATE=`date +"%s-%N"`
FPREFIX="pending.$CURDATE"
PROOFDIR=${1:-"proofs"}

function xtive_loop
{
	BRULEF="$1"
	loopc=0
	while [ 1 ]; do
		APPENDED=`kopt -max-stp-time=30 -pipe-solver -brule-xtive -rule-file="$BRULEF" 2>&1 | grep Append | cut -f2 -d' '`
		if [ -z "$APPENDED" ]; then
			break
		fi
		if [ "$APPENDED" -eq "0" ]; then
			break
		fi

		loopc=$(($loopc + 1))
		if [ "$loopc" -gt 8 ]; then
			break;
		fi

		kopt -dedup-db -rule-file="$BRULEF" $BRULEF.tmp 2>&1
		mv $BRULEF.tmp $BRULEF
	done
}

if [ -f "$PROOFDIR" ]; then
	cp "$PROOFDIR" "$FPREFIX".brule
else
	kopt -max-stp-time=30 -pipe-solver -dump-bin -check-rule "$PROOFDIR" >$FPREFIX.brule
fi

if [ -f "$SEEDBRULE" ]; then
	cat "$SEEDBRULE" >>$FPREFIX.brule
fi

xtive_loop "$FPREFIX.brule"


kopt -pipe-solver -brule-rebuild -rule-file=$FPREFIX.brule $FPREFIX.rebuild.brule 2>/dev/null
mv $FPREFIX.rebuild.brule $FPREFIX.brule
kopt -max-stp-time=30 -pipe-solver -db-punchout		\
	-ko-consts=16					\
	-rule-file=$FPREFIX.brule			\
	-unique-file=$FPREFIX.uniq.brule		\
	-uninteresting-file=$FPREFIX.unint.brule	\
	-stubborn-file=$FPREFIX.stubborn.brule		\
	$FPREFIX.punch.brule
xtive_loop "$FPREFIX.punch.brule"
kopt -pipe-solver -brule-rebuild -rule-file=$FPREFIX.punch.brule $FPREFIX.punch.rebuild.brule
mv $FPREFIX.punch.rebuild.brule $FPREFIX.punch.brule
cat $FPREFIX.{punch,uniq,unint,stubborn}.brule | gzip -c >$FPREFIX.final.brule.gz