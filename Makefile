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
# 音を作るのは重いので最適化を上げる。-O2 より 6% 速い
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wno-unused-variable -Wno-unused-but-set-variable

# 自分の CPU に合わせるとさらに 4% ほど速いが、他の機械では動かなくなる。
#   make MARCH=native
ifdef MARCH
CXXFLAGS += -march=$(MARCH)
endif
CXXFLAGS += -I src -I src/compat
# ヘッダを直したときに .o を作り直させる
CXXFLAGS += -MMD -MP

# MSYS2 の DLL に依存させない。動的リンクのままだと、MSYS2 の環境の外
# （素の PowerShell など）では起動に失敗して何も言わずに終わる
LDFLAGS ?= -static -static-libgcc -static-libstdc++

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

# vst3 と vst3probe は下で定義している。変数はまだ空なので名前で書く
all: $(BUILD)/verify.exe $(BUILD)/boot.exe $(BUILD)/render.exe \
     $(BUILD)/live.exe $(BUILD)/midisend.exe $(BUILD)/panel.exe $(BUILD)/gui.exe \
     vst3 $(BUILD)/vst3probe.exe

$(BUILD)/verify.exe: $(OBJS) $(BUILD)/src/verify.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/boot.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/boot.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/render.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/render.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# panel はフロントパネル（LCD とボタン）を文字だけで動かす
$(BUILD)/panel.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/panel.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# gui は実機のフロントパネル風の画面を出す
UI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp \
           src/ui/audio_out.cpp src/ui/midi_in.cpp src/ui/midi_out.cpp
UI_OBJS := $(UI_SRCS:%.cpp=$(BUILD)/%.o)

$(BUILD)/gui.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(UI_OBJS) $(BUILD)/src/gui.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32

# midisend は MIDI ファイルを実時間で MIDI 出力へ流す（live の試験用）
$(BUILD)/midisend.exe: $(BUILD)/src/smf.o $(BUILD)/src/midisend.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32

# live は Windows の MIDI 入力と音声出力を使う
$(BUILD)/live.exe: $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/ui/midi_in.o $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32

# ---- VST3 プラグイン
#
# Steinberg の SDK は使わず、インターフェース定義（MIT）だけを取り込んである。
# third_party/vst3/README.md を見よ。
#
#   make vst3      build/S-MU2000.vst3/ にバンドルを作る
#   make install-vst3   それを VST3 の置き場へ複製する

VST3_DIR  := $(BUILD)/S-MU2000.vst3
VST3_BIN  := $(VST3_DIR)/Contents/x86_64-win/S-MU2000.vst3
VST3_INC  := -I third_party/vst3

VST3_SDK_SRCS := 	third_party/vst3/pluginterfaces/base/funknown.cpp 	third_party/vst3/pluginterfaces/base/coreiids.cpp 	third_party/vst3/pluginterfaces/base/conststringtable.cpp 	third_party/vst3/pluginterfaces/base/ustring.cpp

VST3_SRCS := src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp \
             src/vst3/view.cpp src/ui/panel.cpp src/ui/editor.cpp \
             src/ui/effects.cpp $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) -c -o $@ $<

vst3: $(VST3_BIN)

$(VST3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VST3_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32
	@mkdir -p $(VST3_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(VST3_DIR)/Contents/Resources/README.txt 2>/dev/null || true

# 既定の置き場へ入れる。管理者権限が要ることがある
VST3_INSTALL ?= $(PROGRAMFILES)/Common Files/VST3

install-vst3: $(VST3_BIN)
	rm -rf "$(VST3_INSTALL)/S-MU2000.vst3"
	cp -r $(VST3_DIR) "$(VST3_INSTALL)/"
	@echo "入れた: $(VST3_INSTALL)/S-MU2000.vst3"

# 工場が名乗るかどうかだけを確かめる小さな道具
$(BUILD)/vst3probe.exe: $(BUILD)/vst3obj/src/vst3/probe.o $(BUILD)/vst3obj/src/vst3/iids.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/funknown.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/coreiids.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/conststringtable.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/ustring.o                         $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lole32

probe: $(BUILD)/vst3probe.exe $(VST3_BIN)
	$(BUILD)/vst3probe.exe $(VST3_BIN)

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

-include $(VST3_OBJS:.o=.d) $(UI_OBJS:.o=.d) $(BUILD)/src/gui.d $(OBJS:.o=.d) $(BUILD)/src/verify.d $(BUILD)/src/mu2000.d $(BUILD)/src/boot.d $(BUILD)/src/render.d $(BUILD)/src/live.d $(BUILD)/src/panel.d $(BUILD)/src/smf.d $(BUILD)/src/midisend.d

.PHONY: all clean regen vst3 install-vst3 probe
