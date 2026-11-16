* Authorized modern Blackmer substitution, not original 200 module.
* Source: THAT2181 datasheet figure 15.
* Requires actual manufacturer THAT2181B subcircuit with package-number
* pin order 1 2 3 4 5 6 7 8; verify header before use.
* External control input is Ec- voltage, not original module control current.
* Original 200 control network needs conversion/calibration before connection.
.subckt VCA2181_ADAPTER IN OUT EC_MINUS VP VN GND params: SYM=0.5
RNEG VNEG VN 5.1k
RSYMTOP VP TRIMW {50k*(1-SYM)}
RSYMBOT TRIMW VN {50k*SYM}
RSYM TRIMW SYMPIN 220k
XCORE IN GND EC_MINUS SYMPIN VNEG GND VP OUT THAT2181B
.ends VCA2181_ADAPTER
* Intended instance (not yet integrated into original control network):
* XM1 VCA_IN VCA_OUT EC_MINUS VP15 VN15 0 VCA2181_ADAPTER
* Retains original R26=100k, R32=100k, C14=22p externally.
* Do not connect original R29/Q7 bias network to modern VCA supply pins.
