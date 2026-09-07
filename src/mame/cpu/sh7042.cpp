// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

// SH7042, sh2 variant

#include "emu.h"
#include "sh7042.h"

DEFINE_DEVICE_TYPE(SH7042,  sh7042_device,  "sh7042",  "Hitachi SH-2 (SH7042)")
DEFINE_DEVICE_TYPE(SH7042A, sh7042a_device, "sh7042a", "Hitachi SH-2 (SH7042A)")
DEFINE_DEVICE_TYPE(SH7043,  sh7043_device,  "sh7043",  "Hitachi SH-2 (SH7043)")
DEFINE_DEVICE_TYPE(SH7043A, sh7043a_device, "sh7043a", "Hitachi SH-2 (SH7043A)")

sh7042_device::sh7042_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	sh7042_device(mconfig, SH7042, tag, owner, clock)
{
	m_die_a = false;
}

sh7042a_device::sh7042a_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	sh7042_device(mconfig, SH7042A, tag, owner, clock)
{
	m_die_a = true;
}

sh7043_device::sh7043_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	sh7042_device(mconfig, SH7043, tag, owner, clock)
{
	m_die_a = false;
}

sh7043a_device::sh7043a_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	sh7042_device(mconfig, SH7043A, tag, owner, clock)
{
	m_die_a = true;
}

sh7042_device::sh7042_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock) :
	sh2_device(mconfig, type, tag, owner, clock, CPU_TYPE_SH2, address_map_constructor(FUNC(sh7042_device::map), this), 32, 0xffffffff),
	m_intc(*this, "intc"),
	m_adc0(*this, "adc0"),
	m_adc1(*this, "adc1"),
	m_bsc(*this, "bsc"),
	m_cmt(*this, "cmt"),
	m_dmac(*this, "dmac"),
	m_dmac0(*this, "dmac:0"),
	m_dmac1(*this, "dmac:1"),
	m_dmac2(*this, "dmac:2"),
	m_dmac3(*this, "dmac:3"),
	m_mtu(*this, "mtu"),
	m_mtu0(*this, "mtu:0"),
	m_mtu1(*this, "mtu:1"),
	m_mtu2(*this, "mtu:2"),
	m_mtu3(*this, "mtu:3"),
	m_mtu4(*this, "mtu:4"),
	m_porta(*this, "porta"),
	m_portb(*this, "portb"),
	m_portc(*this, "portc"),
	m_portd(*this, "portd"),
	m_porte(*this, "porte"),
	m_portf(*this, "portf"),
	m_sci(*this, "sci%d", 0),
	m_read_adc(*this, 0),
	m_sci_tx(*this),
	m_sci_clk(*this),
	m_read_port16(*this, 0xffff),
	m_write_port16(*this),
	m_read_port32(*this, 0xffffffff),
	m_write_port32(*this)
{
	m_port16_names = "bcef";
	m_port32_names = "ad";
	for(unsigned int i=0; i != m_read_adc.size(); i++)
		m_read_adc[i].bind().set([this, i]() { return adc_default(i); });
	for(unsigned int i=0; i != m_read_port16.size(); i++) {
		m_read_port16[i].bind().set([this, i]() { return port16_default_r(i); });
		m_write_port16[i].bind().set([this, i](u16 data) { port16_default_w(i, data); });
	}
	for(unsigned int i=0; i != m_read_port32.size(); i++) {
		m_read_port32[i].bind().set([this, i]() { return port32_default_r(i); });
		m_write_port32[i].bind().set([this, i](u32 data) { port32_default_w(i, data); });
	}
}

u16 sh7042_device::port16_default_r(int port)
{
	if(!machine().side_effects_disabled())
		logerror("read of un-hooked port %c\n", m_port16_names[port]);
	return 0xffff;
}

void sh7042_device::port16_default_w(int port, u16 data)
{
	logerror("write of un-hooked port %c %04x\n", m_port16_names[port], data);
}

u32 sh7042_device::port32_default_r(int port)
{
	if(!machine().side_effects_disabled())
		logerror("read of un-hooked port %c\n", m_port32_names[port]);
	return 0xffff;
}

void sh7042_device::port32_default_w(int port, u32 data)
{
	logerror("write of un-hooked port %c %04x\n", m_port32_names[port], data);
}


u16 sh7042_device::adc_default(int adc)
{
	logerror("read of un-hooked adc %d\n", adc);
	return 0;
}

void sh7042_device::device_start()
{
	sh2_device::device_start();

	m_event_timer = timer_alloc(FUNC(sh7042_device::event_timer_tick), this);

	save_item(NAME(m_pcf_ah));
	save_item(NAME(m_pcf_al));
	save_item(NAME(m_pcf_b));
	save_item(NAME(m_pcf_c));
	save_item(NAME(m_pcf_dh));
	save_item(NAME(m_pcf_dl));
	save_item(NAME(m_pcf_e));
	save_item(NAME(m_pcf_if));

	m_pcf_ah = 0;
	m_pcf_al = 0;
	m_pcf_b = 0;
	m_pcf_c = 0;
	m_pcf_dh = 0;
	m_pcf_dl = 0;
	m_pcf_e = 0;
	m_pcf_if = 0;
}

void sh7042_device::execute_set_input(int irqline, int state)
{
	m_intc->set_input(irqline, state);
}

void sh7042_device::device_reset()
{
	sh2_device::device_reset();
}


// S-MU2000: address_map をやめ、番地で振り分ける形にした。
// 移植したのは firmware が実際に触る周辺だけ（SCI/MTU/CMT/INTC/PORT と
// CPU 自身のピン機能設定 pcf_*）。BSC と DMAC は起動時に少し触られるだけなので
// 何もしない。ADC は参照 0 回だったので配線していない。
// 内蔵 RAM 0xFFFFF000- は mem_bus 側で領域として持つ。
// 根拠は doc/design.md「どの内蔵周辺が要るか」。

u16 sh7042_device::internal_r(offs_t a)
{
	if(a >= 0xffff81a0 && a <= 0xffff81a0) return m_sci[0]->smr_r();
	if(a >= 0xffff81a1 && a <= 0xffff81a1) return m_sci[0]->brr_r();
	if(a >= 0xffff81a2 && a <= 0xffff81a2) return m_sci[0]->scr_r();
	if(a >= 0xffff81a3 && a <= 0xffff81a3) return m_sci[0]->tdr_r();
	if(a >= 0xffff81a4 && a <= 0xffff81a4) return m_sci[0]->ssr_r();
	if(a >= 0xffff81a5 && a <= 0xffff81a5) return m_sci[0]->rdr_r();
	if(a >= 0xffff81b0 && a <= 0xffff81b0) return m_sci[1]->smr_r();
	if(a >= 0xffff81b1 && a <= 0xffff81b1) return m_sci[1]->brr_r();
	if(a >= 0xffff81b2 && a <= 0xffff81b2) return m_sci[1]->scr_r();
	if(a >= 0xffff81b3 && a <= 0xffff81b3) return m_sci[1]->tdr_r();
	if(a >= 0xffff81b4 && a <= 0xffff81b4) return m_sci[1]->ssr_r();
	if(a >= 0xffff81b5 && a <= 0xffff81b5) return m_sci[1]->rdr_r();
	if(a >= 0xffff8200 && a <= 0xffff8200) return m_mtu3->tcr_r();
	if(a >= 0xffff8201 && a <= 0xffff8201) return m_mtu4->tcr_r();
	if(a >= 0xffff8202 && a <= 0xffff8202) return m_mtu3->tmdr_r();
	if(a >= 0xffff8203 && a <= 0xffff8203) return m_mtu4->tmdr_r();
	if(a >= 0xffff8204 && a <= 0xffff8205) return m_mtu3->tior_r();
	if(a >= 0xffff8206 && a <= 0xffff8207) return m_mtu4->tior_r();
	if(a >= 0xffff8208 && a <= 0xffff8208) return m_mtu3->tier_r();
	if(a >= 0xffff8209 && a <= 0xffff8209) return m_mtu4->tier_r();
	if(a >= 0xffff820a && a <= 0xffff820a) return m_mtu->toer_r();
	if(a >= 0xffff820b && a <= 0xffff820b) return m_mtu->tocr_r();
	if(a >= 0xffff820d && a <= 0xffff820d) return m_mtu->tgcr_r();
	if(a >= 0xffff8210 && a <= 0xffff8211) return m_mtu3->tcnt_r();
	if(a >= 0xffff8212 && a <= 0xffff8213) return m_mtu4->tcnt_r();
	if(a >= 0xffff8214 && a <= 0xffff8215) return m_mtu->tcdr_r();
	if(a >= 0xffff8216 && a <= 0xffff8217) return m_mtu->tddr_r();
	if(a >= 0xffff8218 && a <= 0xffff821b) return m_mtu3->tgr_r();
	if(a >= 0xffff821c && a <= 0xffff821f) return m_mtu4->tgr_r();
	if(a >= 0xffff8220 && a <= 0xffff8221) return m_mtu->tcnts_r();
	if(a >= 0xffff8222 && a <= 0xffff8223) return m_mtu->tcbr_r();
	if(a >= 0xffff8224 && a <= 0xffff8227) return m_mtu3->tgrc_r();
	if(a >= 0xffff8228 && a <= 0xffff822b) return m_mtu4->tgrc_r();
	if(a >= 0xffff822c && a <= 0xffff822c) return m_mtu3->tsr_r();
	if(a >= 0xffff822d && a <= 0xffff822d) return m_mtu4->tsr_r();
	if(a >= 0xffff8240 && a <= 0xffff8240) return m_mtu->tstr_r();
	if(a >= 0xffff8241 && a <= 0xffff8241) return m_mtu->tsyr_r();
	if(a >= 0xffff8260 && a <= 0xffff8260) return m_mtu0->tcr_r();
	if(a >= 0xffff8261 && a <= 0xffff8261) return m_mtu0->tmdr_r();
	if(a >= 0xffff8262 && a <= 0xffff8263) return m_mtu0->tior_r();
	if(a >= 0xffff8264 && a <= 0xffff8264) return m_mtu0->tier_r();
	if(a >= 0xffff8265 && a <= 0xffff8265) return m_mtu0->tsr_r();
	if(a >= 0xffff8266 && a <= 0xffff8267) return m_mtu0->tcnt_r();
	if(a >= 0xffff8268 && a <= 0xffff826f) return m_mtu0->tgr_r();
	if(a >= 0xffff8280 && a <= 0xffff8280) return m_mtu1->tcr_r();
	if(a >= 0xffff8281 && a <= 0xffff8281) return m_mtu1->tmdr_r();
	if(a >= 0xffff8282 && a <= 0xffff8283) return m_mtu1->tior_r();
	if(a >= 0xffff8284 && a <= 0xffff8284) return m_mtu1->tier_r();
	if(a >= 0xffff8285 && a <= 0xffff8285) return m_mtu1->tsr_r();
	if(a >= 0xffff8286 && a <= 0xffff8287) return m_mtu1->tcnt_r();
	if(a >= 0xffff8288 && a <= 0xffff828b) return m_mtu1->tgr_r();
	if(a >= 0xffff82a0 && a <= 0xffff82a0) return m_mtu2->tcr_r();
	if(a >= 0xffff82a1 && a <= 0xffff82a1) return m_mtu2->tmdr_r();
	if(a >= 0xffff82a2 && a <= 0xffff82a3) return m_mtu2->tior_r();
	if(a >= 0xffff82a4 && a <= 0xffff82a4) return m_mtu2->tier_r();
	if(a >= 0xffff82a5 && a <= 0xffff82a5) return m_mtu2->tsr_r();
	if(a >= 0xffff82a6 && a <= 0xffff82a7) return m_mtu2->tcnt_r();
	if(a >= 0xffff82a8 && a <= 0xffff82ab) return m_mtu2->tgr_r();
	if(a >= 0xffff8348 && a <= 0xffff8357) return m_intc->ipr_r();
	if(a >= 0xffff8358 && a <= 0xffff8359) return m_intc->icr_r();
	if(a >= 0xffff835a && a <= 0xffff835b) return m_intc->isr_r();
	if(a >= 0xffff8380 && a <= 0xffff8383) return m_porta->dr_r();
	if(a >= 0xffff8384 && a <= 0xffff8387) return m_porta->io_r();
	if(a >= 0xffff8388 && a <= 0xffff8389) return pcf_ah_r();
	if(a >= 0xffff838c && a <= 0xffff838f) return pcf_al_r();
	if(a >= 0xffff8390 && a <= 0xffff8391) return m_portb->dr_r();
	if(a >= 0xffff8392 && a <= 0xffff8393) return m_portc->dr_r();
	if(a >= 0xffff8394 && a <= 0xffff8395) return m_portb->io_r();
	if(a >= 0xffff8396 && a <= 0xffff8397) return m_portc->io_r();
	if(a >= 0xffff8398 && a <= 0xffff839b) return pcf_b_r();
	if(a >= 0xffff839c && a <= 0xffff839d) return pcf_c_r();
	if(a >= 0xffff83a0 && a <= 0xffff83a3) return m_portd->dr_r();
	if(a >= 0xffff83a4 && a <= 0xffff83a7) return m_portd->io_r();
	if(a >= 0xffff83a8 && a <= 0xffff83ab) return pcf_dh_r();
	if(a >= 0xffff83ac && a <= 0xffff83ad) return pcf_dl_r();
	if(a >= 0xffff83b0 && a <= 0xffff83b1) return m_porte->dr_r();
	if(a >= 0xffff83b2 && a <= 0xffff83b3) return m_portf->dr_r();
	if(a >= 0xffff83b4 && a <= 0xffff83b5) return m_porte->io_r();
	if(a >= 0xffff83b8 && a <= 0xffff83bb) return pcf_e_r();
	if(a >= 0xffff83c8 && a <= 0xffff83c9) return pcf_if_r();
	if(a >= 0xffff83d0 && a <= 0xffff83d1) return m_cmt->cmstr_r();
	if(a >= 0xffff83d2 && a <= 0xffff83d3) return m_cmt->cmcsr0_r();
	if(a >= 0xffff83d4 && a <= 0xffff83d5) return m_cmt->cmcnt0_r();
	if(a >= 0xffff83d6 && a <= 0xffff83d7) return m_cmt->cmcor0_r();
	if(a >= 0xffff83d8 && a <= 0xffff83d9) return m_cmt->cmcsr1_r();
	if(a >= 0xffff83da && a <= 0xffff83db) return m_cmt->cmcnt1_r();
	if(a >= 0xffff83dc && a <= 0xffff83dd) return m_cmt->cmcor1_r();
	return 0;
}

void sh7042_device::internal_w(offs_t a, u16 v)
{
	if(a >= 0xffff81a0 && a <= 0xffff81a0) { m_sci[0]->smr_w(v); return; }
	if(a >= 0xffff81a1 && a <= 0xffff81a1) { m_sci[0]->brr_w(v); return; }
	if(a >= 0xffff81a2 && a <= 0xffff81a2) { m_sci[0]->scr_w(v); return; }
	if(a >= 0xffff81a3 && a <= 0xffff81a3) { m_sci[0]->tdr_w(v); return; }
	if(a >= 0xffff81a4 && a <= 0xffff81a4) { m_sci[0]->ssr_w(v); return; }
	if(a >= 0xffff81b0 && a <= 0xffff81b0) { m_sci[1]->smr_w(v); return; }
	if(a >= 0xffff81b1 && a <= 0xffff81b1) { m_sci[1]->brr_w(v); return; }
	if(a >= 0xffff81b2 && a <= 0xffff81b2) { m_sci[1]->scr_w(v); return; }
	if(a >= 0xffff81b3 && a <= 0xffff81b3) { m_sci[1]->tdr_w(v); return; }
	if(a >= 0xffff81b4 && a <= 0xffff81b4) { m_sci[1]->ssr_w(v); return; }
	if(a >= 0xffff8200 && a <= 0xffff8200) { m_mtu3->tcr_w(v); return; }
	if(a >= 0xffff8201 && a <= 0xffff8201) { m_mtu4->tcr_w(v); return; }
	if(a >= 0xffff8202 && a <= 0xffff8202) { m_mtu3->tmdr_w(v); return; }
	if(a >= 0xffff8203 && a <= 0xffff8203) { m_mtu4->tmdr_w(v); return; }
	if(a >= 0xffff8204 && a <= 0xffff8205) { m_mtu3->tior_w(v); return; }
	if(a >= 0xffff8206 && a <= 0xffff8207) { m_mtu4->tior_w(v); return; }
	if(a >= 0xffff8208 && a <= 0xffff8208) { m_mtu3->tier_w(v); return; }
	if(a >= 0xffff8209 && a <= 0xffff8209) { m_mtu4->tier_w(v); return; }
	if(a >= 0xffff820a && a <= 0xffff820a) { m_mtu->toer_w(v); return; }
	if(a >= 0xffff820b && a <= 0xffff820b) { m_mtu->tocr_w(v); return; }
	if(a >= 0xffff820d && a <= 0xffff820d) { m_mtu->tgcr_w(v); return; }
	if(a >= 0xffff8210 && a <= 0xffff8211) { m_mtu3->tcnt_w(v); return; }
	if(a >= 0xffff8212 && a <= 0xffff8213) { m_mtu4->tcnt_w(v); return; }
	if(a >= 0xffff8214 && a <= 0xffff8215) { m_mtu->tcdr_w(v); return; }
	if(a >= 0xffff8216 && a <= 0xffff8217) { m_mtu->tddr_w(v); return; }
	if(a >= 0xffff8218 && a <= 0xffff821b) { m_mtu3->tgr_w(v); return; }
	if(a >= 0xffff821c && a <= 0xffff821f) { m_mtu4->tgr_w(v); return; }
	if(a >= 0xffff8220 && a <= 0xffff8221) { m_mtu->tcnts_w(v); return; }
	if(a >= 0xffff8222 && a <= 0xffff8223) { m_mtu->tcbr_w(v); return; }
	if(a >= 0xffff8224 && a <= 0xffff8227) { m_mtu3->tgrc_w(v); return; }
	if(a >= 0xffff8228 && a <= 0xffff822b) { m_mtu4->tgrc_w(v); return; }
	if(a >= 0xffff822c && a <= 0xffff822c) { m_mtu3->tsr_w(v); return; }
	if(a >= 0xffff822d && a <= 0xffff822d) { m_mtu4->tsr_w(v); return; }
	if(a >= 0xffff8240 && a <= 0xffff8240) { m_mtu->tstr_w(v); return; }
	if(a >= 0xffff8241 && a <= 0xffff8241) { m_mtu->tsyr_w(v); return; }
	if(a >= 0xffff8260 && a <= 0xffff8260) { m_mtu0->tcr_w(v); return; }
	if(a >= 0xffff8261 && a <= 0xffff8261) { m_mtu0->tmdr_w(v); return; }
	if(a >= 0xffff8262 && a <= 0xffff8263) { m_mtu0->tior_w(v); return; }
	if(a >= 0xffff8264 && a <= 0xffff8264) { m_mtu0->tier_w(v); return; }
	if(a >= 0xffff8265 && a <= 0xffff8265) { m_mtu0->tsr_w(v); return; }
	if(a >= 0xffff8266 && a <= 0xffff8267) { m_mtu0->tcnt_w(v); return; }
	if(a >= 0xffff8268 && a <= 0xffff826f) { m_mtu0->tgr_w(v); return; }
	if(a >= 0xffff8280 && a <= 0xffff8280) { m_mtu1->tcr_w(v); return; }
	if(a >= 0xffff8281 && a <= 0xffff8281) { m_mtu1->tmdr_w(v); return; }
	if(a >= 0xffff8282 && a <= 0xffff8283) { m_mtu1->tior_w(v); return; }
	if(a >= 0xffff8284 && a <= 0xffff8284) { m_mtu1->tier_w(v); return; }
	if(a >= 0xffff8285 && a <= 0xffff8285) { m_mtu1->tsr_w(v); return; }
	if(a >= 0xffff8286 && a <= 0xffff8287) { m_mtu1->tcnt_w(v); return; }
	if(a >= 0xffff8288 && a <= 0xffff828b) { m_mtu1->tgr_w(v); return; }
	if(a >= 0xffff82a0 && a <= 0xffff82a0) { m_mtu2->tcr_w(v); return; }
	if(a >= 0xffff82a1 && a <= 0xffff82a1) { m_mtu2->tmdr_w(v); return; }
	if(a >= 0xffff82a2 && a <= 0xffff82a3) { m_mtu2->tior_w(v); return; }
	if(a >= 0xffff82a4 && a <= 0xffff82a4) { m_mtu2->tier_w(v); return; }
	if(a >= 0xffff82a5 && a <= 0xffff82a5) { m_mtu2->tsr_w(v); return; }
	if(a >= 0xffff82a6 && a <= 0xffff82a7) { m_mtu2->tcnt_w(v); return; }
	if(a >= 0xffff82a8 && a <= 0xffff82ab) { m_mtu2->tgr_w(v); return; }
	if(a >= 0xffff8348 && a <= 0xffff8357) { m_intc->ipr_w(v); return; }
	if(a >= 0xffff8358 && a <= 0xffff8359) { m_intc->icr_w(v); return; }
	if(a >= 0xffff835a && a <= 0xffff835b) { m_intc->isr_w(v); return; }
	if(a >= 0xffff8380 && a <= 0xffff8383) { m_porta->dr_w(v); return; }
	if(a >= 0xffff8384 && a <= 0xffff8387) { m_porta->io_w(v); return; }
	if(a >= 0xffff8388 && a <= 0xffff8389) { pcf_ah_w(v); return; }
	if(a >= 0xffff838c && a <= 0xffff838f) { pcf_al_w(v); return; }
	if(a >= 0xffff8390 && a <= 0xffff8391) { m_portb->dr_w(v); return; }
	if(a >= 0xffff8392 && a <= 0xffff8393) { m_portc->dr_w(v); return; }
	if(a >= 0xffff8394 && a <= 0xffff8395) { m_portb->io_w(v); return; }
	if(a >= 0xffff8396 && a <= 0xffff8397) { m_portc->io_w(v); return; }
	if(a >= 0xffff8398 && a <= 0xffff839b) { pcf_b_w(v); return; }
	if(a >= 0xffff839c && a <= 0xffff839d) { pcf_c_w(v); return; }
	if(a >= 0xffff83a0 && a <= 0xffff83a3) { m_portd->dr_w(v); return; }
	if(a >= 0xffff83a4 && a <= 0xffff83a7) { m_portd->io_w(v); return; }
	if(a >= 0xffff83a8 && a <= 0xffff83ab) { pcf_dh_w(v); return; }
	if(a >= 0xffff83ac && a <= 0xffff83ad) { pcf_dl_w(v); return; }
	if(a >= 0xffff83b0 && a <= 0xffff83b1) { m_porte->dr_w(v); return; }
	if(a >= 0xffff83b4 && a <= 0xffff83b5) { m_porte->io_w(v); return; }
	if(a >= 0xffff83b8 && a <= 0xffff83bb) { pcf_e_w(v); return; }
	if(a >= 0xffff83c8 && a <= 0xffff83c9) { pcf_if_w(v); return; }
	if(a >= 0xffff83d0 && a <= 0xffff83d1) { m_cmt->cmstr_w(v); return; }
	if(a >= 0xffff83d2 && a <= 0xffff83d3) { m_cmt->cmcsr0_w(v); return; }
	if(a >= 0xffff83d4 && a <= 0xffff83d5) { m_cmt->cmcnt0_w(v); return; }
	if(a >= 0xffff83d6 && a <= 0xffff83d7) { m_cmt->cmcor0_w(v); return; }
	if(a >= 0xffff83d8 && a <= 0xffff83d9) { m_cmt->cmcsr1_w(v); return; }
	if(a >= 0xffff83da && a <= 0xffff83db) { m_cmt->cmcnt1_w(v); return; }
	if(a >= 0xffff83dc && a <= 0xffff83dd) { m_cmt->cmcor1_w(v); return; }
}

void sh7042_device::device_add_mconfig(machine_config &config)
{
	SH_INTC(config, m_intc, *this);
	if(m_die_a) {
		SH_ADC_MS(config, m_adc0, *this, m_intc, 0, 136);
		SH_ADC_MS(config, m_adc1, *this, m_intc, 4, 137);
	} else
		SH_ADC_HS(config, m_adc0, *this, m_intc, 136);
	SH_BSC(config, m_bsc);
	SH_CMT(config, m_cmt, *this, m_intc, 144, 148);
	SH_DMAC(config, m_dmac, *this);
	SH_DMAC_CHANNEL(config, m_dmac0, *this, m_intc);
	SH_DMAC_CHANNEL(config, m_dmac1, *this, m_intc);
	SH_DMAC_CHANNEL(config, m_dmac2, *this, m_intc);
	SH_DMAC_CHANNEL(config, m_dmac3, *this, m_intc);
	SH_MTU(config, m_mtu, *this, 5);
	SH_MTU_CHANNEL(config, m_mtu0, *this, 4, 0x60, m_intc, 88,
			sh_mtu_channel_device::DIV_1,
			sh_mtu_channel_device::DIV_4,
			sh_mtu_channel_device::DIV_16,
			sh_mtu_channel_device::DIV_64,
			sh_mtu_channel_device::INPUT_A,
			sh_mtu_channel_device::INPUT_B,
			sh_mtu_channel_device::INPUT_C,
			sh_mtu_channel_device::INPUT_D);
	SH_MTU_CHANNEL(config, m_mtu1, *this, 2, 0x4c, m_intc, 96,
			sh_mtu_channel_device::DIV_1,
			sh_mtu_channel_device::DIV_4,
			sh_mtu_channel_device::DIV_16,
			sh_mtu_channel_device::DIV_64,
			sh_mtu_channel_device::INPUT_A,
			sh_mtu_channel_device::INPUT_B,
			sh_mtu_channel_device::DIV_256,
			sh_mtu_channel_device::CHAIN).set_chain(m_mtu2);
	SH_MTU_CHANNEL(config, m_mtu2, *this, 2, 0x4c, m_intc, 104,
			sh_mtu_channel_device::DIV_1,
			sh_mtu_channel_device::DIV_4,
			sh_mtu_channel_device::DIV_16,
			sh_mtu_channel_device::DIV_64,
			sh_mtu_channel_device::INPUT_A,
			sh_mtu_channel_device::INPUT_B,
			sh_mtu_channel_device::INPUT_C,
			sh_mtu_channel_device::DIV_1024);
	SH_MTU_CHANNEL(config, m_mtu3, *this, 4, 0x60, m_intc, 112,
			sh_mtu_channel_device::DIV_1,
			sh_mtu_channel_device::DIV_4,
			sh_mtu_channel_device::DIV_16,
			sh_mtu_channel_device::DIV_64,
			sh_mtu_channel_device::DIV_256,
			sh_mtu_channel_device::DIV_1024,
			sh_mtu_channel_device::INPUT_A,
			sh_mtu_channel_device::INPUT_B);
	SH_MTU_CHANNEL(config, m_mtu4, *this, 4, 0x60, m_intc, 120,
			sh_mtu_channel_device::DIV_1,
			sh_mtu_channel_device::DIV_4,
			sh_mtu_channel_device::DIV_16,
			sh_mtu_channel_device::DIV_64,
			sh_mtu_channel_device::DIV_256,
			sh_mtu_channel_device::DIV_1024,
			sh_mtu_channel_device::INPUT_A,
			sh_mtu_channel_device::INPUT_B);
	SH_PORT32(config, m_porta, *this, 0, 0x00000000, 0xff000000);
	SH_PORT16(config, m_portb, *this, 0, 0x0000, 0xfc00);
	SH_PORT16(config, m_portc, *this, 1, 0x0000, 0x0000);
	SH_PORT32(config, m_portd, *this, 1, 0x0000, 0x0000);
	SH_PORT16(config, m_porte, *this, 2, 0x0000, 0x0000);
	SH_PORT16(config, m_portf, *this, 3, 0x0000, 0xff00);
	SH_SCI(config, m_sci[0], 0, *this, m_intc, 128, 129, 130, 131);
	SH_SCI(config, m_sci[1], 1, *this, m_intc, 132, 133, 134, 135);

}

void sh7042_device::internal_update()
{
	internal_update(current_cycles());
}

void sh7042_device::add_event(u64 &event_time, u64 new_event)
{
	if(!new_event)
		return;
	if(!event_time || event_time > new_event)
		event_time = new_event;
}

void sh7042_device::recompute_timer(u64 event_time)
{
	if(!event_time) {
		m_event_timer->adjust(attotime::never);
		return;
	}

	m_event_timer->adjust(attotime::from_ticks(2*event_time + 1, 2*clock()) - machine().time());
}

TIMER_CALLBACK_MEMBER(sh7042_device::event_timer_tick)
{
	internal_update();
}

void sh7042_device::internal_update(u64 current_time)
{
	u64 event_time = 0;

	add_event(event_time, m_adc0->internal_update(current_time));
	if(m_adc1)
		add_event(event_time, m_adc1->internal_update(current_time));
	add_event(event_time, m_cmt->internal_update(current_time));
	add_event(event_time, m_mtu0->internal_update(current_time));
	add_event(event_time, m_mtu1->internal_update(current_time));
	add_event(event_time, m_mtu2->internal_update(current_time));
	add_event(event_time, m_mtu3->internal_update(current_time));
	add_event(event_time, m_mtu4->internal_update(current_time));
	add_event(event_time, m_sci[0]->internal_update(current_time));
	add_event(event_time, m_sci[1]->internal_update(current_time));

	recompute_timer(event_time);
}

u16 sh7042_device::pcf_ah_r()
{
	return m_pcf_ah;
}

void sh7042_device::pcf_ah_w(offs_t, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_pcf_ah);
	logerror("pcf ah = %04x\n", m_pcf_ah);
}

u32 sh7042_device::pcf_al_r()
{
	return m_pcf_al;
}

void sh7042_device::pcf_al_w(offs_t, u32 data, u32 mem_mask)
{
	COMBINE_DATA(&m_pcf_al);
	logerror("pcf al = %08x\n", m_pcf_al);
}

u32 sh7042_device::pcf_b_r()
{
	return m_pcf_b;
}

void sh7042_device::pcf_b_w(offs_t, u32 data, u32 mem_mask)
{
	COMBINE_DATA(&m_pcf_b);
	logerror("pcf b = %08x\n", m_pcf_b);
}

u16 sh7042_device::pcf_c_r()
{
	return m_pcf_c;
}

void sh7042_device::pcf_c_w(offs_t, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_pcf_c);
	logerror("pcf c = %04x\n", m_pcf_c);
}

u32 sh7042_device::pcf_dh_r()
{
	return m_pcf_dh;
}

void sh7042_device::pcf_dh_w(offs_t, u32 data, u32 mem_mask)
{
	COMBINE_DATA(&m_pcf_dh);
	logerror("pcf dh = %08x\n", m_pcf_dh);
}

u16 sh7042_device::pcf_dl_r()
{
	return m_pcf_dl;
}

void sh7042_device::pcf_dl_w(offs_t, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_pcf_dl);
	logerror("pcf dl = %04x\n", m_pcf_dl);
}

u32 sh7042_device::pcf_e_r()
{
	return m_pcf_e;
}

void sh7042_device::pcf_e_w(offs_t, u32 data, u32 mem_mask)
{
	COMBINE_DATA(&m_pcf_e);
	logerror("pcf e = %08x\n", m_pcf_e);
}

u16 sh7042_device::pcf_if_r()
{
	return m_pcf_if;
}

void sh7042_device::pcf_if_w(offs_t, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_pcf_if);
	logerror("pcf if = %04x\n", m_pcf_if);
}

void sh7042_device::set_internal_interrupt(int level, u32 vector)
{
	m_sh2_state->internal_irq_level = level;
	m_internal_irq_vector = vector;
	m_test_irq = 1;
}

void sh7042_device::sh2_exception_internal(const char *message, int irqline, int vector)
{
	sh2_device::sh2_exception_internal(message, irqline, vector);
	m_intc->interrupt_taken(irqline, vector);
}
