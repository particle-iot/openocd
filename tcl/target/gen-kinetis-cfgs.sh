#!/bin/bash

gen_cfg () {
  local name=$1
  local tapid=$2
  cp kinetis_template.cfg $name.cfg
  sed -i "s/SOMNNAME/$name/g" $name.cfg
  sed -i "s/SOMNTAPID/$tapid/g" $name.cfg
}

gen_cfg kl25 0x0BC11477
gen_cfg kl26 0x0BC11477
gen_cfg k64 0x4BA00477
gen_cfg k21 0x4BA00477
gen_cfg k22 0x4BA00477
gen_cfg kl46 0x0BC11477
gen_cfg k40 0x4ba00477
gen_cfg k60 0x4ba00477
