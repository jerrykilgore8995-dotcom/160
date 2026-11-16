* Modern Blackmer replacement cells, engineering candidate (not original 200/209).
* Manufacturer archive Rev05: 2181_Macro_01.lib and 4301_Macro_01.lib.
* Verified subcircuit headers from GitHub run 43803360179.
* Include vendor files before this file. No duplicate harmonic generator.
* EC_MINUS is voltage control, positive voltage increases attenuation.
.subckt BLACKMER_VCA IN OUT EC_MINUS VP VN REF params: SYM=0.5
RBIAS VEE VN 5.1k
RTRIM1 VP W {50k*(1-SYM)}
RTRIM2 W VN {50k*SYM}
RSYM W SYM_IN 220k
XGAIN IN REF EC_MINUS SYM_IN VEE REF VP OUT THAT2181B
.ends BLACKMER_VCA
* RMS current input; timing-current resistor and timing capacitor retained.
* THAT4301 datasheet Rev07 p10: IT resistor to NEGATIVE rail; CT to ground.
* 2Meg at -15V gives 7.5uA; 1.1 mirror yields 8.25uA.
* CT=10uF gives nominal 126.9 dB/s at 6.5mV/dB.
* CT=10.576923uF targets 120 dB/s analytically at nominal scale.
* This is derived retrofit timing, not a measured original 209 equivalence.
.subckt BLACKMER_RMS IN LEVEL VP VN REF params: RT=2Meg CTVAL=10.576923u
RTIME IT VN {RT}
CTIME CT REF {CTVAL}
XDETECT IN IT CT LEVEL VN VP REF 4301RMS
.ends BLACKMER_RMS
* Integrated signal split and VCA I/V converter.
* External threshold/ratio network drives EC_MINUS; no fabricated control law.
.subckt BLACKMER_AUDIO OA3OUT EC_MINUS LEVEL AUDIO_OUT VP VN REF params: SYM=0.5
C11 OA3OUT RMS_AC 330n
R38 RMS_AC RMS_IN 23.2k
XDET RMS_IN LEVEL VP VN REF BLACKMER_RMS
C12 OA3OUT VCA_AC 1.65u
R26 VCA_AC VCA_IN 20k
XVCA VCA_IN VCA_OUT EC_MINUS VP VN REF BLACKMER_VCA SYM={SYM}
R32 VCA_OUT AUDIO_OUT 20k
C14 VCA_OUT AUDIO_OUT 110p
XOA4 REF VCA_OUT VP VN AUDIO_OUT RC4558_SECTION
.ends BLACKMER_AUDIO
* Requires verified RC4558_SECTION wrapper. Replace duplicate C11/C12/R26/
* R38/R32/C14 in earlier draft when assembling final top-level circuit.
* Not a standalone reference netlist; control and output network remain unverified.
