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
* Timing component values below are provisional modern application values,
* not claimed original 209 values; need calibration against factory timing.
.subckt BLACKMER_RMS IN LEVEL VP VN REF params: RT=2Meg CTVAL=10u
RTIME VP IT {RT}
CTIME CT VN {CTVAL}
XDETECT IN IT CT LEVEL VN VP REF 4301RMS
.ends BLACKMER_RMS
* Integrated signal split and VCA I/V converter.
* External threshold/ratio network drives EC_MINUS; no fabricated control law.
.subckt BLACKMER_AUDIO OA3OUT EC_MINUS LEVEL AUDIO_OUT VP VN REF
C11 OA3OUT RMS_AC 330n
R38 RMS_AC RMS_IN 23.2k
XDET RMS_IN LEVEL VP VN REF BLACKMER_RMS
C12 OA3OUT VCA_AC 330n
R26 VCA_AC VCA_IN 100k
XVCA VCA_IN VCA_OUT EC_MINUS VP VN REF BLACKMER_VCA
R32 VCA_OUT AUDIO_OUT 100k
C14 VCA_OUT AUDIO_OUT 22p
XOA4 REF VCA_OUT VP VN AUDIO_OUT RC4558_SECTION
.ends BLACKMER_AUDIO
* Requires verified RC4558_SECTION wrapper. Replace duplicate C11/C12/R26/
* R38/R32/C14 in earlier draft when assembling final top-level circuit.
* Not a standalone reference netlist; control and output network remain unverified.
