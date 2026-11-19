* dbx 340140 rev 02 (1981). Draft transcription; junction interpretation pending validation.
* Factory-selected resistors parameterized; guessed starting values, not calibrated.
* Internal part codes mapped to provisional complementary silicon models.
.subckt DBX202C IN OUT EC SYM VP VN REF params: R1S=82k R5S=330k R6S=330k R14S=1e12 R18S=4.7 R21S=4.7
R1 VP JDR {R1S}
R2 JDR Q2B 220
R3 VP Q2C 2.2k
R4 VP Q3E 390
C1 JDR Q3E 560p
J1 Q2B IN REF LEG_J
Q2 Q2C Q2B Q2E LEG_N
R25 Q2E REF 100
Q3 PE Q2C Q3E LEG_P
R5 VP Q5E {R5S}
Q5 PE Q5B Q5E LEG_P
Q4 Q5E Q4B NE LEG_N
R9 PE Q4B 2.74k
R10 Q4B NE 3.32k
R12 NE VN 3.32k
Q31 IN PBI PE LEG_P
Q32 OUT PBO PE LEG_P
R8 PBI REF 75
Q41 IN NBI NE LEG_N
Q42 OUT NBO NE LEG_N
R13 NBO REF 75
* Upper auxiliary matched pairs; verify junctions against scan before claims.
Q11 U1 REF PE LEG_P
Q12 U2 PBO PE LEG_P
Q21 U1 U2 UTAIL LEG_N
Q22 U2 U2 UTAIL LEG_N
R6 SYM U1 {R6S}
R7 U1 REF 100
R11 UTAIL VN 3.32k
D2 REF UTAIL LEG_D
* Lower matched auxiliary pairs
Q51 L1 REF NE LEG_N
Q52 L2 NBI NE LEG_N
Q61 L1 L1 LTAIL LEG_P
Q62 L2 L1 LTAIL LEG_P
R24 VP LTAIL 3.32k
D3 LTAIL REF LEG_D
R14 NBO L2 {R14S}
R23 L2 REF 100
R15 EC CT 750
R16 CT NBI 4.7
R17 CT PBO 4.7
R18 CT AUX {R18S}
R19 NBI CB 4.7
R20 PBO CB 4.7
R21 AUX CB {R21S}
R22 CB REF 45.3
* Q5 base connection is provisional reading of scan.
RQB Q5B Q4B 1m
.ends DBX202C
