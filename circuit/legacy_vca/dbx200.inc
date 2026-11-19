* dbx drawing 110057 rev 07, 1975. Experimental transcription.
* Current input/output, four independent base control terminals.
* Selected resistances are provisional, NOT factory values.
.subckt DBX200 IN OUT PI PO NI NO VP VN REF params: R1S=82k R10S=1e12 R2S=2.2k R13S=2.2k R11S=1e12
R1 VP JDR {R1S}
R10 VP JDR {R10S}
R3 VP Q1C 2.2k
R4 VP Q2E 390
R5 C1R Q2E 220
C1 JDR C1R 560p
Q1 Q1C JDR Q1E LEG_N
D1 Q1E REF LEG_D
Q2 PE Q1C Q2E LEG_P
J3 JDR IN REF LEG_J
Q4 IN PI PE LEG_P
Q5 OUT PB PE LEG_P
Q6 PE BIAS NE LEG_N
Q7 IN NB NE LEG_N
Q8 OUT NO NE LEG_N
R6 PE BIAS 390
R7 BIAS NE 1k
R8 NE VN 4.7k
R9 NI NB 22
R2 NB REF {R2S}
R12 PO PB 22
R13 PB REF {R13S}
R11 NO VN {R11S}
.ends DBX200
