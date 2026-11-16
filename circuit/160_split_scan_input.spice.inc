* PARTIAL TRANSCRIPTION — not a runnable reference circuit.
* Source: https://audiocircuit.dk/downloads/dbx/DBX-160-lim-sch.pdf page 1.
* Split scan revision must be reconciled against assembly 26018.2 before merging.
* Directly visible topology: two input legs with two shunt capacitors each,
* differential OA stage with 20k input and 10k feedback/reference arms.
* P3-4 is upper/inverting leg, P3-6 lower/noninverting leg.
* Node names are transcription aliases. Values require independent second read.
.subckt INPUT_PASSIVES P3_4 P3_6 P3_3 OA3_MINUS OA3_PLUS OA3_OUT VCA_IN
R81 P3_4 upper_filtered 2.2k
R20 P3_6 lower_filtered 2.2k
C19 P3_4 P3_3 1n
C9 P3_6 P3_3 1n
C20 upper_filtered P3_3 1n
C10 lower_filtered P3_3 1n
R21 upper_filtered OA3_MINUS 20k
R22 lower_filtered OA3_PLUS 20k
R23 OA3_PLUS P3_3 10k
R24 OA3_OUT OA3_MINUS 10k
C12 OA3_OUT vca_coupled 330n
R26 vca_coupled VCA_IN 100k
.ends INPUT_PASSIVES
* OA3 is deliberately NOT instantiated with an ideal or substitute model.
* Resolve exact IC marking and acquire validated model before integration.
* C11 RMS branch, R25 VCA reference, M1 pins and all remaining circuit
* elements are outside this verified subset and not represented by substitutes.
* No simulation or accuracy assertion is authorized by this file.
