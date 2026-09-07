# S-MU2000
#
#   make          verify / boot / render / live を作る
#                 render は MIDI ファイルを WAV に書き出す
#                 live   は MIDI 入力を受けてその場で鳴らす
#   make clean    消す
#
# MSYS2 / MinGW-w64 の g++ を想定している。
# C++20 が要る（sh.cpp が std::rotl / std::rotr を使う）。

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wno-unused-variable -Wno-unused-but-set-variable
CXXFLAGS += -I src -I src/compat
# ヘッダを直したときに .o を作り直させる
CXXFLAGS += -MMD -MP

BUILD := build

SRCS := \
	src/compat/compat.cpp \
	src/mame/sound/swp30.cpp \
	src/mame/video/hd44780.cpp \
	src/mame/machine/sci4.cpp \
	src/mame/cpu/sh.cpp \
	src/mame/cpu/sh2.cpp \
	src/mame/cpu/sh7042.cpp \
	src/mame/cpu/sh_adc.cpp \
	src/mame/cpu/sh_bsc.cpp \
	src/mame/cpu/sh_cmt.cpp \
	src/mame/cpu/sh_dmac.cpp \
	src/mame/cpu/sh_intc.cpp \
	src/mame/cpu/sh_mtu.cpp \
	src/mame/cpu/sh_port.cpp \
	src/mame/cpu/sh_sci.cpp

OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)

all: $(BUILD)/verify.exe $(BUILD)/boot.exe $(BUILD)/render.exe \
     $(BUILD)/live.exe $(BUILD)/midisend.exe

$(BUILD)/verify.exe: $(OBJS) $(BUILD)/src/verify.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/boot.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/boot.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/render.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/render.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

# midisend は MIDI ファイルを実時間で MIDI 出力へ流す（live の試験用）
$(BUILD)/midisend.exe: $(BUILD)/src/smf.o $(BUILD)/src/midisend.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lwinmm

# live は Windows の MIDI 入力と音声出力を使う
$(BUILD)/live.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lwinmm

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 内蔵周辺のレジスタ振り分けは MAME の map() から起こす。
# MAME のソースの場所は MAME_SH7042 で渡す
MAME_SH7042 ?= ../MU2000/mame-src/src/devices/cpu/sh/sh7042.cpp

regen:
	python tools/gen_sh7042_map.py $(MAME_SH7042)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(BUILD)/src/verify.d $(BUILD)/src/mu2000.d $(BUILD)/src/boot.d $(BUILD)/src/render.d $(BUILD)/src/live.d $(BUILD)/src/smf.d $(BUILD)/src/midisend.d

.PHONY: all clean regen
