# Native circuit solver development

The native engine compiles the connected 160 retrofit deck into a modified nodal system. It expands the RC4558, THAT2181B and 4301RMS manufacturer models and the discrete BC341/BC361 output stage. Linear controlled sources, junction currents and charge storage are part of the solve. The solver precomputes the sparse LU schedule and performs bounded Newton iteration with BDF2 capacitor history. Circuit state updates run in C++ with no external simulator calls or audio-path allocation.

Build preparation uses ngspice to capture the nominal power-on state at a 0.2 V RMS threshold, 4:1 ratio, and zero makeup. ngspice also supplies the independent PWL comparison. The native solver currently uses quiet rails, the deck’s 600 Ω load, and one integration step per host sample. The solver source is under development. The offline VST3 target still uses its existing implementation.

Prepare the vendor models with `mkii/prepare_models.py`, build `mkii/SpiceWorker.cpp` against the ngspice shared library, and put `spice2poly.cm` beside the deck. Run `python mkii/native/prepare.py WORKER CIRCUIT_DIR GENERATED_DIR/Deck.hpp`. Compile `Probe.cpp` in C++20 with the generated include directory and optimization enabled. The probe reports convergence and deadline misses on two simultaneous channels at 44.1 and 48 kHz. `--gate` fails the command on any miss. `--capture` writes the native 1 kHz fixture for comparison against the original circuit.

The native audio path still requires sustained real-time performance and reliable convergence on complex transients before it is ready for a VST3 build. Full parameter, high-frequency, noise and hardware validation remains open.
