# CPU16 Software CPU & Program Layout/Execution

**CMPE 220 – System Software (Spring 2026)**  
**Author:** Faiq Malik  
**Repository:** [https://github.com/Faiq2001/software_cpu_design-main](https://github.com/Faiq2001/software_cpu_design-main)

This repository contains two related assignments:

1. **CPU16 — Software CPU Design** — 16-bit ISA, assembler, and emulator in C++ (`cpu16_core.cpp`).
2. **Program Layout & Execution** — Recursion, call stack, and memory layout using the C factorial program (`factorial.c`) and CPU16 assembly images.

---

## Project overview

CPU16 is a fully software-implemented 16-bit CPU written in C++.

Features include:

- CPU core (registers, ALU, condition flags)
- Instruction set architecture (ISA)
- Memory and MMIO (UART and timer)
- Two-pass assembler
- Emulator with memory dump support (`asm`, `emu`, `run`)

Example assembly programs (under `examples/` when that directory is present):

- `hello.asm`
- `timer.asm`
- `fib.asm`
- `fact.asm` (factorial / recursion)

---

## Project structure

```
software_cpu_design-main/
├── README.md
├── cpu16_core.cpp
├── cpu16
├── factorial.c
├── factorial
├── fact.bin
├── fib.bin
├── demos/
│   ├── Demo.mp4
│   └── Demo_Recursion_Memory.mp4
├── figures/
│   └── Sample Drawing.png
├── examples/
│   ├── hello.asm
│   ├── timer.asm
│   ├── fib.asm
│   └── fact.asm
└── reports/
    ├── Assignment1_Software_CPU_Report.pdf
    └── Assignment2_Program_Layout_Report.pdf
```

## Clone

```bash
git clone https://github.com/Faiq2001/software_cpu_design-main.git
cd software_cpu_design-main
```

---

## Requirements

- macOS or Linux  
- `g++` with C++17 support  
- `gcc` with C11 support (for the factorial program)  
- Terminal or shell environment

---

## Compilation

Build CPU16:

```bash
g++ -std=c++17 -O2 -o cpu16 cpu16_core.cpp
```

This produces the executable `cpu16`.

Build the C factorial program:

```bash
gcc -std=c11 -O2 -o factorial factorial.c
```

---

## Running example programs

### 1. Hello world

```bash
./cpu16 run examples/hello.asm
```

Expected:

```
Hello, World!
```

### 2. Timer example

```bash
./cpu16 run examples/timer.asm
```

Expected:

```
STimer
```

### 3. Fibonacci example

Assemble:

```bash
./cpu16 asm examples/fib.asm -o fib.bin
```

Emulate (Fibonacci is linked for execution at `0x0100`):

```bash
./cpu16 emu fib.bin --base 0x0000 --pc 0x0100 --dump 0x0100 0x0140
```

If you use the prebuilt `fib.bin` in the repository root, keep the same `emu` arguments and pass `fib.bin` as the image file.

---

## Memory-mapped I/O (MMIO)


| Address | Register         | Description                           |
| ------- | ---------------- | ------------------------------------- |
| 0xFF00  | `UART_OUT`       | Write a byte to the simulated console |
| 0xFF01  | `UART_IN` (stub) | Simulated input path                  |
| 0xFF10  | TIMER (low)      | Timer low byte                        |
| 0xFF11  | TIMER (high)     | Timer high byte                       |
| 0xFF12  | TIMERCMP (low)   | Compare low byte                      |
| 0xFF13  | TIMERCMP (high)  | Compare high byte                     |
| 0xFF14  | IRQ / ACK        | Pending flag / acknowledge behavior   |


---

## Program layout & execution (recursion)

This part of the project demonstrates recursion, stack behavior, and memory layout in C and on CPU16.

### C recursive factorial

Source: `factorial.c`

Compile:

```bash
gcc -std=c11 -O2 -o factorial factorial.c
```

Run:

```bash
./factorial
```

Example:

```
Enter a number: 5
Factorial of 5 = 120
```

### CPU16 recursive factorial (assembly)

Assembly source: `examples/fact.asm`

Run directly:

```bash
./cpu16 run examples/fact.asm
```

This demonstrates:

- `CALL` / `RET`
- Stack frame creation
- Recursive expansion
- Stack unwinding
- Returning values through the simulated machine

### Examine memory layout

Assemble:

```bash
./cpu16 asm examples/fact.asm -o fact.bin
```

Emulate with dump:

```bash
./cpu16 emu fact.bin --base 0x0000 --pc 0x0000 --dump 0x0000 0x01FF
```

The dump shows:

- Code region  
- Data region  
- Stack frames  
- Saved return addresses

If you only use the prebuilt `fact.bin` at the repository root, run the same `emu` command with `fact.bin` as the image argument.

---

## Demo videos


| File                              | Content                                                                                                     |
| --------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `demos/Demo.mp4`                  | Software CPU design: CPU16 tool chain, example programs, Fibonacci-oriented walkthrough                     |
| `demos/Demo_Recursion_Memory.mp4` | Program layout & execution: function calls and recursion in C and assembly, memory layout, `emu` / `--dump` |


---

## Reports

Submitted write-ups (PDF) live in `reports/`:

- `Assignment1_Software_CPU_Report.pdf` — Software CPU design (title page, repository URL, download/compile/run, contributions).  
- `Assignment2_Program_Layout_Report.pdf` — Program layout and execution (same document structure; double spacing, page numbers, one-inch margins).

---

## Team members

- Faiq Malik

---

## License

Academic use only — CMPE 220, Spring 2026.