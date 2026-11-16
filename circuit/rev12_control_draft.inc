* Draft transcription from split schematic page 1; NOT validated.
* Pins shared with audio path: OA3OUT, RMS_OUT, CONTROL_SUM, VP15, VN15.
* RMS input coupling C11 already present in audio draft.
* Original RMS module external trim and timing network.
.param RMSSYM=0.5 THRESH=0.5 RATIOPOS=0.5 LEVELCAL=0.5
R34a VN15 RMS_SYM_W {50k*RMSSYM}
R34b RMS_SYM_W VP15 {50k*(1-RMSSYM)}
R37 RMS_SYM_W RMS_SYM 1.5Meg
R38 RMS_INPUT_BRANCH RMS_IN 23.2k
* Selected timing and offset components are not assigned invented values.
* Threshold potentiometer and summing amplifier
R70a VP15 TH_W {20k*THRESH}
R70b TH_W VN15 {20k*(1-THRESH)}
R71 TH_W CMP_SUM 523k
R72 LEVEL_OUT CMP_SUM 909k
R69 LEVEL_OUT CMP_SUM 5.49k
* OA5 comparator wiring and diode network provisional; verify pin mapping
XOA5A 0 CMP_SUM VP15 VN15 CMP_OUT RC4558_SECTION
R73 VP15 LED_COMMON 1.5k
D20 LED_COMMON LED_BELOW LED_MODEL
D21 LED_ABOVE LED_COMMON LED_MODEL
D23 CMP_OUT LED_BELOW D1N4148
D24 LED_ABOVE CMP_OUT D1N4148
R75 CMP_RECT 0 100k
R74 CMP_RECT CMP_FILTER 1k
C18 CMP_FILTER RATIO_LOW 10n
D12 CMP_RECT DIODE_MID D1N4148
D13 DIODE_MID RATIO_LOW D1N4148
R77 CMP_RECT RATIO_LOW 10k
R78 RATIO_LOW RATIO_REF 100
* Ratio network, page 1, pot law and endpoint wiring require verification
R41 RATIO_TOP RATIO_LOW 7.5k
R42 RATIO_TOP RATIO_CAL_TOP 2.49k
R43a RATIO_CAL_TOP RATIO_W {50k*RATIOPOS}
R43b RATIO_W RATIO_LOW {50k*(1-RATIOPOS)}
R45 RATIO_AMP_OUT RATIO_TOP 10k
XOA5B RATIO_W RATIO_TOP VP15 VN15 RATIO_AMP_OUT RC4558_SECTION
R46 RATIO_AMP_OUT CONTROL_SUM 4.7k
.param MAKEUP=0.5
R80a VN15 MAKEUP_W {20k*MAKEUP}
R80b MAKEUP_W VP15 {20k*(1-MAKEUP)}
* Actual makeup summing junction and RMS level calibration require second read.
* No .end or simulation directives; this is an unverified transcription subset.
