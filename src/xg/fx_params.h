// license:BSD-3-Clause
//
// インサーションエフェクトの種類ごとのパラメータ（doc/pc-editor.md の「インサーションの設定の窓」）。
// **tools/fxsweep/make_fx_params.py が作ったもの。手で直さない。**
//
// firmware の LCD の編集画面を 1 ページずつ見て、パラメータの名前、SysEx の番地（03 nn xx）、
// 受け付ける範囲、値ごとの表示を調べた（tools/fxsweep/fxsweep.cpp）。
// パラメータ 1-10 は、2 バイトの値を持つ種類（ディレイ）では 30-43、ほかは 02-0B。11-16 は 20-25。

#ifndef S_MU2000_XG_FX_PARAMS_H
#define S_MU2000_XG_FX_PARAMS_H

#pragma once

#include "compat/mamecompat.h"

namespace xg {

enum class fx_fmt : u8 {
	raw,        // 数のまま
	table,      // texts[値 - lo]
	tenths,     // 10 分の 1（ミリ秒など）
};

struct fx_param {
	u8 addr;             // 03 nn の後ろの番地
	u8 size;             // バイト数（7bit ずつ）
	u16 lo, hi;          // firmware が受け付ける範囲
	const char *label;   // LCD の名前
	fx_fmt fmt;
	const char *const *texts;
};

struct fx_def {
	u8 msb;              // 種類の MSB。LSB 違い（HALL 2 など）も同じ並び
	const fx_param *params;
	int count;
};

namespace fx_text {
inline constexpr const char *const T0[] = {
	"0.3", "0.4", "0.5", "0.6", "0.7", "0.8", "0.9", "1.0",
	"1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8",
	"1.9", "2.0", "2.1", "2.2", "2.3", "2.4", "2.5", "2.6",
	"2.7", "2.8", "2.9", "3.0", "3.1", "3.2", "3.3", "3.4",
	"3.5", "3.6", "3.7", "3.8", "3.9", "4.0", "4.1", "4.2",
	"4.3", "4.4", "4.5", "4.6", "4.7", "4.8", "4.9", "5.0",
	"5.5", "6.0", "6.5", "7.0", "7.5", "8.0", "8.5", "9.0",
	"9.5", "10.0", "11.0", "12.0", "13.0", "14.0", "15.0", "16.0",
	"17.0", "18.0", "19.0", "20.0", "25.0", "30.0",
};
inline constexpr const char *const T1[] = {
	"0.1", "1.7", "3.2", "4.8", "6.4", "8.0", "9.5", "11.1",
	"12.7", "14.3", "15.8", "17.4", "19.0", "20.6", "22.1", "23.7",
	"25.3", "26.9", "28.4", "30.0", "31.6", "33.2", "34.7", "36.3",
	"37.9", "39.5", "41.0", "42.6", "44.2", "45.7", "47.3", "48.9",
	"50.5", "52.0", "53.6", "55.2", "56.8", "58.3", "59.9", "61.5",
	"63.1", "64.6", "66.2", "67.8", "69.4", "70.9", "72.5", "74.1",
	"75.7", "77.2", "78.8", "80.4", "81.9", "83.5", "85.1", "86.7",
	"88.2", "89.8", "91.4", "93.0", "94.5", "96.1", "97.7", "99.3",
};
inline constexpr const char *const T2[] = {
	"Thru", "22", "25", "28", "32", "36", "40", "45",
	"50", "56", "63", "70", "80", "90", "100", "110",
	"125", "140", "160", "180", "200", "225", "250", "280",
	"315", "355", "400", "450", "500", "560", "630", "700",
	"800", "900", "1.0k", "1.1k", "1.2k", "1.4k", "1.6k", "1.8k",
	"2.0k", "2.2k", "2.5k", "2.8k", "3.2k", "3.6k", "4.0k", "4.5k",
	"5.0k", "5.6k", "6.3k", "7.0k", "8.0k",
};
inline constexpr const char *const T3[] = {
	"1.0k", "1.1k", "1.2k", "1.4k", "1.6k", "1.8k", "2.0k", "2.2k",
	"2.5k", "2.8k", "3.2k", "3.6k", "4.0k", "4.5k", "5.0k", "5.6k",
	"6.3k", "7.0k", "8.0k", "9.0k", "10k", "11k", "12k", "14k",
	"16k", "18k", "Thru",
};
inline constexpr const char *const T4[] = {
	"D63>W", "D62>W", "D61>W", "D60>W", "D59>W", "D58>W", "D57>W", "D56>W",
	"D55>W", "D54>W", "D53>W", "D52>W", "D51>W", "D50>W", "D49>W", "D48>W",
	"D47>W", "D46>W", "D45>W", "D44>W", "D43>W", "D42>W", "D41>W", "D40>W",
	"D39>W", "D38>W", "D37>W", "D36>W", "D35>W", "D34>W", "D33>W", "D32>W",
	"D31>W", "D30>W", "D29>W", "D28>W", "D27>W", "D26>W", "D25>W", "D24>W",
	"D23>W", "D22>W", "D21>W", "D20>W", "D19>W", "D18>W", "D17>W", "D16>W",
	"D15>W", "D14>W", "D13>W", "D12>W", "D11>W", "D10>W", "D09>W", "D08>W",
	"D07>W", "D06>W", "D05>W", "D04>W", "D03>W", "D02>W", "D01>W", "(D=W)",
	"D<W01", "D<W02", "D<W03", "D<W04", "D<W05", "D<W06", "D<W07", "D<W08",
	"D<W09", "D<W10", "D<W11", "D<W12", "D<W13", "D<W14", "D<W15", "D<W16",
	"D<W17", "D<W18", "D<W19", "D<W20", "D<W21", "D<W22", "D<W23", "D<W24",
	"D<W25", "D<W26", "D<W27", "D<W28", "D<W29", "D<W30", "D<W31", "D<W32",
	"D<W33", "D<W34", "D<W35", "D<W36", "D<W37", "D<W38", "D<W39", "D<W40",
	"D<W41", "D<W42", "D<W43", "D<W44", "D<W45", "D<W46", "D<W47", "D<W48",
	"D<W49", "D<W50", "D<W51", "D<W52", "D<W53", "D<W54", "D<W55", "D<W56",
	"D<W57", "D<W58", "D<W59", "D<W60", "D<W61", "D<W62", "D<W63",
};
inline constexpr const char *const T5[] = {
	"E63>R", "E62>R", "E61>R", "E60>R", "E59>R", "E58>R", "E57>R", "E56>R",
	"E55>R", "E54>R", "E53>R", "E52>R", "E51>R", "E50>R", "E49>R", "E48>R",
	"E47>R", "E46>R", "E45>R", "E44>R", "E43>R", "E42>R", "E41>R", "E40>R",
	"E39>R", "E38>R", "E37>R", "E36>R", "E35>R", "E34>R", "E33>R", "E32>R",
	"E31>R", "E30>R", "E29>R", "E28>R", "E27>R", "E26>R", "E25>R", "E24>R",
	"E23>R", "E22>R", "E21>R", "E20>R", "E19>R", "E18>R", "E17>R", "E16>R",
	"E15>R", "E14>R", "E13>R", "E12>R", "E11>R", "E10>R", "E09>R", "E08>R",
	"E07>R", "E06>R", "E05>R", "E04>R", "E03>R", "E02>R", "E01>R", "(E=R)",
	"E<R01", "E<R02", "E<R03", "E<R04", "E<R05", "E<R06", "E<R07", "E<R08",
	"E<R09", "E<R10", "E<R11", "E<R12", "E<R13", "E<R14", "E<R15", "E<R16",
	"E<R17", "E<R18", "E<R19", "E<R20", "E<R21", "E<R22", "E<R23", "E<R24",
	"E<R25", "E<R26", "E<R27", "E<R28", "E<R29", "E<R30", "E<R31", "E<R32",
	"E<R33", "E<R34", "E<R35", "E<R36", "E<R37", "E<R38", "E<R39", "E<R40",
	"E<R41", "E<R42", "E<R43", "E<R44", "E<R45", "E<R46", "E<R47", "E<R48",
	"E<R49", "E<R50", "E<R51", "E<R52", "E<R53", "E<R54", "E<R55", "E<R56",
	"E<R57", "E<R58", "E<R59", "E<R60", "E<R61", "E<R62", "E<R63",
};
inline constexpr const char *const T6[] = {
	"0.1", "0.2", "0.3", "0.4", "0.5", "0.6", "0.7", "0.8",
	"0.9", "1.0",
};
inline constexpr const char *const T7[] = {
	"-63", "-62", "-61", "-60", "-59", "-58", "-57", "-56",
	"-55", "-54", "-53", "-52", "-51", "-50", "-49", "-48",
	"-47", "-46", "-45", "-44", "-43", "-42", "-41", "-40",
	"-39", "-38", "-37", "-36", "-35", "-34", "-33", "-32",
	"-31", "-30", "-29", "-28", "-27", "-26", "-25", "-24",
	"-23", "-22", "-21", "-20", "-19", "-18", "-17", "-16",
	"-15", "-14", "-13", "-12", "-11", "-10", "-09", "-08",
	"-07", "-06", "-05", "-04", "-03", "-02", "-01", "+00",
	"+01", "+02", "+03", "+04", "+05", "+06", "+07", "+08",
	"+09", "+10", "+11", "+12", "+13", "+14", "+15", "+16",
	"+17", "+18", "+19", "+20", "+21", "+22", "+23", "+24",
	"+25", "+26", "+27", "+28", "+29", "+30", "+31", "+32",
	"+33", "+34", "+35", "+36", "+37", "+38", "+39", "+40",
	"+41", "+42", "+43", "+44", "+45", "+46", "+47", "+48",
	"+49", "+50", "+51", "+52", "+53", "+54", "+55", "+56",
	"+57", "+58", "+59", "+60", "+61", "+62", "+63",
};
inline constexpr const char *const T8[] = {
	"32", "36", "40", "45", "50", "56", "63", "70",
	"80", "90", "100", "110", "125", "140", "160", "180",
	"200", "225", "250", "280", "315", "355", "400", "450",
	"500", "560", "630", "700", "800", "900", "1.0k", "1.1k",
	"1.2k", "1.4k", "1.6k", "1.8k", "2.0k",
};
inline constexpr const char *const T9[] = {
	"-12", "-11", "-10", "-09", "-08", "-07", "-06", "-05",
	"-04", "-03", "-02", "-01", "+00", "+01", "+02", "+03",
	"+04", "+05", "+06", "+07", "+08", "+09", "+10", "+11",
	"+12",
};
inline constexpr const char *const T10[] = {
	"500", "560", "630", "700", "800", "900", "1.0k", "1.1k",
	"1.2k", "1.4k", "1.6k", "1.8k", "2.0k", "2.2k", "2.5k", "2.8k",
	"3.2k", "3.6k", "4.0k", "4.5k", "5.0k", "5.6k", "6.3k", "7.0k",
	"8.0k", "9.0k", "10k", "11k", "12k", "14k", "16k",
};
inline constexpr const char *const T11[] = {
	"L", "R", "L&R",
};
inline constexpr const char *const T12[] = {
	"S-H", "L-H", "Rdm", "Rvs", "Plt", "Spr",
};
inline constexpr const char *const T13[] = {
	"0.1", "0.3", "0.4", "0.6", "0.7", "0.9", "1.0", "1.2",
	"1.4", "1.5", "1.7", "1.8", "2.0", "2.1", "2.3", "2.5",
	"2.6", "2.8", "2.9", "3.1", "3.2", "3.4", "3.5", "3.7",
	"3.9", "4.0", "4.2", "4.3", "4.5", "4.6", "4.8", "5.0",
	"5.1", "5.3", "5.4", "5.6", "5.7", "5.9", "6.1", "6.2",
	"6.4", "6.5", "6.7", "6.8", "7.0",
};
inline constexpr const char *const T14[] = {
	"0.1", "1.7", "3.2", "4.8", "6.4", "8.0", "9.5", "11.1",
	"12.7", "14.3", "15.8", "17.4", "19.0", "20.6", "22.1", "23.7",
	"25.3", "26.9", "28.4", "30.0", "31.6", "33.2", "34.7", "36.3",
	"37.9", "39.5", "41.0", "42.6", "44.2", "45.7", "47.3", "48.9",
	"50.5", "52.0", "53.6", "55.2", "56.8", "58.3", "59.9", "61.5",
	"63.1", "64.6", "66.2", "67.8", "69.4", "70.9", "72.5", "74.1",
	"75.7", "77.2", "78.8", "80.4", "81.9", "83.5", "85.1", "86.7",
	"88.2", "89.8", "91.4", "93.0", "94.5", "96.1", "97.7", "99.3",
	"100.8", "102.4", "104.0", "105.6", "107.1", "108.7", "110.3", "111.9",
	"113.4", "115.0", "116.6", "118.2", "119.7", "121.3", "122.9", "124.4",
	"126.0", "127.6", "129.2", "130.7", "132.3", "133.9", "135.5", "137.0",
	"138.6", "140.2", "141.8", "143.3", "144.9", "146.5", "148.1", "149.6",
	"151.2", "152.8", "154.4", "155.9", "157.5", "159.1", "160.6", "162.2",
	"163.8", "165.4", "166.9", "168.5", "170.1", "171.7", "173.2", "174.8",
	"176.4", "178.0", "179.5", "181.1", "182.7", "184.3", "185.8", "187.4",
	"189.0", "190.6", "192.1", "193.7", "195.3", "196.9", "198.4", "200.0",
};
inline constexpr const char *const T15[] = {
	"TypeA", "TypeB",
};
inline constexpr const char *const T16[] = {
	"0.00Hz", "0.04Hz", "0.08Hz", "0.13Hz", "0.17Hz", "0.21Hz", "0.25Hz", "0.29Hz",
	"0.34Hz", "0.38Hz", "0.42Hz", "0.46Hz", "0.51Hz", "0.55Hz", "0.59Hz", "0.63Hz",
	"0.67Hz", "0.72Hz", "0.76Hz", "0.80Hz", "0.84Hz", "0.88Hz", "0.93Hz", "0.97Hz",
	"1.01Hz", "1.05Hz", "1.09Hz", "1.14Hz", "1.18Hz", "1.22Hz", "1.26Hz", "1.30Hz",
	"1.35Hz", "1.39Hz", "1.43Hz", "1.47Hz", "1.51Hz", "1.56Hz", "1.60Hz", "1.64Hz",
	"1.68Hz", "1.72Hz", "1.77Hz", "1.81Hz", "1.85Hz", "1.89Hz", "1.94Hz", "1.98Hz",
	"2.02Hz", "2.06Hz", "2.10Hz", "2.15Hz", "2.19Hz", "2.23Hz", "2.27Hz", "2.31Hz",
	"2.36Hz", "2.40Hz", "2.44Hz", "2.48Hz", "2.52Hz", "2.57Hz", "2.61Hz", "2.65Hz",
	"2.69Hz", "2.78Hz", "2.86Hz", "2.94Hz", "3.03Hz", "3.11Hz", "3.20Hz", "3.28Hz",
	"3.37Hz", "3.45Hz", "3.53Hz", "3.62Hz", "3.70Hz", "3.87Hz", "4.04Hz", "4.21Hz",
	"4.37Hz", "4.54Hz", "4.71Hz", "4.88Hz", "5.05Hz", "5.22Hz", "5.38Hz", "5.55Hz",
	"5.72Hz", "6.06Hz", "6.39Hz", "6.73Hz", "7.07Hz", "7.40Hz", "7.74Hz", "8.08Hz",
	"8.41Hz", "8.75Hz", "9.08Hz", "9.42Hz", "9.76Hz", "10.1Hz", "10.8Hz", "11.4Hz",
	"12.1Hz", "12.8Hz", "13.5Hz", "14.1Hz", "14.8Hz", "15.5Hz", "16.2Hz", "16.8Hz",
	"17.5Hz", "18.2Hz", "19.5Hz", "20.9Hz", "22.2Hz", "23.6Hz", "24.9Hz", "26.2Hz",
	"27.6Hz", "28.9Hz", "30.3Hz", "31.6Hz", "33.0Hz", "34.3Hz", "37.0Hz", "39.7Hz",
};
inline constexpr const char *const T17[] = {
	"0.0", "0.1", "0.2", "0.3", "0.4", "0.5", "0.6", "0.7",
	"0.8", "0.9", "1.0", "1.1", "1.2", "1.3", "1.4", "1.5",
	"1.6", "1.7", "1.8", "1.9", "2.0", "2.1", "2.2", "2.3",
	"2.4", "2.5", "2.6", "2.7", "2.8", "2.9", "3.0", "3.1",
	"3.2", "3.3", "3.4", "3.5", "3.6", "3.7", "3.8", "3.9",
	"4.0", "4.1", "4.2", "4.3", "4.4", "4.5", "4.6", "4.7",
	"4.8", "4.9", "5.0", "5.1", "5.2", "5.3", "5.4", "5.5",
	"5.6", "5.7", "5.8", "5.9", "6.0", "6.1", "6.2", "6.3",
	"6.4", "6.5", "6.6", "6.7", "6.8", "6.9", "7.0", "7.1",
	"7.2", "7.3", "7.4", "7.5", "7.6", "7.7", "7.8", "7.9",
	"8.0", "8.1", "8.2", "8.3", "8.4", "8.5", "8.6", "8.7",
	"8.8", "8.9", "9.0", "9.1", "9.2", "9.3", "9.4", "9.5",
	"9.6", "9.7", "9.8", "9.9", "10.0", "11.1", "12.2", "13.3",
	"14.4", "15.5", "17.1", "18.6", "20.2", "21.8", "23.3", "24.9",
	"26.5", "28.0", "29.6", "31.2", "32.8", "34.3", "35.9", "37.5",
	"39.0", "40.6", "42.2", "43.7", "45.3", "46.9", "48.4", "50.0",
};
inline constexpr const char *const T18[] = {
	"100", "110", "125", "140", "160", "180", "200", "225",
	"250", "280", "315", "355", "400", "450", "500", "560",
	"630", "700", "800", "900", "1.0k", "1.1k", "1.2k", "1.4k",
	"1.6k", "1.8k", "2.0k", "2.2k", "2.5k", "2.8k", "3.2k", "3.6k",
	"4.0k", "4.5k", "5.0k", "5.6k", "6.3k", "7.0k", "8.0k", "9.0k",
	"10k",
};
inline constexpr const char *const T19[] = {
	"1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7",
	"1.8", "1.9", "2.0", "2.1", "2.2", "2.3", "2.4", "2.5",
	"2.6", "2.7", "2.8", "2.9", "3.0", "3.1", "3.2", "3.3",
	"3.4", "3.5", "3.6", "3.7", "3.8", "3.9", "4.0", "4.1",
	"4.2", "4.3", "4.4", "4.5", "4.6", "4.7", "4.8", "4.9",
	"5.0", "5.1", "5.2", "5.3", "5.4", "5.5", "5.6", "5.7",
	"5.8", "5.9", "6.0", "6.1", "6.2", "6.3", "6.4", "6.5",
	"6.6", "6.7", "6.8", "6.9", "7.0", "7.1", "7.2", "7.3",
	"7.4", "7.5", "7.6", "7.7", "7.8", "7.9", "8.0", "8.1",
	"8.2", "8.3", "8.4", "8.5", "8.6", "8.7", "8.8", "8.9",
	"9.0", "9.1", "9.2", "9.3", "9.4", "9.5", "9.6", "9.7",
	"9.8", "9.9", "10.0", "10.1", "10.2", "10.3", "10.4", "10.5",
	"10.6", "10.7", "10.8", "10.9", "11.0", "11.1", "11.2", "11.3",
	"11.4", "11.5", "11.6", "11.7", "11.8", "11.9", "12.0",
};
inline constexpr const char *const T20[] = {
	"mono", "stero",
};
inline constexpr const char *const T21[] = {
	"-180", "-177", "-174", "-171", "-168", "-165", "-162", "-159",
	"-156", "-153", "-150", "-147", "-144", "-141", "-138", "-135",
	"-132", "-129", "-126", "-123", "-120", "-117", "-114", "-111",
	"-108", "-105", "-102", "-099", "-096", "-093", "-090", "-087",
	"-084", "-081", "-078", "-075", "-072", "-069", "-066", "-063",
	"-060", "-057", "-054", "-051", "-048", "-045", "-042", "-039",
	"-036", "-033", "-030", "-027", "-024", "-021", "-018", "-015",
	"-012", "-009", "-006", "-003", "+000", "+003", "+006", "+009",
	"+012", "+015", "+018", "+021", "+024", "+027", "+030", "+033",
	"+036", "+039", "+042", "+045", "+048", "+051", "+054", "+057",
	"+060", "+063", "+066", "+069", "+072", "+075", "+078", "+081",
	"+084", "+087", "+090", "+093", "+096", "+099", "+102", "+105",
	"+108", "+111", "+114", "+117", "+120", "+123", "+126", "+129",
	"+132", "+135", "+138", "+141", "+144", "+147", "+150", "+153",
	"+156", "+159", "+162", "+165", "+168", "+171", "+174", "+177",
	"+180",
};
inline constexpr const char *const T22[] = {
	"L.~R", "L~R", "L.R", "Lturn", "Rturn", "L/R",
};
inline constexpr const char *const T23[] = {
	"off", "Stack", "Combo", "Tube",
};
inline constexpr const char *const T24[] = {
	"50", "56", "63", "70", "80", "90", "100", "110",
	"125", "140", "160", "180", "200", "225", "250", "280",
	"315", "355", "400", "450", "500", "560", "630", "700",
	"800", "900", "1.0k", "1.1k", "1.2k", "1.4k", "1.6k", "1.8k",
	"2.0k",
};
} // namespace fx_text

// HALL 1
inline constexpr fx_param FX_01[] = {
	{ 0x02, 1,     0,    69, "ReverbTime",  fx_fmt::table, fx_text::T0 },
	{ 0x03, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,    63, "InitDelay",   fx_fmt::table, fx_text::T1 },
	{ 0x05, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x06, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    63, "Rev Delay",   fx_fmt::table, fx_text::T1 },
	{ 0x21, 1,     0,     4, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,   127, "Er/Rev",      fx_fmt::table, fx_text::T5 },
	{ 0x23, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x24, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
};
// ROOM 1
inline constexpr fx_param FX_02[] = {
	{ 0x02, 1,     0,    69, "ReverbTime",  fx_fmt::table, fx_text::T0 },
	{ 0x03, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,    63, "InitDelay",   fx_fmt::table, fx_text::T1 },
	{ 0x05, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x06, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    63, "Rev Delay",   fx_fmt::table, fx_text::T1 },
	{ 0x21, 1,     0,     4, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,   127, "Er/Rev",      fx_fmt::table, fx_text::T5 },
	{ 0x23, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x24, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
};
// STAGE 1
inline constexpr fx_param FX_03[] = {
	{ 0x02, 1,     0,    69, "ReverbTime",  fx_fmt::table, fx_text::T0 },
	{ 0x03, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,    63, "InitDelay",   fx_fmt::table, fx_text::T1 },
	{ 0x05, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x06, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    63, "Rev Delay",   fx_fmt::table, fx_text::T1 },
	{ 0x21, 1,     0,     4, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,   127, "Er/Rev",      fx_fmt::table, fx_text::T5 },
	{ 0x23, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x24, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
};
// PLATE
inline constexpr fx_param FX_04[] = {
	{ 0x02, 1,     0,    69, "ReverbTime",  fx_fmt::table, fx_text::T0 },
	{ 0x03, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,    63, "InitDelay",   fx_fmt::table, fx_text::T1 },
	{ 0x05, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x06, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    63, "Rev Delay",   fx_fmt::table, fx_text::T1 },
	{ 0x21, 1,     0,     4, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,   127, "Er/Rev",      fx_fmt::table, fx_text::T5 },
	{ 0x23, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x24, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
};
// DELAY LCR
inline constexpr fx_param FX_05[] = {
	{ 0x30, 2,     1, 14860, "LchDelay",    fx_fmt::tenths, nullptr },
	{ 0x32, 2,     1, 14860, "RchDelay",    fx_fmt::tenths, nullptr },
	{ 0x34, 2,     1, 14860, "CchDelay",    fx_fmt::tenths, nullptr },
	{ 0x36, 2,     1, 14860, "FB Delay",    fx_fmt::tenths, nullptr },
	{ 0x38, 2,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x3a, 2,     0,   127, "Cch Level",   fx_fmt::raw,   nullptr },
	{ 0x3c, 2,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x42, 2,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x22, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x23, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x24, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x25, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
};
// DELAY L,R
inline constexpr fx_param FX_06[] = {
	{ 0x30, 2,     1, 14860, "LchDelay",    fx_fmt::tenths, nullptr },
	{ 0x32, 2,     1, 14860, "RchDelay",    fx_fmt::tenths, nullptr },
	{ 0x34, 2,     1, 14860, "FBDelay1",    fx_fmt::tenths, nullptr },
	{ 0x36, 2,     1, 14860, "FBDelay2",    fx_fmt::tenths, nullptr },
	{ 0x38, 2,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x3a, 2,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x42, 2,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x22, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x23, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x24, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x25, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
};
// ECHO
inline constexpr fx_param FX_07[] = {
	{ 0x30, 2,     1,  7430, "Lch Delay",   fx_fmt::tenths, nullptr },
	{ 0x32, 2,     1,   127, "Lch FBLevl",  fx_fmt::table, fx_text::T7 },
	{ 0x34, 2,     1,  7430, "Rch Delay",   fx_fmt::tenths, nullptr },
	{ 0x36, 2,     1,   127, "Rch FBLevl",  fx_fmt::table, fx_text::T7 },
	{ 0x38, 2,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x3a, 2,     1,  7430, "LchDelay2",   fx_fmt::tenths, nullptr },
	{ 0x3c, 2,     1,  7430, "RchDelay2",   fx_fmt::tenths, nullptr },
	{ 0x3e, 2,     0,   127, "Delay2Lvl",   fx_fmt::raw,   nullptr },
	{ 0x42, 2,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x22, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x23, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x24, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x25, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
};
// CROSS DELAY
inline constexpr fx_param FX_08[] = {
	{ 0x30, 2,     1,  7430, "L~R Delay",   fx_fmt::tenths, nullptr },
	{ 0x32, 2,     1,  7430, "R~L Delay",   fx_fmt::tenths, nullptr },
	{ 0x34, 2,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x36, 2,     0,     2, "InputSelect", fx_fmt::table, fx_text::T11 },
	{ 0x38, 2,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
	{ 0x42, 2,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x22, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x23, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x24, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x25, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
};
// ER 1
inline constexpr fx_param FX_09[] = {
	{ 0x02, 1,     0,     5, "Early Type",  fx_fmt::table, fx_text::T12 },
	{ 0x03, 1,     0,    44, "Room Size",   fx_fmt::table, fx_text::T13 },
	{ 0x04, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x05, 1,     0,   127, "InitDelay",   fx_fmt::table, fx_text::T14 },
	{ 0x06, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x07, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x08, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    10, "Liveness",    fx_fmt::raw,   nullptr },
	{ 0x21, 1,     0,     3, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
};
// GATE REVERB
inline constexpr fx_param FX_0A[] = {
	{ 0x02, 1,     0,     1, "GateType",    fx_fmt::table, fx_text::T15 },
	{ 0x03, 1,     0,    44, "Room Size",   fx_fmt::table, fx_text::T13 },
	{ 0x04, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x05, 1,     0,   127, "InitDelay",   fx_fmt::table, fx_text::T14 },
	{ 0x06, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x07, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x08, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    10, "Liveness",    fx_fmt::raw,   nullptr },
	{ 0x21, 1,     0,     3, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
};
// GATE REVERB
inline constexpr fx_param FX_0B[] = {
	{ 0x02, 1,     0,     1, "GateType",    fx_fmt::table, fx_text::T15 },
	{ 0x03, 1,     0,    44, "Room Size",   fx_fmt::table, fx_text::T13 },
	{ 0x04, 1,     0,    10, "Diffusion",   fx_fmt::raw,   nullptr },
	{ 0x05, 1,     0,   127, "InitDelay",   fx_fmt::table, fx_text::T14 },
	{ 0x06, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x07, 1,     0,    52, "HPF Cutoff",  fx_fmt::table, fx_text::T2 },
	{ 0x08, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,    10, "Liveness",    fx_fmt::raw,   nullptr },
	{ 0x21, 1,     0,     3, "Density",     fx_fmt::raw,   nullptr },
	{ 0x22, 1,     1,    10, "High Damp",   fx_fmt::table, fx_text::T6 },
};
// CHORUS 1
inline constexpr fx_param FX_41[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x05, 1,     0,   127, "DelayOfst",   fx_fmt::table, fx_text::T17 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
	{ 0x24, 1,     0,     1, "InputMode",   fx_fmt::table, fx_text::T20 },
};
// CELESTE 1
inline constexpr fx_param FX_42[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x05, 1,     0,   127, "DelayOfst",   fx_fmt::table, fx_text::T17 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
	{ 0x24, 1,     0,     1, "InputMode",   fx_fmt::table, fx_text::T20 },
};
// FLANGER 1
inline constexpr fx_param FX_43[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x05, 1,     0,   127, "DelayOfst",   fx_fmt::table, fx_text::T17 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
	{ 0x23, 1,     4,   124, "LFO Phase",   fx_fmt::table, fx_text::T21 },
};
// SYMPHONIC
inline constexpr fx_param FX_44[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,   127, "DelayOfst",   fx_fmt::table, fx_text::T17 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
};
// ROTARY SP
inline constexpr fx_param FX_45[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
};
// TREMOLO
inline constexpr fx_param FX_46[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "AM Depth",    fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,   127, "PM Depth",    fx_fmt::raw,   nullptr },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
	{ 0x23, 1,     4,   124, "LFO Phase",   fx_fmt::table, fx_text::T21 },
	{ 0x24, 1,     0,     1, "InputMode",   fx_fmt::table, fx_text::T20 },
};
// AUTO PAN
inline constexpr fx_param FX_47[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "L/R Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,   127, "F/R Depth",   fx_fmt::raw,   nullptr },
	{ 0x05, 1,     0,     5, "PAN Dir",     fx_fmt::table, fx_text::T22 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x20, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x21, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x22, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
};
// PHASER 1
inline constexpr fx_param FX_48[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,   127, "PhaseShift",  fx_fmt::raw,   nullptr },
	{ 0x05, 1,     1,   127, "FB Level",    fx_fmt::table, fx_text::T7 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     4,    12, "Stage",       fx_fmt::raw,   nullptr },
	{ 0x21, 1,     0,     1, "Diffusion",   fx_fmt::table, fx_text::T20 },
};
// DISTORTION
inline constexpr fx_param FX_49[] = {
	{ 0x02, 1,     0,   127, "Drive",       fx_fmt::raw,   nullptr },
	{ 0x03, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x04, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x05, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x06, 1,     0,   127, "OutputLvl",   fx_fmt::raw,   nullptr },
	{ 0x08, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x09, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0a, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,   127, "Edge",        fx_fmt::raw,   nullptr },
};
// OVERDRIVE
inline constexpr fx_param FX_4A[] = {
	{ 0x02, 1,     0,   127, "Drive",       fx_fmt::raw,   nullptr },
	{ 0x03, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x04, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x05, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x06, 1,     0,   127, "OutputLvl",   fx_fmt::raw,   nullptr },
	{ 0x08, 1,    14,    54, "EQ MidFreq",  fx_fmt::table, fx_text::T18 },
	{ 0x09, 1,    52,    76, "EQ MidGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0a, 1,    10,   120, "EQ MidWidt",  fx_fmt::table, fx_text::T19 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,   127, "Edge",        fx_fmt::raw,   nullptr },
};
// AMP SIM
inline constexpr fx_param FX_4B[] = {
	{ 0x02, 1,     0,   127, "Drive",       fx_fmt::raw,   nullptr },
	{ 0x03, 1,     0,     3, "AmpType",     fx_fmt::table, fx_text::T23 },
	{ 0x04, 1,    34,    60, "LPF Cutoff",  fx_fmt::table, fx_text::T3 },
	{ 0x05, 1,     0,   127, "OutputLvl",   fx_fmt::raw,   nullptr },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,   127, "Edge",        fx_fmt::raw,   nullptr },
};
// 3BAND EQ
inline constexpr fx_param FX_4C[] = {
	{ 0x02, 1,    52,    76, "Low Gain",    fx_fmt::table, fx_text::T9 },
	{ 0x03, 1,    14,    54, "Mid Freq",    fx_fmt::table, fx_text::T18 },
	{ 0x04, 1,    52,    76, "Mid Gain",    fx_fmt::table, fx_text::T9 },
	{ 0x05, 1,    10,   120, "Mid Width",   fx_fmt::table, fx_text::T19 },
	{ 0x06, 1,    52,    76, "High Gain",   fx_fmt::table, fx_text::T9 },
	{ 0x07, 1,     8,    40, "Low Freq",    fx_fmt::table, fx_text::T24 },
	{ 0x08, 1,    28,    58, "High Freq",   fx_fmt::table, fx_text::T10 },
	{ 0x24, 1,     0,     1, "InputMode",   fx_fmt::table, fx_text::T20 },
};
// 2BAND EQ
inline constexpr fx_param FX_4D[] = {
	{ 0x02, 1,     4,    40, "Low Freq",    fx_fmt::table, fx_text::T8 },
	{ 0x03, 1,    52,    76, "Low Gain",    fx_fmt::table, fx_text::T9 },
	{ 0x04, 1,    28,    58, "High Freq",   fx_fmt::table, fx_text::T10 },
	{ 0x05, 1,    52,    76, "High Gain",   fx_fmt::table, fx_text::T9 },
};
// AUTO WAH
inline constexpr fx_param FX_4E[] = {
	{ 0x02, 1,     0,   127, "LFO Freq",    fx_fmt::table, fx_text::T16 },
	{ 0x03, 1,     0,   127, "LFO Depth",   fx_fmt::raw,   nullptr },
	{ 0x04, 1,     0,   127, "CutoffFreq",  fx_fmt::raw,   nullptr },
	{ 0x05, 1,    10,   120, "Resonance",   fx_fmt::table, fx_text::T19 },
	{ 0x07, 1,     4,    40, "EQ LowFreq",  fx_fmt::table, fx_text::T8 },
	{ 0x08, 1,    52,    76, "EQ LowGain",  fx_fmt::table, fx_text::T9 },
	{ 0x09, 1,    28,    58, "EQHighFreq",  fx_fmt::table, fx_text::T10 },
	{ 0x0a, 1,    52,    76, "EQHighGain",  fx_fmt::table, fx_text::T9 },
	{ 0x0b, 1,     1,   127, "Dry/Wet",     fx_fmt::table, fx_text::T4 },
	{ 0x20, 1,     0,   127, "Drive",       fx_fmt::raw,   nullptr },
};

inline constexpr fx_def FX_DEFS[] = {
	{ 0x01, FX_01, 11 },
	{ 0x02, FX_02, 11 },
	{ 0x03, FX_03, 11 },
	{ 0x04, FX_04, 11 },
	{ 0x05, FX_05, 12 },
	{ 0x06, FX_06, 11 },
	{ 0x07, FX_07, 13 },
	{ 0x08, FX_08, 10 },
	{ 0x09, FX_09, 11 },
	{ 0x0a, FX_0A, 11 },
	{ 0x0b, FX_0B, 11 },
	{ 0x41, FX_41, 13 },
	{ 0x42, FX_42, 13 },
	{ 0x43, FX_43, 13 },
	{ 0x44, FX_44, 11 },
	{ 0x45, FX_45, 10 },
	{ 0x46, FX_46, 12 },
	{ 0x47, FX_47, 11 },
	{ 0x48, FX_48, 11 },
	{ 0x49, FX_49, 10 },
	{ 0x4a, FX_4A, 10 },
	{ 0x4b, FX_4B, 6 },
	{ 0x4c, FX_4C, 8 },
	{ 0x4d, FX_4D, 4 },
	{ 0x4e, FX_4E, 10 },
};

// 種類（MSB << 7 | LSB）から。表に無ければ nullptr
inline const fx_def *fx_find(int type)
{
	for (const fx_def &d : FX_DEFS)
		if (d.msb == (type >> 7))
			return &d;
	return nullptr;
}

} // namespace xg

#endif // S_MU2000_XG_FX_PARAMS_H
