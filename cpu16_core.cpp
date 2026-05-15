// cpu16_core.cpp
// A simple 16-bit software CPU with ISA, emulator, assembler, and example programs.

#include <iostream>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <iterator>

using namespace std;

// ------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------
// Note: Helper functions removed as they were unused
// static inline uint16_t u16(int v){ return (uint16_t)(v & 0xFFFF); }
// static inline int16_t  s16(uint16_t v){ return (int16_t)v; }
// static inline uint8_t  u8 (int v){ return (uint8_t)(v & 0xFF); }

// ------------------------------------------------------------
// RAM image with memory-mapped peripherals (UART + timer)
// ------------------------------------------------------------
struct MmioBackedRam {
    static constexpr size_t SIZE = 65536;
    array<uint8_t, SIZE> physicalRam{};
    uint16_t timer = 0;
    uint16_t timercmp = 0;
    bool irq_pending = false;

    uint8_t mapMmioRead(uint16_t addr){
        switch(addr){
            case 0xFF00: // UART_OUT read (unused)
                return 0;
            case 0xFF01: // UART_IN hi byte (we just simulate "no data": 0xFFFF)
                return 0xFF;
            case 0xFF10: // TIMER low
                return (uint8_t)(timer & 0xFF);
            case 0xFF11: // TIMER high
                return (uint8_t)((timer >> 8) & 0xFF);
            case 0xFF12: // TIMERCMP low
                return (uint8_t)(timercmp & 0xFF);
            case 0xFF13: // TIMERCMP high
                return (uint8_t)((timercmp >> 8) & 0xFF);
            case 0xFF14: // IRQ pending flag
                return irq_pending ? 1 : 0;
            default:
                return 0;
        }
    }

    void mapMmioWrite(uint16_t addr, uint8_t val){
        switch(addr){
            case 0xFF00: { // UART_OUT
                char ch = (char)val;
                cout << ch << flush;
                break;
            }
            case 0xFF10: // TIMER low
                timer = (uint16_t)((timer & 0xFF00) | val);
                break;
            case 0xFF11: // TIMER high
                timer = (uint16_t)((timer & 0x00FF) | (val << 8));
                break;
            case 0xFF12: // TIMERCMP low
                timercmp = (uint16_t)((timercmp & 0xFF00) | val);
                break;
            case 0xFF13: // TIMERCMP high
                timercmp = (uint16_t)((timercmp & 0x00FF) | (val << 8));
                break;
            case 0xFF14: // IRQ_ACK
                if (val == 1) irq_pending = false;
                break;
            default:
                // ignore unknown MMIO writes
                break;
        }
    }

    uint8_t loadByte(uint16_t addr){
        if (addr >= 0xFF00) return mapMmioRead(addr);
        return physicalRam[addr];
    }

    uint16_t loadLeWord(uint16_t addr){
        uint16_t lo = loadByte(addr);
        uint16_t hi = loadByte(addr + 1);
        return (uint16_t)((hi << 8) | lo);
    }

    void storeByte(uint16_t addr, uint8_t val){
        if (addr >= 0xFF00) { mapMmioWrite(addr, val); return; }
        physicalRam[addr] = val;
    }

    void storeLeWord(uint16_t addr, uint16_t val){
        storeByte(addr, (uint8_t)(val & 0xFF));
        storeByte(addr + 1, (uint8_t)((val >> 8) & 0xFF));
    }

    void bumpTimerAfterInstruction(){
        timer = (uint16_t)(timer + 1);
        // Set IRQ when timer reaches or exceeds compare value
        // Note: timercmp of 0 means "never trigger" (timer starts at 0, so 0 means disabled)
        if (timercmp > 0 && timer >= timercmp) {
            irq_pending = true;
        }
    }
};

// ------------------------------------------------------------
// Cpu16_core CPU core (decode / execute)
// ------------------------------------------------------------
struct Cpu16_coreCpu {
    MmioBackedRam &ramHost;
    uint16_t gpr[8]{}; // r0..r7; r7 is stack pointer
    uint16_t programCounter = 0;
    bool flagZ = false, flagN = false, flagC = false, flagV = false;
    bool halted = false;

    Cpu16_coreCpu(MmioBackedRam &ram) : ramHost(ram) {
        gpr[7] = 0x7FFC; // initial stack top
    }

    uint16_t fetchInstructionWord(){
        uint16_t w = ramHost.loadLeWord(programCounter);
        programCounter = (uint16_t)(programCounter + 2);
        return w;
    }

    void assignZeroNegativeFrom(uint16_t resultBits){
        flagZ = (resultBits == 0);
        flagN = ((resultBits & 0x8000) != 0);
    }

    void aluAddWithFlags(uint16_t lhs, uint16_t rhs, uint16_t &out){
        uint32_t wide = (uint32_t)lhs + (uint32_t)rhs;
        out = (uint16_t)wide;
        flagC = (wide >> 16) & 1;
        flagV = ((~(lhs ^ rhs) & (lhs ^ out)) >> 15) & 1;
        assignZeroNegativeFrom(out);
    }

    void aluSubWithFlags(uint16_t lhs, uint16_t rhs, uint16_t &out){
        uint32_t wide = (uint32_t)lhs + (uint32_t)(~rhs) + 1;
        out = (uint16_t)wide;
        flagC = (wide >> 16) & 1; // carry = !borrow
        flagV = (((lhs ^ rhs) & (lhs ^ out)) >> 15) & 1;
        assignZeroNegativeFrom(out);
    }

    void stackPushWord(uint16_t value){
        gpr[7] = (uint16_t)(gpr[7] - 2);
        ramHost.storeLeWord(gpr[7], value);
    }

    uint16_t stackPopWord(){
        uint16_t value = ramHost.loadLeWord(gpr[7]);
        gpr[7] = (uint16_t)(gpr[7] + 2);
        return value;
    }

    void decodeExecute(){
        if (halted) return;

        uint16_t instrBits = fetchInstructionWord();
        uint8_t primaryOpcode = (instrBits >> 11) & 0x1F;
        uint8_t destRegIdx     = (instrBits >> 8)  & 0x07;
        uint8_t srcRegIdx    = (instrBits >> 5)  & 0x07;
        uint8_t imm3Field   = instrBits & 0x07;
        uint8_t imm8Field   = instrBits & 0xFF;
        int8_t  signedImm8  = (int8_t)imm8Field;

        auto fetchImmediateWord = [&](uint16_t &outWord){ outWord = fetchInstructionWord(); };

        switch (primaryOpcode){
            case 0x00: // NOP
                break;
            case 0x01: // HALT
                halted = true;
                break;

            case 0x02: { // LDI rd, imm16
                uint16_t immWord{};
                fetchImmediateWord(immWord);
                gpr[destRegIdx] = immWord;
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x03: { // MOV rd, rs1
                gpr[destRegIdx] = gpr[srcRegIdx];
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x04: { // ADD rd, rs1
                uint16_t sum{};
                aluAddWithFlags(gpr[destRegIdx], gpr[srcRegIdx], sum);
                gpr[destRegIdx] = sum;
                break;
            }

            case 0x05: { // SUB rd, rs1
                uint16_t diff{};
                aluSubWithFlags(gpr[destRegIdx], gpr[srcRegIdx], diff);
                gpr[destRegIdx] = diff;
                break;
            }

            case 0x06: { // AND rd, rs1
                gpr[destRegIdx] &= gpr[srcRegIdx];
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x07: { // OR rd, rs1
                gpr[destRegIdx] |= gpr[srcRegIdx];
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x08: { // XOR rd, rs1
                gpr[destRegIdx] ^= gpr[srcRegIdx];
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x09: { // NOT rd
                gpr[destRegIdx] = ~gpr[destRegIdx];
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x0A: { // SHL rd, imm3
                uint8_t shiftAmt = imm3Field & 7;
                if (shiftAmt){
                    flagC = (gpr[destRegIdx] >> (16 - shiftAmt)) & 1;
                    gpr[destRegIdx] <<= shiftAmt;
                } else {
                    flagC = 0;
                }
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagV = 0;
                break;
            }

            case 0x0B: { // SHR rd, imm3 (logical)
                uint8_t shiftAmt = imm3Field & 7;
                if (shiftAmt){
                    flagC = (gpr[destRegIdx] >> (shiftAmt - 1)) & 1;
                    gpr[destRegIdx] >>= shiftAmt;
                } else {
                    flagC = 0;
                }
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagV = 0;
                break;
            }

            case 0x0C: { // ADDI rd, imm8 (sign-extended)
                uint16_t sum{};
                aluAddWithFlags(gpr[destRegIdx], (uint16_t)(int16_t)signedImm8, sum);
                gpr[destRegIdx] = sum;
                break;
            }

            case 0x0D: { // CMPI rd, imm8
                uint16_t dummy{};
                aluSubWithFlags(gpr[destRegIdx], (uint16_t)(int16_t)signedImm8, dummy);
                break;
            }

            case 0x0E: { // CMP rd, rs1
                uint16_t dummy{};
                aluSubWithFlags(gpr[destRegIdx], gpr[srcRegIdx], dummy);
                break;
            }

            case 0x0F: { // LD rd, [addr16]
                uint16_t addr{};
                fetchImmediateWord(addr);
                gpr[destRegIdx] = ramHost.loadLeWord(addr);
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x10: { // ST rs1, [addr16]
                uint16_t addr{};
                fetchImmediateWord(addr);
                ramHost.storeLeWord(addr, gpr[srcRegIdx]);
                break;
            }

            case 0x11: { // LDB rd, [addr16]
                uint16_t addr{};
                fetchImmediateWord(addr);
                gpr[destRegIdx] = ramHost.loadByte(addr);
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x12: { // STB rs1, [addr16]
                uint16_t addr{};
                fetchImmediateWord(addr);
                ramHost.storeByte(addr, (uint8_t)(gpr[srcRegIdx] & 0xFF));
                break;
            }

            case 0x13: { // LD rd, [rb+imm5]
                int8_t disp5 = (int8_t)(instrBits & 0x1F);
                if (disp5 & 0x10) disp5 |= ~0x1F; // sign extend 5-bit
                uint16_t addr = (uint16_t)(gpr[srcRegIdx] + (int16_t)disp5);
                gpr[destRegIdx] = ramHost.loadLeWord(addr);
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x14: { // ST rs1, [rb+imm5] (rb=rd)
                int8_t disp5 = (int8_t)(instrBits & 0x1F);
                if (disp5 & 0x10) disp5 |= ~0x1F;
                uint16_t addr = (uint16_t)(gpr[destRegIdx] + (int16_t)disp5);
                ramHost.storeLeWord(addr, gpr[srcRegIdx]);
                break;
            }

            case 0x15: { // JMP addr16
                uint16_t target{};
                fetchImmediateWord(target);
                programCounter = target;
                break;
            }

            case 0x16: { // JZ addr16
                uint16_t target{};
                fetchImmediateWord(target);
                if (flagZ) programCounter = target;
                break;
            }

            case 0x17: { // JNZ addr16
                uint16_t target{};
                fetchImmediateWord(target);
                if (!flagZ) programCounter = target;
                break;
            }

            case 0x18: { // JC addr16
                uint16_t target{};
                fetchImmediateWord(target);
                if (flagC) programCounter = target;
                break;
            }

            case 0x19: { // JN addr16
                uint16_t target{};
                fetchImmediateWord(target);
                if (flagN) programCounter = target;
                break;
            }

            case 0x1A: { // CALL addr16
                uint16_t target{};
                fetchImmediateWord(target);
                stackPushWord(programCounter);
                programCounter = target;
                break;
            }

            case 0x1B: { // RET
                programCounter = stackPopWord();
                break;
            }

            case 0x1C: { // IN rd, [io_addr]
                uint16_t ioAddr{};
                fetchImmediateWord(ioAddr);
                // For MMIO addresses, read as byte and zero-extend
                if (ioAddr >= 0xFF00) {
                    gpr[destRegIdx] = ramHost.loadByte(ioAddr);
                } else {
                    gpr[destRegIdx] = ramHost.loadLeWord(ioAddr);
                }
                assignZeroNegativeFrom(gpr[destRegIdx]);
                flagC = flagV = 0;
                break;
            }

            case 0x1D: { // OUT rs1, [io_addr]
                uint16_t ioAddr{};
                fetchImmediateWord(ioAddr);
                // For MMIO addresses, write as byte (UART expects byte)
                if (ioAddr >= 0xFF00) {
                    ramHost.storeByte(ioAddr, (uint8_t)(gpr[srcRegIdx] & 0xFF));
                } else {
                    ramHost.storeLeWord(ioAddr, gpr[srcRegIdx]);
                }
                break;
            }

            default:
                cerr << "Unknown opcode: " << (int)primaryOpcode
                     << " at PC=0x" << hex << (programCounter - 2) << dec << "\n";
                halted = true;
                break;
        }

        ramHost.bumpTimerAfterInstruction();
    }
};

// ------------------------------------------------------------
// Two-pass assembler for Cpu16_core mnemonics
// ------------------------------------------------------------
struct Cpu16_coreAssembler {
    unordered_map<string,uint16_t> symbolAddresses;
    vector<uint8_t> objectBytes;
    vector<pair<int,string>> relocationSlots;
    uint16_t imageOrigin = 0x0000;
    vector<string> sourceLines;

    static string asciiLower(const string& s){
        string r = s;
        for (auto &c : r) c = (char)tolower((unsigned char)c);
        return r;
    }

    static bool isInlineWhitespace(char c){
        return c==' ' || c=='\t' || c=='\r' || c=='\n';
    }

    static string stripEdges(const string& s){
        size_t a = 0, b = s.size();
        while (a < b && isInlineWhitespace(s[a])) a++;
        while (b > a && isInlineWhitespace(s[b-1])) b--;
        return s.substr(a, b - a);
    }

    static vector<string> splitTopLevelOperands(const string& s){
        vector<string> out;
        string cur;
        int par = 0;
        bool inStr = false;
        for(char c : s){
            if (c == '"') inStr = !inStr;
            if (!inStr && c == '[') par++;
            if (!inStr && c == ']') par--;
            if (!inStr && par == 0 && c == ','){
                out.push_back(stripEdges(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(stripEdges(cur));
        return out;
    }

    static bool parseRegisterNumber(const string& s, int &r){
        string t = asciiLower(s);
        if (t.size() < 2 || t[0] != 'r') return false;
        char *end = nullptr;
        long v = strtol(t.c_str() + 1, &end, 10);
        if (*end != 0) return false;
        if (v < 0 || v > 7) return false;
        r = (int)v;
        return true;
    }

    static bool parseNumericImmediate(const string& s, int &v){
        string t = s;
        if (!t.empty() && t[0] == '#') t = t.substr(1);

        // char literal: 'A' or '\n'
        if (t.size() >= 3 && t.front()=='\'' && t.back()=='\''){
            if (t.size() == 3){
                v = (unsigned char)t[1];
                return true;
            }
            if (t.size() == 4 && t[1] == '\\'){
                if (t[2]=='n') { v = '\n'; return true; }
                if (t[2]=='t') { v = '\t'; return true; }
                if (t[2]=='0') { v = '\0'; return true; }
                v = (unsigned char)t[2];
                return true;
            }
        }

        int base = 10;
        const char *c = t.c_str();
        if (t.size() > 2 && t[0]=='0' && (t[1]=='x' || t[1]=='X')) base = 16;
        char *end = nullptr;
        long L = strtol(c, &end, base);
        if (*end == 0){ v = (int)L; return true; }
        return false;
    }

    void ingestSource(const string& text){
        sourceLines.clear();
        symbolAddresses.clear();
        objectBytes.clear();
        relocationSlots.clear();
        imageOrigin = 0;

        string cur;
        stringstream ss(text);
        while (getline(ss, cur)){
            sourceLines.push_back(cur);
        }
    }

    void emitRawByte(uint8_t b){ objectBytes.push_back(b); }
    void emitLittleEndianWord(uint16_t w){
        emitRawByte((uint8_t)(w & 0xFF));
        emitRawByte((uint8_t)((w >> 8) & 0xFF));
    }

    void emitForwardReferenceWord(const string& name){
        relocationSlots.push_back({(int)objectBytes.size(), name});
        emitLittleEndianWord(0);
    }

    uint16_t decodeRequiredRegister(const string& tok){
        int r;
        if (!parseRegisterNumber(tok, r)) throw runtime_error("bad register: " + tok);
        return (uint16_t)r;
    }

    // [0x1234] or [label]
    static bool decodeBracketAddress(const string& tok, uint16_t &addr, string &symbolOrEmpty){
        string t = stripEdges(tok);
        if (t.size() < 3 || t.front() != '[' || t.back() != ']') return false;
        string inner = stripEdges(t.substr(1, t.size()-2));
        int v;
        if (parseNumericImmediate(inner, v)){
            addr = (uint16_t)v;
            symbolOrEmpty.clear();
            return true;
        }
        symbolOrEmpty = asciiLower(inner);
        return true;
    }

    // ----------------- Pass 1: build symbol table & size -----------------
    void layoutLabelsAndSizes(){
        uint16_t pc = imageOrigin;

        for (auto raw : sourceLines){

            // strip comments
            string s = raw;
            size_t sc = s.find(';');
            if (sc != string::npos) s = s.substr(0, sc);
            s = stripEdges(s);
            if (s.empty()) continue;

            // pure label
            if (s.back() == ':'){
                string lab = stripEdges(s.substr(0, s.size()-1));
                symbolAddresses[asciiLower(lab)] = pc;
                continue;
            }

            // label + rest
            size_t colon = s.find(':');
            if (colon != string::npos){
                string lab = stripEdges(s.substr(0, colon));
                symbolAddresses[asciiLower(lab)] = pc;
                s = stripEdges(s.substr(colon+1));
                if (s.empty()) continue;
            }

            string low = asciiLower(s);

            // directives
            if (low.rfind(".org", 0) == 0){
                int v;
                if (!parseNumericImmediate(stripEdges(s.substr(4)), v))
                    throw runtime_error(".org expects value");
                pc = (uint16_t)v;
                continue;
            }

            if (low.rfind(".word", 0) == 0){
                string rest = stripEdges(s.substr(5));
                auto parts = splitTopLevelOperands(rest);
                for (size_t i = 0; i < parts.size(); ++i){
                    pc += 2;
                }
                continue;
            }

            if (low.rfind(".stringz", 0) == 0){
                string rest = stripEdges(s.substr(8));
                if (rest.empty() || rest[0] != '"')
                    throw runtime_error(".stringz expects string");
                string body;
                bool esc = false;
                for (size_t i = 1; i < rest.size(); ++i){
                    char c = rest[i];
                    if (esc){
                        if (c=='n') body.push_back('\n');
                        else if (c=='t') body.push_back('\t');
                        else if (c=='0') body.push_back('\0');
                        else body.push_back(c);
                        esc = false;
                    } else {
                        if (c=='\\') esc = true;
                        else if (c=='"') break;
                        else body.push_back(c);
                    }
                }
                pc = (uint16_t)(pc + (uint16_t)(body.size() + 1));
                continue;
            }

            // instruction size estimation
            string mnemRaw, rest;
            {
                stringstream ts(s);
                ts >> mnemRaw;
                getline(ts, rest);
            }
            string mnem = asciiLower(mnemRaw);
            rest = stripEdges(rest);

            auto needWide = [&](const string& m)->bool{
                static unordered_set<string> w = {
                    "ldi","ldb","stb","jmp","jz","jnz","jc","jn",
                    "call","in","out"
                };
                return w.count(m) > 0;
            };

            if (mnem == "ld" || mnem == "st"){
                // detect short ([rb+imm]) vs absolute ([addr])
                auto parts = splitTopLevelOperands(rest);
                if (parts.size() == 2 && parts[1].find('+') != string::npos){
                    pc = (uint16_t)(pc + 2); // short form: 1 word
                } else {
                    pc = (uint16_t)(pc + 4); // absolute: 2-word
                }
            } else {
                pc = (uint16_t)(pc + 2);
                if (needWide(mnem)) pc = (uint16_t)(pc + 2);
            }
        }
    }

    // ----------------- Pass 2: emit code -----------------
    void assembleToObjectBytes(){
        uint16_t pc = imageOrigin;
        objectBytes.clear();

        for (auto raw : sourceLines){

            // strip comments
            string s = raw;
            size_t sc = s.find(';');
            if (sc != string::npos) s = s.substr(0, sc);
            s = stripEdges(s);
            if (s.empty()) continue;

            if (s.back() == ':') continue; // pure label

            size_t colon = s.find(':');
            if (colon != string::npos){
                s = stripEdges(s.substr(colon + 1));
                if (s.empty()) continue;
            }

            string low = asciiLower(s);

            // directives
            if (low.rfind(".org", 0) == 0){
                int v;
                parseNumericImmediate(stripEdges(s.substr(4)), v);
                pc = (uint16_t)v;
                while (objectBytes.size() < (size_t)(pc - imageOrigin))
                    objectBytes.push_back(0);
                continue;
            }

            if (low.rfind(".word", 0) == 0){
                string rest = stripEdges(s.substr(5));
                auto parts = splitTopLevelOperands(rest);
                for (auto &p : parts){
                    int v;
                    if (parseNumericImmediate(p, v)) emitLittleEndianWord((uint16_t)v);
                    else emitForwardReferenceWord(asciiLower(stripEdges(p)));
                }
                continue;
            }

            if (low.rfind(".stringz", 0) == 0){
                string rest = stripEdges(s.substr(8));
                string body;
                bool esc = false;
                for (size_t i = 1; i < rest.size(); ++i){
                    char c = rest[i];
                    if (esc){
                        if (c=='n') body.push_back('\n');
                        else if (c=='t') body.push_back('\t');
                        else if (c=='0') body.push_back('\0');
                        else body.push_back(c);
                        esc = false;
                    } else {
                        if (c=='\\') esc = true;
                        else if (c=='"') break;
                        else body.push_back(c);
                    }
                }
                for (char c : body) emitRawByte((uint8_t)c);
                emitRawByte(0);
                continue;
            }

            // instructions
            string mnemRaw, rest;
            {
                stringstream ts(s);
                ts >> mnemRaw;
                getline(ts, rest);
            }
            string M = asciiLower(mnemRaw);
            rest = stripEdges(rest);
            auto parts = splitTopLevelOperands(rest);

            auto encodeWordRegRegImm3 = [&](uint8_t op, uint8_t rd, uint8_t rs1, uint8_t imm3){
                uint16_t w = (uint16_t)((op << 11) | (rd << 8) | (rs1 << 5) | (imm3 & 0x07));
                emitLittleEndianWord(w);
            };
            auto encodeWordRegReg = [&](uint8_t op, uint8_t rd, uint8_t rs1){
                uint16_t w = (uint16_t)((op << 11) | (rd << 8) | (rs1 << 5));
                emitLittleEndianWord(w);
            };
            auto encodeWordRegImm8 = [&](uint8_t op, uint8_t rd, int imm8){
                uint16_t w = (uint16_t)((op << 11) | (rd << 8) | ((uint8_t)imm8));
                emitLittleEndianWord(w);
            };
            auto encodeOpcodePlusRegField = [&](uint8_t op, uint8_t rd){
                uint16_t w = (uint16_t)((op << 11) | (rd << 8));
                emitLittleEndianWord(w);
            };

            if (M == "nop"){
                emitLittleEndianWord(0x0000);
                continue;
            }
            if (M == "halt"){
                emitLittleEndianWord((uint16_t)((0x01 << 11)));
                continue;
            }
            if (M == "ldi"){
                if (parts.size() != 2) throw runtime_error("LDI rd, imm16");
                int rd = decodeRequiredRegister(parts[0]);
                encodeOpcodePlusRegField(0x02, (uint8_t)rd);
                int v;
                if (parseNumericImmediate(parts[1], v)) emitLittleEndianWord((uint16_t)v);
                else emitForwardReferenceWord(asciiLower(parts[1]));
                continue;
            }
            if (M == "mov"){
                if (parts.size() != 2) throw runtime_error("MOV rd, rs");
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x03, (uint8_t)rd, (uint8_t)rs);
                continue;
            }
            if (M == "add"){
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x04, (uint8_t)rd, (uint8_t)rs);
                continue;
            }
            if (M == "sub"){
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x05, (uint8_t)rd, (uint8_t)rs);
                continue;
            }
            if (M == "and"){
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x06, (uint8_t)rd, (uint8_t)rs);
                continue;
            }
            if (M == "or"){
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x07, (uint8_t)rd, (uint8_t)rs);
                continue;
            }
            if (M == "xor"){
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x08, (uint8_t)rd, (uint8_t)rs);
                continue;
            }
            if (M == "not"){
                if (parts.size() != 1) throw runtime_error("NOT rd");
                int rd = decodeRequiredRegister(parts[0]);
                encodeWordRegReg(0x09, (uint8_t)rd, 0);
                continue;
            }
            if (M == "shl"){
                int rd = decodeRequiredRegister(parts[0]);
                int s;
                if (!parseNumericImmediate(parts[1], s) || s < 0 || s > 7)
                    throw runtime_error("SHL rd, 0..7");
                encodeWordRegRegImm3(0x0A, (uint8_t)rd, 0, (uint8_t)s);
                continue;
            }
            if (M == "shr"){
                int rd = decodeRequiredRegister(parts[0]);
                int s;
                if (!parseNumericImmediate(parts[1], s) || s < 0 || s > 7)
                    throw runtime_error("SHR rd, 0..7");
                encodeWordRegRegImm3(0x0B, (uint8_t)rd, 0, (uint8_t)s);
                continue;
            }
            if (M == "addi"){
                int rd = decodeRequiredRegister(parts[0]);
                int v;
                if (!parseNumericImmediate(parts[1], v)) throw runtime_error("ADDI rd, imm8");
                encodeWordRegImm8(0x0C, (uint8_t)rd, v);
                continue;
            }
            if (M == "cmpi"){
                int rd = decodeRequiredRegister(parts[0]);
                int v;
                if (!parseNumericImmediate(parts[1], v)) throw runtime_error("CMPI rd, imm8");
                encodeWordRegImm8(0x0D, (uint8_t)rd, v);
                continue;
            }
            if (M == "cmp"){
                int rd = decodeRequiredRegister(parts[0]);
                int rs = decodeRequiredRegister(parts[1]);
                encodeWordRegReg(0x0E, (uint8_t)rd, (uint8_t)rs);
                continue;
            }

            if (M == "ld"){
                if (parts.size() != 2) throw runtime_error("LD rd, [..]");
                int rd = decodeRequiredRegister(parts[0]);
                string addrTok = parts[1];

                if (addrTok.find('+') != string::npos){
                    // short form: LD rd, [rb+imm5]
                    // encode: op=0x13, bits 15..11 op, 10..8 rd, 7..5 rb, 4..0 imm5
                    string inside = stripEdges(addrTok.substr(1, addrTok.size()-2)); // rb+imm
                    auto plus = inside.find('+');
                    if (plus == string::npos) throw runtime_error("LD short expects [rb+imm]");
                    string rbS = stripEdges(inside.substr(0, plus));
                    string immS = stripEdges(inside.substr(plus + 1));
                    int rb = decodeRequiredRegister(rbS);
                    int v;
                    if (!parseNumericImmediate(immS, v)) throw runtime_error("LD short imm must be int");
                    int imm5 = v & 0x1F;
                    uint16_t w = (uint16_t)((0x13 << 11) | (rd << 8) | (rb << 5) | imm5);
                    emitLittleEndianWord(w);
                } else {
                    // absolute: LD rd, [addr16]
                    encodeOpcodePlusRegField(0x0F, (uint8_t)rd);
                    uint16_t addr;
                    string labelReference;
                    if (decodeBracketAddress(addrTok, addr, labelReference)){
                        if (labelReference.empty()) emitLittleEndianWord(addr);
                        else emitForwardReferenceWord(labelReference);
                    } else {
                        throw runtime_error("LD rd, [addr16]");
                    }
                }
                continue;
            }

            if (M == "st"){
                if (parts.size() != 2) throw runtime_error("ST rs, [..]");
                int rs = decodeRequiredRegister(parts[0]);
                string addrTok = parts[1];

                if (addrTok.find('+') != string::npos){
                    // short form: ST rs, [rb+imm5], encoded as op=0x14 rd=rb, rs1=rs
                    string inside = stripEdges(addrTok.substr(1, addrTok.size()-2));
                    auto plus = inside.find('+');
                    if (plus == string::npos) throw runtime_error("ST short expects [rb+imm]");
                    string rbS = stripEdges(inside.substr(0, plus));
                    string immS = stripEdges(inside.substr(plus + 1));
                    int rb = decodeRequiredRegister(rbS);
                    int v;
                    if (!parseNumericImmediate(immS, v)) throw runtime_error("ST short imm must be int");
                    int imm5 = v & 0x1F;
                    uint16_t w = (uint16_t)((0x14 << 11) | (rb << 8) | (rs << 5) | imm5);
                    emitLittleEndianWord(w);
                } else {
                    encodeOpcodePlusRegField(0x10, (uint8_t)rs);
                    uint16_t addr;
                    string labelReference;
                    if (decodeBracketAddress(addrTok, addr, labelReference)){
                        if (labelReference.empty()) emitLittleEndianWord(addr);
                        else emitForwardReferenceWord(labelReference);
                    } else {
                        throw runtime_error("ST rs, [addr16]");
                    }
                }
                continue;
            }

            if (M == "ldb"){
                if (parts.size() != 2) throw runtime_error("LDB rd, [addr16]");
                int rd = decodeRequiredRegister(parts[0]);
                encodeOpcodePlusRegField(0x11, (uint8_t)rd);
                uint16_t addr;
                string labelReference;
                if (decodeBracketAddress(parts[1], addr, labelReference)){
                    if (labelReference.empty()) emitLittleEndianWord(addr);
                    else emitForwardReferenceWord(labelReference);
                } else {
                    throw runtime_error("LDB rd, [addr16]");
                }
                continue;
            }

            if (M == "stb"){
                if (parts.size() != 2) throw runtime_error("STB rs, [addr16]");
                int rs = decodeRequiredRegister(parts[0]);
                encodeOpcodePlusRegField(0x12, (uint8_t)rs);
                uint16_t addr;
                string labelReference;
                if (decodeBracketAddress(parts[1], addr, labelReference)){
                    if (labelReference.empty()) emitLittleEndianWord(addr);
                    else emitForwardReferenceWord(labelReference);
                } else {
                    throw runtime_error("STB rs, [addr16]");
                }
                continue;
            }

            if (M == "jmp" || M == "jz" || M == "jnz" || M == "jc" || M == "jn"){
                uint8_t op =
                    (M == "jmp") ? 0x15 :
                    (M == "jz")  ? 0x16 :
                    (M == "jnz") ? 0x17 :
                    (M == "jc")  ? 0x18 : 0x19;
                encodeOpcodePlusRegField(op, 0);
                int v;
                if (parseNumericImmediate(parts[0], v)) emitLittleEndianWord((uint16_t)v);
                else emitForwardReferenceWord(asciiLower(parts[0]));
                continue;
            }

            if (M == "call"){
                encodeOpcodePlusRegField(0x1A, 0);
                int v;
                if (parseNumericImmediate(parts[0], v)) emitLittleEndianWord((uint16_t)v);
                else emitForwardReferenceWord(asciiLower(parts[0]));
                continue;
            }

            if (M == "ret"){
                encodeOpcodePlusRegField(0x1B, 0);
                continue;
            }

            if (M == "in"){
                if (parts.size() != 2) throw runtime_error("IN rd, [addr16]");
                int rd = decodeRequiredRegister(parts[0]);
                encodeOpcodePlusRegField(0x1C, (uint8_t)rd);
                uint16_t addr;
                string labelReference;
                if (decodeBracketAddress(parts[1], addr, labelReference)){
                    if (labelReference.empty()) emitLittleEndianWord(addr);
                    else emitForwardReferenceWord(labelReference);
                } else {
                    throw runtime_error("IN rd, [addr16]");
                }
                continue;
            }

            if (M == "out"){
                if (parts.size() != 2) throw runtime_error("OUT rs, [addr16]");
                int rs = decodeRequiredRegister(parts[0]);
                // OUT format: opcode=0x1D, rd=0 (unused), rs1=rs
                uint16_t w = (uint16_t)((0x1D << 11) | (0 << 8) | (rs << 5));
                emitLittleEndianWord(w);
                uint16_t addr;
                string labelReference;
                if (decodeBracketAddress(parts[1], addr, labelReference)){
                    if (labelReference.empty()) emitLittleEndianWord(addr);
                    else emitForwardReferenceWord(labelReference);
                } else {
                    throw runtime_error("OUT rs, [addr16]");
                }
                continue;
            }

            throw runtime_error(string("Unknown mnemonic: ") + M);
        }

        // resolve forward references
        for (auto &slot : relocationSlots){
            int off = slot.first;
            auto it = symbolAddresses.find(slot.second);
            if (it == symbolAddresses.end())
                throw runtime_error("undefined label: " + slot.second);
            uint16_t a = it->second;
            objectBytes[off]     = (uint8_t)(a & 0xFF);
            objectBytes[off + 1] = (uint8_t)((a >> 8) & 0xFF);
        }
    }
};

// ------------------------------------------------------------
// Example programs
// ------------------------------------------------------------

static const char *kBuiltinSourceHello = R"ASM(
; Minimal Hello, World using UART_OUT at 0xFF00
; No data section, no addressing tricks – just immediates.

.org 0x0000
start:
  ; "Hello, World!\n"
  LDI r0, 72      ; 'H'
  OUT r0, [0xFF00]

  LDI r0, 101     ; 'e'
  OUT r0, [0xFF00]

  LDI r0, 108     ; 'l'
  OUT r0, [0xFF00]

  LDI r0, 108     ; 'l'
  OUT r0, [0xFF00]

  LDI r0, 111     ; 'o'
  OUT r0, [0xFF00]

  LDI r0, 44      ; ','
  OUT r0, [0xFF00]

  LDI r0, 32      ; ' '
  OUT r0, [0xFF00]

  LDI r0, 87      ; 'W'
  OUT r0, [0xFF00]

  LDI r0, 111     ; 'o'
  OUT r0, [0xFF00]

  LDI r0, 114     ; 'r'
  OUT r0, [0xFF00]

  LDI r0, 108     ; 'l'
  OUT r0, [0xFF00]

  LDI r0, 100     ; 'd'
  OUT r0, [0xFF00]

  LDI r0, 33      ; '!'
  OUT r0, [0xFF00]

  LDI r0, 10      ; '\n'
  OUT r0, [0xFF00]

  HALT
)ASM";

static const char *kBuiltinSourceFibonacci = R"ASM(
; Fibonacci: compute first 10 16-bit Fibonacci numbers into memory
; at label 'buf' (you can inspect with --dump).

.org 0x0100
start:
  LDI r0, 0      ; a = 0
  LDI r1, 1      ; b = 1
  LDI r2, 10     ; count
  LDI r3, buf    ; pointer to buffer

loop:
  ST  r0, [r3+0] ; store a
  ADDI r3, #2    ; advance pointer (each word = 2 bytes)

  ; next fib
  MOV r4, r1     ; temp = b
  ADD r1, r0     ; b = a + b
  MOV r0, r4     ; a = old b

  ADDI r2, #-1
  JNZ loop

  HALT

buf:
  .word 0,0,0,0,0,0,0,0,0,0
)ASM";

static const char *kBuiltinSourceTimer = R"ASM(
; Timer demo: demonstrates Fetch/Compute/Store cycles
; 
; This program demonstrates the Fetch/Compute/Store cycle by executing
; a series of instructions. Each instruction follows this cycle:
;
; Fetch/Compute/Store cycle:
; 1. Fetch: CPU fetches instruction from memory at Program Counter (PC)
; 2. Compute: ALU performs the operation (add, compare, load, etc.)
; 3. Store: Result is stored in register or memory
;
; The timer increments automatically after each instruction execution,
; demonstrating how many Fetch/Compute/Store cycles have occurred.

.org 0x0000
start:
  ; === Example 1: LDI (Load Immediate) - Fetch/Compute/Store ===
  ; Fetch: CPU fetches LDI opcode (0x02) from memory at PC
  ;        Then fetches immediate value 'S' (0x53) from next memory location
  ; Compute: ALU loads the immediate value 0x53 into the destination register
  ; Store: Value 0x53 is stored in register r3
  ; Timer increments: +2 (one for opcode fetch, one for immediate fetch)
  LDI r3, 83           ; Load 'S' (ASCII 83) into r3
  
  ; === Example 2: OUT (Output) - Fetch/Compute/Store ===
  ; Fetch: CPU fetches OUT opcode (0x1D) and address 0xFF00 from memory
  ; Compute: ALU gets value from r3 (83), computes MMIO address 0xFF00
  ; Store: Byte 83 is written to UART output register (prints 'S')
  ; Timer increments: +2 (one for opcode, one for address)
  OUT r3, [0xFF00]

  ; === Example 3: Arithmetic operations - Fetch/Compute/Store ===
  ; Demonstrates multiple Fetch/Compute/Store cycles
  LDI r0, 5            ; Fetch: LDI opcode+5, Compute: load 5, Store: to r0
  LDI r1, 3            ; Fetch: LDI opcode+3, Compute: load 3, Store: to r1
  ADD r0, r1           ; Fetch: ADD opcode, Compute: r0+r1=8, Store: to r0

  ; === Example 4: Print "Timer\n" - Multiple Fetch/Compute/Store cycles ===
  ; Each character print demonstrates a complete Fetch/Compute/Store cycle
  LDI r3, 84           ; 'T' - Fetch/Compute/Store
  OUT r3, [0xFF00]     ; Fetch/Compute/Store
  LDI r3, 105          ; 'i' - Fetch/Compute/Store
  OUT r3, [0xFF00]     ; Fetch/Compute/Store
  LDI r3, 109          ; 'm' - Fetch/Compute/Store
  OUT r3, [0xFF00]     ; Fetch/Compute/Store
  LDI r3, 101          ; 'e' - Fetch/Compute/Store
  OUT r3, [0xFF00]     ; Fetch/Compute/Store
  LDI r3, 114          ; 'r' - Fetch/Compute/Store
  OUT r3, [0xFF00]     ; Fetch/Compute/Store
  LDI r3, 10           ; '\n' - Fetch/Compute/Store
  OUT r3, [0xFF00]     ; Fetch/Compute/Store

  HALT
)ASM";

static const char *kBuiltinSourceFactorial = R"ASM(
; Recursive factorial on Cpu16_core:
;   fact(n) = 1 if n <= 1
;             n * fact(n-1) otherwise
;   Input : r0 = n
;   Output: r0 = n!
;   Uses  : r1, r2, r3, r4 (saved on stack)

.org 0x0000

start:
  ; Initialize stack pointer
  LDI r7, 0x7FFC

  ; Compute fact(5)
  LDI r0, 5
  CALL fact            ; r0 = fact(5) = 120

  ; Print the result in hex: "Result: 0x0078\n"
  CALL print_result

  HALT

; --------------------------------------------------
; fact:
;   Input : r0 = n
;   Output: r0 = n!
;   Clobbers: r1,r2,r3,r4 (saved/restored)
; --------------------------------------------------
fact:
  ; Prologue: save r1..r4 on stack

  ; push r1
  ADDI r7, #-2
  ST   r1, [r7+0]

  ; push r2
  ADDI r7, #-2
  ST   r2, [r7+0]

  ; push r3
  ADDI r7, #-2
  ST   r3, [r7+0]

  ; push r4
  ADDI r7, #-2
  ST   r4, [r7+0]

  ; ---- Base case: if (n <= 1) return 1 ----
  CMPI r0, #1         ; compare n with 1 => flags from (n - 1)
  JZ   base_case      ; if n == 1
  JN   base_case      ; if n <  1 (negative)

  ; ---- Recursive case: n > 1 ----
  ; Save n in r1
  MOV  r1, r0         ; r1 = n

  ; Call fact(n-1)
  ADDI r0, #-1        ; r0 = n-1
  CALL fact           ; r0 = fact(n-1)

  ; Now:
  ;   r1 = n
  ;   r0 = fact(n-1)
  ; We need r0 = n * fact(n-1)

  MOV  r2, r0         ; r2 = fact(n-1) (multiplicand)
  MOV  r3, r1         ; r3 = n        (counter)
  LDI  r0, 0          ; r0 = result = 0
  MOV  r4, r2         ; r4 = multiplicand copy

mul_loop:
  CMPI r3, #0         ; while (n > 0)
  JZ   mul_done
  ADD  r0, r4         ; result += multiplicand
  ADDI r3, #-1        ; n--
  JMP  mul_loop

mul_done:
  ; r0 contains n * fact(n-1)
  JMP  epilogue

base_case:
  ; n <= 1 => return 1
  LDI  r0, 1

epilogue:
  ; Restore r4,r3,r2,r1 in reverse (LIFO) order

  ; pop r4
  LD   r4, [r7+0]
  ADDI r7, #2

  ; pop r3
  LD   r3, [r7+0]
  ADDI r7, #2

  ; pop r2
  LD   r2, [r7+0]
  ADDI r7, #2

  ; pop r1
  LD   r1, [r7+0]
  ADDI r7, #2

  RET


; --------------------------------------------------
; print_result:
;   Uses r1,r2,r3,r5 as scratch.
;   Prints: "Result: 0x" + 4 hex digits of r0 + "\n"
; --------------------------------------------------
print_result:
  ; Copy result from r0 into r1
  MOV  r1, r0

  ; Print "Result: 0x"
  ; ASCII: R=82, e=101, s=115, u=117, l=108, t=116, ':'=58, ' '=32, '0'=48, 'x'=120

  LDI r2, 82      ; 'R'
  OUT r2, [0xFF00]
  LDI r2, 101     ; 'e'
  OUT r2, [0xFF00]
  LDI r2, 115     ; 's'
  OUT r2, [0xFF00]
  LDI r2, 117     ; 'u'
  OUT r2, [0xFF00]
  LDI r2, 108     ; 'l'
  OUT r2, [0xFF00]
  LDI r2, 116     ; 't'
  OUT r2, [0xFF00]
  LDI r2, 58      ; ':'
  OUT r2, [0xFF00]
  LDI r2, 32      ; ' '
  OUT r2, [0xFF00]
  LDI r2, 48      ; '0'
  OUT r2, [0xFF00]
  LDI r2, 120     ; 'x'
  OUT r2, [0xFF00]

  ; Now print r1 as 4 hex digits
  ; We'll use r5 as mask (0x000F), r2 as nibble, r3 in print_nibble.

  LDI r5, 0x000F

  ; ---- High nibble (bits 12..15) ----
  MOV r2, r1
  SHR r2, 4
  SHR r2, 4
  SHR r2, 4        ; r2 = r1 >> 12
  AND r2, r5
  CALL print_nibble

  ; ---- Next nibble (bits 8..11) ----
  MOV r2, r1
  SHR r2, 4
  SHR r2, 4        ; r2 = r1 >> 8
  AND r2, r5
  CALL print_nibble

  ; ---- Next nibble (bits 4..7) ----
  MOV r2, r1
  SHR r2, 4        ; r2 = r1 >> 4
  AND r2, r5
  CALL print_nibble

  ; ---- Lowest nibble (bits 0..3) ----
  MOV r2, r1
  AND r2, r5
  CALL print_nibble

  ; Newline
  LDI r2, 10      ; '\n'
  OUT r2, [0xFF00]

  RET


; --------------------------------------------------
; print_nibble:
;   Input : r2 = value 0..15
;   Output: prints one hex digit via UART
;   Uses  : r2, r3
; --------------------------------------------------
print_nibble:
  LDI r3, 10
  CMP r2, r3
  JN  pn_digit       ; if r2 < 10 -> digit

  ; r2 >= 10 -> 'A' + (r2-10)
  LDI r3, 55         ; 'A'(65) - 10 = 55
  ADD r2, r3
  JMP pn_out

pn_digit:
  ; r2 < 10 -> '0' + r2
  LDI r3, 48         ; '0'
  ADD r2, r3

pn_out:
  OUT r2, [0xFF00]
  RET
)ASM";



// in-memory "filesystem" for bundled example sources
unordered_map<string,string> bundledAsmByPath = {
    {"examples/hello.asm", kBuiltinSourceHello},
    {"examples/fib.asm",   kBuiltinSourceFibonacci},
    {"examples/timer.asm", kBuiltinSourceTimer},
    {"examples/fact.asm",  kBuiltinSourceFactorial}
};

string readAsmTextFromFileOrBundle(const string& path){
    auto it = bundledAsmByPath.find(path);
    if (it != bundledAsmByPath.end()) return it->second;

    ifstream f(path);
    if (!f.good()) throw runtime_error("Cannot open file: " + path);
    stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeBinaryFile(const string& path, const vector<uint8_t>& bin){
    ofstream o(path, ios::binary);
    if (!o.good()) throw runtime_error("Cannot write: " + path);
    o.write((const char*)bin.data(), (streamsize)bin.size());
}

void copyProgramBytes(MmioBackedRam &ram, const vector<uint8_t>& bin, uint16_t base){
    for (size_t i = 0; i < bin.size(); ++i)
        ram.storeByte((uint16_t)(base + i), bin[i]);
}

void renderHexDump(MmioBackedRam &ram, uint16_t a0, uint16_t a1){
    for (uint32_t a = a0; a <= a1; a += 16){
        cout << hex << setw(4) << setfill('0') << a << ": ";
        for (int i = 0; i < 16 && a + i <= a1; ++i){
            cout << setw(2) << (int)ram.loadByte((uint16_t)(a + i)) << " ";
        }
        cout << dec << "\n";
    }
}

// ------------------------------------------------------------
// main
// ------------------------------------------------------------
int main(int argc, char** argv){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    if (argc < 2){
        cerr << "Usage: cpu16 <asm|emu|run> <file> [options]\n";
        cerr << "Examples:\n"
             << "  ./cpu16 run examples/hello.asm\n"
             << "  ./cpu16 run examples/timer.asm\n"
             << "  ./cpu16 asm examples/fib.asm -o fib.bin\n"
             << "  ./cpu16 emu fib.bin --base 0x0000 --pc 0x0100 --dump 0x0100 0x01FF\n";
        return 1;
    }

    string mode = argv[1];

    try {
        if (mode == "asm"){
            if (argc < 3) throw runtime_error("asm: missing <file>");
            string in = argv[2];
            string out = "a.bin";
            for (int i = 3; i < argc; ++i){
                string t = argv[i];
                if (t == "-o" && i + 1 < argc){
                    out = argv[++i];
                }
            }
            string text = readAsmTextFromFileOrBundle(in);
            Cpu16_coreAssembler assembler;
            assembler.ingestSource(text);
            assembler.layoutLabelsAndSizes();
            assembler.assembleToObjectBytes();
            writeBinaryFile(out, assembler.objectBytes);
            cout << "Assembled " << in << " -> " << out
                 << " (" << assembler.objectBytes.size() << " bytes)\n";
        } else if (mode == "emu"){
            if (argc < 3) throw runtime_error("emu: missing <image.bin>");
            string img = argv[2];
            uint16_t base = 0x0000, pc = 0x0000;
            bool dump = false;
            uint16_t da = 0, db = 0;

            for (int i = 3; i < argc; ++i){
                string t = argv[i];
                if (t == "--base" && i + 1 < argc){
                    base = (uint16_t)strtoul(argv[++i], nullptr, 0);
                } else if (t == "--pc" && i + 1 < argc){
                    pc = (uint16_t)strtoul(argv[++i], nullptr, 0);
                } else if (t == "--dump" && i + 2 < argc){
                    dump = true;
                    da = (uint16_t)strtoul(argv[++i], nullptr, 0);
                    db = (uint16_t)strtoul(argv[++i], nullptr, 0);
                }
            }

            ifstream f(img, ios::binary);
            if (!f.good()) throw runtime_error("Cannot open image: " + img);
            vector<uint8_t> bin((istreambuf_iterator<char>(f)), {});

            MmioBackedRam ram;
            copyProgramBytes(ram, bin, base);
            Cpu16_coreCpu cpu(ram);
            cpu.programCounter = pc;
            while (!cpu.halted) cpu.decodeExecute();

            if (dump) renderHexDump(ram, da, db);

        } else if (mode == "run"){
            if (argc < 3) throw runtime_error("run: missing <file.asm>");
            string in = argv[2];
            bool doDump = false;
            uint16_t da = 0, db = 0;
            for (int i = 3; i < argc; ++i){
                string t = argv[i];
                if (t == "--dump" && i + 2 < argc){
                    doDump = true;
                    da = (uint16_t)strtoul(argv[++i], nullptr, 0);
                    db = (uint16_t)strtoul(argv[++i], nullptr, 0);
                }
            }

            string text = readAsmTextFromFileOrBundle(in);
            Cpu16_coreAssembler assembler;
            assembler.ingestSource(text);
            assembler.layoutLabelsAndSizes();
            assembler.assembleToObjectBytes();

            MmioBackedRam ram;
            copyProgramBytes(ram, assembler.objectBytes, 0x0000);
            Cpu16_coreCpu cpu(ram);
            cpu.programCounter = 0x0000;
            while (!cpu.halted) cpu.decodeExecute();

            if (doDump) renderHexDump(ram, da, db);
        } else {
            throw runtime_error("unknown mode: " + mode);
        }
    } catch (const exception& e){
        cerr << "Error: " << e.what() << "\n";
        return 2;
    }

    return 0;
}
