#!/bin/bash
# runs every test scene one after the other (about 5 min each); summary in $S/SCENES_RESULT.txt
S=${MP_S:-/tmp/mp_test}
: > $S/SCENES_RESULT.txt
for sc in ${SCENES:-disarm steal useitem armor console recraft workout}; do
  bash $S/scene.sh $sc
  case $sc in armor) n=ARMORNPC ;; *) n=$(echo $sc | tr a-z A-Z) ;; esac
  echo "$sc: $(grep -a "^PASS\|^FAIL" $S/${n}_RESULT.txt | head -1)" >> $S/SCENES_RESULT.txt
done
echo SCENES DONE >> $S/SCENES_RESULT.txt
