// Probe the MU2000 firmware for the real FX-parameter SysEx addresses.
#include "mu2000.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr u32 RATE = 44100;
struct rig {
    mu2000 mu; u64 samples = 0;
    void send(const std::vector<u8>& m){ for (u8 b : m) mu.midi_in(b, 0); }
    std::vector<u8> pump(u32 ms){
        std::vector<u8> out; s32 l, r; u64 until = samples + u64(ms)*RATE/1000;
        for (; samples < until; samples++){ mu.run_sample(l,r); u8 b; while (mu.midi_out_take(b)) out.push_back(b); }
        return out;
    }
    std::vector<u8> dump(u8 ah,u8 am,u8 al){ send({0xF0,0x43,0x20,0x4C,ah,am,al,0xF7}); return pump(400); }
    void w1(u8 ah,u8 am,u8 al,u8 d){ send({0xF0,0x43,0x10,0x4C,ah,am,al,d,0xF7}); pump(40); }
    void w2(u8 ah,u8 am,u8 al,u8 d1,u8 d2){ send({0xF0,0x43,0x10,0x4C,ah,am,al,d1,d2,0xF7}); pump(40); }
};
std::vector<u8> payload(const std::vector<u8>& v){
    if (v.size() < 11) return {};
    return std::vector<u8>(v.begin()+9, v.end()-2);
}
int firstdiff(const std::vector<u8>&a,const std::vector<u8>&b){
    for (size_t i=0;i<a.size()&&i<b.size();i++) if (a[i]!=b[i]) return (int)i; return -1; }
}

int main(int argc, char** argv){
    std::string dir = argv[1];
    rig g;
    if (!g.mu.load_program(dir+"/mu2000_flash.bin") || !g.mu.load_wave(dir+"/dump")){
        std::fprintf(stderr,"%s\n", g.mu.error().c_str()); return 2; }
    g.mu.load_sintab(dir+"/standin/sin-table.bin");
    g.mu.reset();
    s32 l,r; for (; g.samples < 30*RATE && !g.mu.midi_ready(); g.samples++) g.mu.run_sample(l,r);
    std::printf("boot %.2fs\n", double(g.samples)/RATE);
    g.pump(500);

    // ---- VARIATION: 2-byte atomic param writes ----
    std::printf("\n== VARIATION param map (2-byte atomic write, marker MSB=0 LSB=0x55) ==\n");
    g.w1(0x02,0x01,0x5A,0x00); g.w1(0x02,0x01,0x5B,0x00);   // insertion, part 1
    g.w2(0x02,0x01,0x40,0x46,0x00);                        // type = TREMOLO
    auto vbase = payload(g.dump(0x02,0x01,0x40));
    for (int P=1; P<=10; P++){
        u8 al = 0x42 + 2*(P-1);
        g.w2(0x02,0x01,al,0x00,0x55);                      // 2-byte: MSB=0, LSB=0x55
        auto d = payload(g.dump(0x02,0x01,0x40));
        int at = firstdiff(d,vbase);
        std::printf("  P%-2d  write 02 01 %02X (2-byte) -> %s (offset %d)\n",
                    P, al, at>=0?"STORED":"ignored", at);
        g.w2(0x02,0x01,0x40,0x46,0x00); vbase = payload(g.dump(0x02,0x01,0x40));
    }
    // P11-16 (1-byte at 70..75 per muxg2k)
    for (int P=11; P<=16; P++){
        u8 al = 0x70 + (P-11);
        g.w1(0x02,0x01,al,0x55);
        auto d = payload(g.dump(0x02,0x01,0x40));
        int at = firstdiff(d,vbase);
        std::printf("  P%-2d  write 02 01 %02X (1-byte) -> %s (offset %d)\n",
                    P, al, at>=0?"STORED":"ignored", at);
        g.w2(0x02,0x01,0x40,0x46,0x00); vbase = payload(g.dump(0x02,0x01,0x40));
    }

    // ---- INSERTION 1: confirm P1-10 map + P11-16 hunt ----
    std::printf("\n== INSERTION 1 param map ==\n");
    g.w1(0x03,0x00,0x0C,0x00); g.w2(0x03,0x00,0x00,0x46,0x00);
    auto ibase = payload(g.dump(0x03,0x00,0x00));
    for (int P=1; P<=10; P++){
        u8 al = 0x01 + P;
        g.w1(0x03,0x00,al,0x55);
        auto d = payload(g.dump(0x03,0x00,0x00));
        int at = firstdiff(d,ibase);
        std::printf("  P%-2d  write 03 00 %02X (1-byte) -> %s (offset %d)\n",
                    P, al, at>=0?"STORED":"ignored", at);
        g.w2(0x03,0x00,0x00,0x46,0x00); ibase = payload(g.dump(0x03,0x00,0x00));
    }
    // hunt P11-16 across 0x0C..0x3F
    std::printf("  -- insertion P11-16 hunt (0x0D..0x30) --\n");
    for (u8 al=0x0D; al<=0x30; al++){
        g.w1(0x03,0x00,al,0x55);
        auto d = payload(g.dump(0x03,0x00,0x00));
        int at = firstdiff(d,ibase);
        if (at>=0) std::printf("    03 00 %02X stored at offset %d\n", al, at);
        g.w2(0x03,0x00,0x00,0x46,0x00); ibase = payload(g.dump(0x03,0x00,0x00));
    }
    return 0;
}
