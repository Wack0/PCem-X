#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "ibm.h"

#include "dma.h"
#include "io.h"
#include "pic.h"
#include "timer.h"

#include "fdc.h"
#include "floppy.h"
#include "win-display.h"

int output;
int lastbyte=0;
int sio=0;

uint8_t OLD_BPS = 0;
uint8_t OLD_SPC = 0;
uint8_t OLD_C = 0;
uint8_t OLD_H = 0;
uint8_t OLD_R = 0;
uint8_t OLD_N = 0;

uint8_t flag = 0;
int curoffset = 0;

int tempdiv = 0;
int m3 = 0;

int densel_polarity = 0;
int densel_force = 0;

int fdc_os2 = 0;
int drive_swap = 0;

uint16_t fdcport = 0;

int ps2 = 0;

void configure_from_int(int d, int val)
{
	if (val == 16)
	{
		fdd[d].floppy_drive_enabled = 0;
		return;
	}
	else
	{
		fdd[d].floppy_drive_enabled = 1;
	}

	fdd[d].BIGFLOPPY = ((val) & 8) >> 3;
	fdd[d].DENSITY = ((val) & 6) >> 1;
	fdd[d].THREEMODE = ((val) & 1);
}

int int_from_config(int d)
{
	if (!fdd[d].floppy_drive_enabled)
	{
		return 16;
	}

	int temp = 0;
	temp |= fdd[d].BIGFLOPPY;
	temp <<= 2;
	temp |= fdd[d].DENSITY;
	temp <<= 1;
	temp |= fdd[d].THREEMODE;
	return temp;
}

double byterate()
{
	switch(fdc.rate)
	{
		default:
		case 0:
			return 500.0;
		case 1:
			return 300.0 * 1.02;
		case 2:
			return 250.0 * 1.02;
		case 3:
			return 1000.0 * 1.02;
	}
}

void loaddisc(int d, char *fn)
{
	floppy_load_image(d, fn);
}

uint8_t construct_wp_byte(int d)
{
	return fdd[d].WP | (fdd[d].XDF << 1) | (fdd[d].CLASS << 2) | (fdd[d].LITE << 5);
}

void savedisc(int d)
{
        FILE *f;
        int h,t,s,b;
	int dw;
	// Temporary
        if (!fdd[d].discmodified) return;
        if (fdd[d].WP) return;
	if (fdd[d].XDF)  return;
        f=fopen(discfns[d],"wb");
        if (!f) return;
	if (fdd[d].IMGTYPE == IMGT_NONE)  return;
	if (fdd[d].IMGTYPE != IMGT_RAW)  return;
	if(fdd[d].IMGTYPE == IMGT_PEF)
	{
		putc('P',f);
		putc('C',f);
		putc('e',f);
		putc('m',f);
		putc((uint8_t) fdd[d].SIDES,f);
		putc((uint8_t) fdd[d].TRACKS,f);
		putc((uint8_t) fdd[d].SECTORS,f);
		putc((uint8_t) (fdd[d].BPS >> 7),f);
		putc((uint8_t) construct_wp_byte(d),f);
		putc(0,f);
		putc(0,f);
		putc(0,f);
		putc(0,f);
		putc(0,f);
		putc(0,f);
		putc(0,f);
		if (!fdd[d].LITE)
		{
			curoffset = 16;
        		for (t=0;t<fdd[d].TRACKS;t++)
        		{
                		for (h=0;h<fdd[d].SIDES;h++)
                		{
                        		for (s=0;s<fdd[d].SECTORS;s++)
                        		{
                                        	putc(fdd[d].sstat[h][t][s],f);
						curoffset++;
                        		}
                		}
        		}
			for(t = curoffset; t <= 32767; t++)
			{
				putc(0,f);
			}
		}
	}
	else if (fdd[d].IMGTYPE == IMGT_FDI)
	{
		dw = fdd[d].FDIDATA;
		fwrite(&dw, 1, 8, f);
		dw = fdd[d].RAWOFFS;
		fwrite(&dw, 1, 4, f);
		dw = fdd[d].TOTAL / fdd[d].BPS;
		fwrite(&dw, 1, 4, f);
		dw = fdd[d].BPS;
		fwrite(&dw, 1, 4, f);
		dw = fdd[d].SECTORS;
		fwrite(&dw, 1, 4, f);
		dw = fdd[d].SIDES;
		fwrite(&dw, 1, 4, f);
		dw = fdd[d].TRACKS;
		fwrite(&dw, 1, 4, f);
		for(t = 0x20; t < fdd[d].RAWOFFS; t++)
		{
			putc(0, f);
		}
	}
        for (t=0;t<fdd[d].TRACKS;t++)
        {
                for (h=0;h<fdd[d].SIDES;h++)
                {
                        for (s=0;s<fdd[d].SECTORS;s++)
                        {
				if (fdd[d].scid[h][t][s][3] >= 1)
				{
	                                for (b=0;b<(128 << fdd[d].scid[h][t][s][3]);b++)
	                                {
        	                                putc(fdd[d].disc[h][t][s][b],f);
	                                }
				}
                        }
                }
        }
        fclose(f);
}

/*
	DenSel	BigFloppy	IsHD	Description
	1	1		1	1.6 MB (360 rpm)
	1	1		0	1.0 MB (360 rpm)
					300 rpm drive: Invalid
					360 rpm drive: 1.0 MB (360 rpm)
					Bi-rpm drive: 1.0 MB (360 rpm)
	0	1		1	2.0 MB (300 rpm) (Invalid)
	0	1		0	1.0 MB (300 rpm)
					300 rpm drive: 1.0 MB (300 rpm)
					360 rpm drive: Invalid
					Bi-rpm drive: 1.0 MB (300 rpm)
	1	0		1	2.0 MB (300 rpm)
	1	0		0	Invalid
					Some drives (such as Acer AcerMate 970's drive)
					support 3.5" DD media at 360 rpm
					This might be used for that
	0	0		1	1.6 MB (360 rpm) (Only for 3-mode)
	0	0		0	1.0 MB (300 rpm)

	PS/2 Inverts DENSEL
*/

int densel_by_class()
{
	return (vfdd[fdc.drive].CLASS < CLASS_1600) ? (ps2 ? 1 : 0) : (ps2 ? 0 : 1);
}

int densel_pin()
{
	uint8_t dsel;

	dsel = densel_by_class();

	/* If polarity is set to 1, invert it. */
	if (densel_polarity == 0)  dsel = (dsel ? 0 : 1);

	switch(densel_force)
	{
		case 0:
			/* Return DENSEL as per normal. */
			return dsel;
		case 1:
			/* Reserved, make it behave like normal. */
			return dsel;
		case 2:
			/* Force 1. */
			return 1;
		case 3:
			/* Force 0. */
			return 0;
	}
}

/* This specifies, if 360 rpm mode is enabled. */
int mode3_enabled()
{
	if (vfdd[fdc.drive].BIGFLOPPY)
	{
		/* 360 RPM if DenSel is 1. */
		return densel_pin() ? (!ps2 ? 1 : 0) : (!ps2 ? 0 : 1);
	}
	else
	{
		if (vfdd[fdc.drive].CLASS < CLASS_1600)
		{
			/* 360 RPM if DenSel is 1. */
			return densel_pin() ? (!ps2 ? 1 : 0) : (!ps2 ? 0 : 1);
		}
		else
		{
			/* 360 RPM if DenSel is 0. */
			return densel_pin() ? (!ps2 ? 0 : 1) : (!ps2 ? 1 : 0);
		}
	}
}

int current_rpm(int d)
{
	return (mode3_enabled) ? 360 : 300;
}

void reset_fifo_bus()
{
	int i = 0;
	for (i = 0; i < 16; i++)
	{
		fdc.fifobuf[i] = 0;
	}
	fdc.fifobufpos = 0;
}

void config_default()
{
	int i = 0;

	fdc.dsr = 2;
	fdc.st1 = 0;
	fdc.st2 = 0;
	fdc.dor |= 0xF8;
	fdc.dor &= 0xFC;
	fdc.format_started[0] = 0;
	fdc.format_started[1] = 0;
	fdc.dma = 1;
	fdc.tdr = 0;
	fdc.deldata = 0;
	fdc.fifo = 0;
	fdc.tfifo = 1;
	fdc.gotdata[0] = 0;
	fdc.gotdata[1] = 0;
}

int discint;
void fdc_reset()
{
        fdc.stat=0x80;
        fdc.pnum=fdc.ptot=0;
        fdc.st0=0xC0;
        fdc.lock = 0;
        fdc.head[0] = 0;
        fdc.head[1] = 0;
        fdc.abort[0] = 0;
	fdc.abort[1] = 0;
	fdd[0].rws = 0;
	fdd[1].rws = 0;
        if (!AT)
           fdc.rate=RATE_250K;
}
int ins;

static void fdc_int()
{
        if (!fdc.pcjr && fdc.dma)
                picint(1 << 6);
}

static void fdc_watchdog_poll(void *p)
{
        FDC *fdc = (FDC *)p;
        
        fdc->watchdog_count--;
        if (fdc->watchdog_count)
                fdc->watchdog_timer += 1000 * TIMER_USEC;
        else
        {
//                pclog("Watchdog timed out\n");
        
                fdc->watchdog_timer = 0;
                if (fdc->dor & 0x20)
                        picint(1 << 6);
        }
}

int fdc_fail(int dint)
{
	discint=dint;
	if (dint==0xFE)  return 2;
	disctime = 1024 * (1 << TIMER_SHIFT);
	return 1;
}

int is_hded()
{
	return (vfdd[fdc.drive].DENSITY != DEN_DD);
}

// Rates 0 and 5 should require mode3_enabled being 0 on a 3-mode drive
int fdc_checkrate()
{
	int x = 0;

	if (!vfdd[fdc.drive].floppy_drive_enabled)
	{
		x += fdc_fail(0xFE);		// If floppy drive is disabled, assume drive is empty
		return 0;
	}

	if (discint == 0xFE)  return 0;

	if (vfdd[fdc.drive].CLASS == -1)
	{
		x += fdc_fail(0xFE);		// If rate of inserted disk is invalid, assume drive is empty
		return 0;
	}

	if (discint == 0xFE)  return 0;

	if (vfdd[fdc.drive].SIDES > 2)  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	if ((vfdd[fdc.drive].TRACKS < 60) && (vfdd[fdc.drive].TRACKS > 43))  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	if (vfdd[fdc.drive].TRACKS > 86)  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	if (vfdd[fdc.drive].TOTAL * vfdd[fdc.drive].BPS > (4000 * 1000))  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	if ((vfdd[fdc.drive].CLASS == CLASS_500) && !vfdd[fdc.drive].BIGFLOPPY)  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	switch(fdc.rate)
	{
		case RATE_500K:
			if (vfdd[fdc.drive].DENSITY == DEN_DD)  x += fdc_fail(0xFF);
			if (vfdd[fdc.drive].BIGFLOPPY)
			{
				// Accept 800/1600 if mode 3 is disabled, otherwise accept 1000/2000
				switch(vfdd[fdc.drive].CLASS)
				{
					case CLASS_1600:
						if (!(mode3_enabled()))  x += fdc_fail(0xFF);
						break;
					case CLASS_2000:
						if (!vfdd[fdc.drive].THREEMODE) x += fdc_fail(0xFF);
						if (mode3_enabled())  x += fdc_fail(0xFF);
						break;
					default:
						x += fdc_fail(0xFF);
				}
			}
			else
			{
				// Accept 1000/2000 if mode 3 is disabled, otherwise accept 800/1600
				switch(vfdd[fdc.drive].CLASS)
				{
					case CLASS_1600:
						if (!vfdd[fdc.drive].THREEMODE) x += fdc_fail(0xFF);
						if (!(mode3_enabled()))  x += fdc_fail(0xFF);
						break;
					case CLASS_2000:
						if (mode3_enabled())  x += fdc_fail(0xFF);
						break;
					default:
						x += fdc_fail(0xFF);
				}
			}
			break;
		case RATE_300K:
			if (!vfdd[fdc.drive].DENSITY)  x += fdc_fail(0xFF);
			switch(vfdd[fdc.drive].CLASS)
			{
				case CLASS_500:
					if (!vfdd[fdc.drive].BIGFLOPPY)  x += fdc_fail(0xFE);
					break;
				case CLASS_1000:
					if (!mode3_enabled())  x += fdc_fail(0xFF);
					if (!vfdd[fdc.drive].BIGFLOPPY && vfdd[fdc.drive].THREEMODE)  x += fdc_fail(0xFF);
					break;
				case CLASS_600:
					if (!vfdd[fdc.drive].BIGFLOPPY)  x += fdc_fail(0xFE);
					break;
				case CLASS_1200:
					if (mode3_enabled())  x += fdc_fail(0xFF);
					if (vfdd[fdc.drive].BIGFLOPPY && !vfdd[fdc.drive].THREEMODE)  x += fdc_fail(0xFF);
					break;
				default:
					x += fdc_fail(0xFF);
			}
			break;
		case RATE_250K:
			switch(vfdd[fdc.drive].CLASS)
			{
				case CLASS_400:
					if (!vfdd[fdc.drive].BIGFLOPPY)  x += fdc_fail(0xFE);
					break;
				case CLASS_800:
					if (!vfdd[fdc.drive].DENSITY)  x += fdc_fail(0xFF);
					if (!(mode3_enabled()))  x += fdc_fail(0xFF);
					break;
				case CLASS_500:
					if (!vfdd[fdc.drive].BIGFLOPPY)  x += fdc_fail(0xFE);
					break;
				case CLASS_1000:
					if (vfdd[fdc.drive].BIGFLOPPY && vfdd[fdc.drive].DENSITY && !vfdd[fdc.drive].THREEMODE) x += fdc_fail(0xFF);
					if (mode3_enabled())  x += fdc_fail(0xFF);
					break;
				default:
					x += fdc_fail(0xFF);
			}
			break;
		case RATE_1M:
			if (vfdd[fdc.drive].DENSITY != DEN_ED)  x += fdc_fail(0xFF);
			if (vfdd[fdc.drive].BIGFLOPPY)
			{
				switch(vfdd[fdc.drive].CLASS)
				{
					case CLASS_3200:
						if (!(mode3_enabled()))  x += fdc_fail(0xFF);
						break;
					case CLASS_4000:
						if (!vfdd[fdc.drive].THREEMODE) x += fdc_fail(0xFF);
						if (mode3_enabled())  x += fdc_fail(0xFF);
						break;
					default:
						x += fdc_fail(0xFF);
				}
			}
			else
			{
				// Accept 1000/2000 if mode 3 is disabled, otherwise accept 800/1600
				switch(vfdd[fdc.drive].CLASS)
				{
					case CLASS_3200:
						if (!vfdd[fdc.drive].THREEMODE) x += fdc_fail(0xFF);
						if (!(mode3_enabled()))  x += fdc_fail(0xFF);
						break;
					case CLASS_4000:
						if (mode3_enabled())  x += fdc_fail(0xFF);
						break;
					default:
						x += fdc_fail(0xFF);
				}
			}
			break;
		default:
			x += fdc_fail(0xFF);
			break;
	}

rc_common:
	if (discint == 0xFE)  return 0;

	/* 128-byte sectors are only used for FM floppies, which we do not support. */
	if (vfdd[fdc.drive].BPS < 256)  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	// Don't allow too small floppies on 3.5" drives
	if (!vfdd[fdc.drive].BIGFLOPPY && (vfdd[fdc.drive].TRACKS < 60))  x += fdc_fail(0xFE);

	if (discint == 0xFE)  return 0;

	if (vfdd[fdc.drive].driveempty)  x += fdc_fail(0xFE);

	if (x)  return 0;
	return 1;
}

int fdc_checkparams()
{
	int x = 0;
	// Basically, if with current track number we don't reach above the maximum size for our floppy class
	x = samediskclass(fdc.drive, vfdd[fdc.drive].TRACKS, fdc.params[2], fdc.params[1]);
	if (!x)  fdc_fail(0x101);
	return x;
}

// XDF-aware bytes per sector returner
int real_bps_code()
{
	// Modified so it returns the code from the sector ID rather than the old hardcoded stuff
	return vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][3];
}

// bits 6,7 - only for first sector of each track: 11 = data, 10 = deleted data, 0x = invalid
// bits 5 - only for first sector of each track: 1 = id, 0 = nothing
// bit 2 - data status: 1 = ok, 0 = data error
// bit 1 - id status: 1 = ok, 0 = data error
int sector_state(int read)
{
	uint8_t state = current_state();

	if (ss_good(state))  return 0;

	// Do *NOT* verify the ID address mark on formatting, as that's when it's being written in the first place!
	if (!(ss_idam_present(state)) && (discint != 13))
	{
		fdc.st1 |= 1;
		return 1;
	}
	else
	{
		if (ss_id_crc_present(state) || (discint == 13))
		{
			if (!(ss_id_crc_correct(state)) && (discint != 13))
			{
				fdc.st1 |= 0x20;
				fdc.st2 &= 0xdf;
				return 1;
			}
		}
		else
		{
			fdc.st1 |= 0x20;
			fdc.st2 &= 0xdf;
			return 1;
		}
	}

	if (read)
	{
		if (!(ss_dam_present(state)))
		{
			fdc.st1 |= 1;
			return 1;
		}
		else
		{
			if (ss_data_crc_present(state))
			{
				if (!(ss_data_crc_correct(state)))
				{
					fdc.st1 |= 0x20;
					fdc.st2 |= 0x20;				
					return 1;
				}
			}
			else
			{
				fdc.st1 |= 0x20;
				fdc.st2 |= 0x20;
				return 1;
			}
		}
	}

	if (read == 3)
	{
		if (!fdc.deldata && ss_dam_nondel(state))  fdc.st2 |= 0x40;
		if (fdc.deldata && !(ss_dam_nondel(state)))  fdc.st2 |= 0x40;
	}

	if ((fdc.st1 == 0) && (fdc.st2 == 0))  return 0;

	return 1;
}

int fdc_format()
{
	int b = 0;

	if (OLD_C < 0)
	{
		OLD_C = fdc.params[5];
		OLD_H = fdc.params[6];
		OLD_R = fdc.params[7];
	}
	pclog("ROK!\n");
	fdc.pos[fdc.drive] = 0;

	if ((fdc.params[7] == 0) && !(fdc.params[6] & 0x80))  goto ignore_this_sector;

	/* Make sure the "sector" points to the correct position in the track buffer. */
	vfdd[fdc.drive].disc[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]] = vfdd[fdc.drive].trackbufs[fdc.head[fdc.drive]][fdc.track[fdc.drive]] + ((128 << fdc.params[1]) * (int) vfdd[fdc.drive].spt);
	for (b = 0; b < 4; b++)
	{
		vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]][b] = fdc.params[b + 5];
	}
	vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]][4] = 0xbf;
	if (vfdd[fdc.drive].sectors_formatted == 0)  vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]][4] |= 0x40;

	vfdd[fdc.drive].disc[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]] = (uint8_t *) malloc(128 << fdc.params[b + 8]);

	for(fdc.pos[fdc.drive] = 0; fdc.pos[fdc.drive] < (128 << vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]][3]); fdc.pos[fdc.drive]++)
	{
		vfdd[fdc.drive].disc[fdc.head[fdc.drive]][fdc.track[fdc.drive]][vfdd[fdc.drive].spt[fdc.track[fdc.drive]]][fdc.pos[fdc.drive]] = fdc.fillbyte[fdc.drive];
	}
	// We need to check the state of the current sector before increasing
	vfdd[fdc.drive].sstates += sector_state(0);

	vfdd[fdc.drive].spt[fdc.track[fdc.drive]]++;

ignore_this_sector:
	vfdd[fdc.drive].sectors_formatted++;
	if (fdc.sector[fdc.drive] < vfdd[fdc.drive].temp_spt)  fdc.sector[fdc.drive]++;
	pclog("FDCFMT: %02X %02X %02X %02X | %02X %02X %02X\n", fdc.params[5], fdc.params[6], fdc.params[7], fdc.params[8], vfdd[fdc.drive].sstates, fdc.st1, fdc.st2);
	return 0;
}

int act = 0;

void fifo_buf_write(int val)
{
	if (fdc.fifobufpos < fdc.tfifo)
	{
		fdc.fifobuf[fdc.fifobufpos++] = val;
		if (fdc.fifobufpos == fdc.tfifo)  fdc.fifobufpos = 0;
	}
}

int fifo_buf_read()
{
	int temp = 0;
	if (fdc.fifobufpos < fdc.tfifo)
	{
		temp = fdc.fifobuf[fdc.fifobufpos++];
		if (fdc.fifobufpos == fdc.tfifo)  fdc.fifobufpos = 0;
	}
	return temp;
}

int paramstogo=0;
void fdc_write(uint16_t addr, uint8_t val, void *priv)
{
//        printf("Write FDC %04X %02X %04X:%04X %i %02X %i rate=%i\n",addr,val,cs>>4,pc,ins,fdc.st0,ins,fdc.rate);
	// printf("OUT 0x%04X, %02X\n", addr, val);
        switch (addr&7)
        {
		case 0: /*Configuration*/
			return;
                case 1: /*Change configuration*/
			return;
                case 2: /*DOR*/
                // if (val == 0xD && (cs >> 4) == 0xFC81600 && ins > 769619936) output = 3;
//                printf("DOR was %02X\n",fdc.dor);
                if (fdc.pcjr)
                {
			if (!(val&0x80) && !(fdc.dor&0x80))  return;		
                        if ((fdc.dor & 0x40) && !(val & 0x40))
                        {
                                fdc.watchdog_timer = 1000 * TIMER_USEC;
                                fdc.watchdog_count = 1000;
                                picintc(1 << 6);
//                                pclog("watchdog set %i %i\n", fdc.watchdog_timer, TIMER_USEC);
                        }
                        if ((val & 0x80) && !(fdc.dor & 0x80))
                        {
        			timer_process();
                                disctime = 128 * (1 << TIMER_SHIFT);
                                timer_update_outstanding();
                                discint=-1;
                                fdc_reset();
                        }
                }
                else
                {
			//  If in RESET state, return
			if (!(val&4) && !(fdc.dor&4))  return;
                       	if ((val&4) && !(fdc.dor&4))
                       	{
       				timer_process();
                               	disctime = 128 * (1 << TIMER_SHIFT);
                               	timer_update_outstanding();
                               	discint=-1;
				// printf("Resetting the FDC\n");
                               	fdc.stat=0x80;
                               	fdc.pnum=fdc.ptot=0;
                               	fdc_reset();
                       	}
                }
                fdc.dor=val;
		// if(!(fdc.pcjr))  fdc.dor |= 0x30;
		fdc.dor |= 0x30;
//                printf("DOR now %02X\n",val);
                return;
		case 3:
		if (!(fdc.dor&4))  return;
		if (!(fdc.dor&0x80) && fdc.pcjr)  return;
		fdc.tdr=val&3;
		return;
                case 4:
		if (!(fdc.dor&4))  return;
		if (!(fdc.dor&0x80) && fdc.pcjr)  return;
                if (val & 0x80)
                {
			if (!fdc.pcjr)  fdc.dor &= 0xFB;
			if (fdc.pcjr)  fdc.dor &= 0x7F;
			timer_process();
                        disctime = 128 * (1 << TIMER_SHIFT);
                        timer_update_outstanding();
                        discint=-1;
                        fdc_reset();
			if (!fdc.pcjr) fdc.dor |= 4;
			if (fdc.pcjr) fdc.dor |= 0x80;
                }
		if (val & 0x40)
		{
			timer_process();
			disctime = 128 * (1 << TIMER_SHIFT);
			timer_update_outstanding();
			discint=-1;
			fdc_reset();
		}
                return;
                case 5: /*Command register*/
		if (!(fdc.dor&4) && !fdc.pcjr)  return;		
		if (!(fdc.dor&0x80) && fdc.pcjr)  return;
                if ((fdc.stat & 0xf0) == 0xb0)
                {
			if (!fdc.fifo)
			{
                        	fdc.dat = val;
				fifo_buf_write(val);
                        	fdc.stat &= ~0x80;
			}
			else
			{
				fifo_buf_write(val);
				if (fdc.fifobufpos == 0)  fdc.stat &= ~0x80;
			}
                        break;
                }
                // pclog("Write command reg %i %i\n",fdc.pnum, fdc.ptot);
                if (fdc.pnum==fdc.ptot)
                {
                        fdc.command=val;
                        // printf("Starting FDC command %02X\n",fdc.command);
			fdd[0].sstates = 0;
			fdd[1].sstates = 0;
			fdc.deldata = 0;
			fdc.res[4] = 0;
			fdc.res[5] = 0;
			fdc.res[6] = 0;
                        switch (fdc.command&0x1F)
                        {
                                case 2: /*Read track*/
        	                        fdc.pnum=0;
	                                fdc.ptot=8;
	                                fdc.stat=0x90;
	                                break;
                                case 3: /*Specify*/
	                                fdc.pnum=0;
	                                fdc.ptot=2;
	                                fdc.stat=0x90;
	                                break;
                                case 4: /*Sense drive status*/
	                                fdc.pnum=0;
	                                fdc.ptot=1;
	                                fdc.stat=0x90;
	                                break;
				case 9: /*Write deleted data*/
					fdc.deldata = 1;
                                case 5: /*Write data*/
					if ((fdc.command&0x1F) == 5)  fdc.deldata = 0;
	                                fdc.pnum=0;
	                                fdc.ptot=8;
	                                fdc.stat=0x90;
	                                readflash=1;
	                                break;
				case 12: /*Read deleted data*/
					fdc.deldata = 1;
				case 0x16: /*Verify data*/
                                case 6: /*Read data*/
					if ((fdc.command&0x1F) != 12)  fdc.deldata = 0;
	                                fullspeed();
	                                fdc.pnum=0;
	                                fdc.ptot=8;
	                                fdc.stat=0x90;
	                                readflash=1;
	                                break;
                                case 7: /*Recalibrate*/
	                                fdc.pnum=0;
	                                fdc.ptot=1;
	                                fdc.stat=0x90;
	                                break;
                                case 8: /*Sense interrupt status*/
	                                fdc.lastdrive = fdc.drive;
	                                discint = 8;
	                                fdc_poll();
	                                break;
                                case 10: /*Read sector ID*/
	                                fdc.pnum=0;
	                                fdc.ptot=1;
	                                fdc.stat=0x90;
	                                break;
                                case 13: /*Format*/
	                                fdc.pnum=0;
	                                fdc.ptot=5;
	                                fdc.stat=0x90;
	                                readflash=1;
	                                break;
                                case 15: /*Seek*/
					fdc.relative=fdc.command & 0x80;
					fdc.direction=fdc.command & 0x40;
        	                        fdc.pnum=0;
	                                fdc.ptot=2;
	                                fdc.stat=0x90;
        	                        break;
				case 0x11: /* Scan Equal */
				case 0x19: /* Scan Low or Equal */
				case 0x1D: /* Scan High or Equal */
	                                fdc.pnum=0;
	                                fdc.ptot=8;
	                                fdc.stat=0x90;
        	                        readflash=1;
					break;
                                case 0x0e: /*Dump registers*/
	                                fdc.lastdrive = fdc.drive;
	                                discint = 0x0e;
	                                fdc_poll();
	                                break;
                                case 0x10: /*Get version*/
	                                fdc.lastdrive = fdc.drive;
	                                discint = 0x10;
	                                fdc_poll();
	                                break;
                                case 0x12: /*Set perpendicular mode*/
	                                fdc.pnum=0;
	                                fdc.ptot=1;
	                                fdc.stat=0x90;
	                                break;
                                case 0x13: /*Configure*/
	                                fdc.pnum=0;
	                                fdc.ptot=3;
	                                fdc.stat=0x90;
	                                break;
                                case 0x14: /*Unlock*/
                                case 0x94: /*Lock*/
	                                fdc.lastdrive = fdc.drive;
	                                discint = fdc.command;
        	                        fdc_poll();
	                                break;

                                case 0x18:
	                                fdc.stat = 0x10;
        	                        discint  = 0xfc;
	                                fdc_poll();
        	                        break;

                                default:
	                                pclog("Bad FDC command %02X\n",val);
	                                fdc.stat=0x10;
	                                discint=0xfc;
	                                timer_process();
	                                disctime = 200 * (1 << TIMER_SHIFT);
	                                timer_update_outstanding();
	                                break;
                        }
                }
                else
                {
                        fdc.params[fdc.pnum++]=val;
                        if (fdc.pnum==fdc.ptot)
                        {
                                fdc.stat=0x30;
                                discint=fdc.command&0x1F;
                                timer_process();
				disctime = 1024 * (1 << TIMER_SHIFT);
				if ((discint!=9) && (discint!=12))  fdc.deldata = 0;
				if ((discint!=8) && (discint!=0x12) && (discint!=0x14) && (discint!=0x94) && (discint!=0xE) && (discint!=0x13) && (discint!=3) && (discint!= 0x10) && (discint<=0x16))
				{
					// This is so we make sure fdc.drive isn't changed on commands that don't change it
                                	fdc.drive=fdc.params[0]&1;
					fdc.abort[fdc.drive] = 0;
					fdc.pos[fdc.drive] = 0;
				}
                                if (discint==2 || discint==5 || discint==6 || discint==9 || discint==12 || discint==0x16)
                                {
                                        fdc.track[fdc.drive]=fdc.params[1];
                                        fdc.head[fdc.drive]=fdc.params[2];
                                        fdc.sector[fdc.drive]=fdc.params[3];
                                        fdc.eot[fdc.drive] = fdc.params[5];
                                        if (!fdc.params[5])
                                        {
                                                fdc.params[5]=fdc.sector[fdc.drive];
                                        }
                                        if (fdc.params[5]>vfdd[fdc.drive].spt[fdc.track[fdc.drive]]) fdc.params[5]=vfdd[fdc.drive].spt[fdc.track[fdc.drive]];
                                        if (vfdd[fdc.drive].driveempty)
                                        {
                                                pclog("Drive empty\n");
                                                discint=0xFE;
						goto end_of_dwrite;
                                        }
                                }
                                if (discint==2 || discint==5 || discint==6 || discint==9 || discint==10 || discint==12 || discint==13 || discint==0x16)
                                {
					if ((discint == 13) && (!fdc.format_started[fdc.drive]))
					{
						if (fdc.track[fdc.drive] < 0)  fdc_fail(0x101);
						if (fdc.track[fdc.drive] >= vfdd[fdc.drive].TRACKS)
						{
							if (!samediskclass(fdc.drive, fdc.track[fdc.drive] + 1, fdc.params[2], fdc.params[1]))
							{
								fdc_fail(0x101);
							}
							else
							{
								vfdd[fdc.drive].TRACKS++;
							}
						}
					}
					// Check rate after the format stuff
                                        pclog("Rate %i %i %i at %i RPM (dp %i, df %i, ds %i)\n",fdc.rate,vfdd[fdc.drive].CLASS,vfdd[fdc.drive].driveempty,(mode3_enabled() ? 360 : 300), densel_polarity, densel_force, densel_pin());
					if (discint < 0xFC)  fdc_checkrate();
					if (discint == 0xFE)  pclog("Not ready\n");
					if (discint >= 0xFF)  pclog("Wrong rate\n");
                                }
                                if (discint == 7 || discint == 0xf)
                                {
                                        // fdc.stat = 1 << fdc.drive;
//                                        disctime = 8000000;
                                }
                                if (discint == 0xf || discint == 10)
                                {
                                        fdc.head[fdc.drive] = (fdc.params[0] & 4) ? 1 : 0;
                                }
                                if (discint == 5 && (fdc.pcjr || !fdc.dma))
                                        fdc.stat = 0xb0;
                                if (discint == 9 && (fdc.pcjr || !fdc.dma))
                                        fdc.stat = 0xb0;
                                timer_update_outstanding();
                        }
                }
end_of_dwrite:
                return;
                case 7:
                        if (!AT) return;
	                fdc.rate=val&3;
	                disc_3f7=val;
	                return;
        }
//        printf("Write FDC %04X %02X\n",addr,val);
}

uint8_t fdc_read(uint16_t addr, void *priv)
{
        uint8_t temp;
	uint8_t fdcd;
//        /*if (addr!=0x3f4) */printf("Read FDC %04X %04X:%04X %04X %i %02X %i ",addr,cs>>4,pc,BX,fdc.pos[fdc.drive],fdc.st0,ins);
        switch (addr&7)
        {
		case 0: /*Configuration, index, and status register A*/
		return 0xFF;
		case 1:	/*Data, and status register B*/
		return 0x50;
		case 2:
		temp = fdc.dor;
		if (!AT)  temp = 0xFF;
		if (AT)
		{
			if (!vfdd[0].floppy_drive_enabled)  temp &= 0xEF;
			if (!vfdd[1].floppy_drive_enabled)  temp &= 0xDF;
			temp &= 0x3F;
		}
		break;
                case 3:
		temp = 0x30;
		break;
		if (!AT)
		{
			temp = 0x20;
		}
		else
		{
			temp = fdc.tdr & 3;

			temp |= 0xFC;
		}
                break;
                case 4: /*Status*/
		temp = fdc.stat;
		break;
		temp=fdc.stat;
                break;
                case 5: /*Data*/
                fdc.stat&=~0x80;
                if ((fdc.stat & 0xf0) == 0xf0)
                {
			temp = fifo_buf_read();
			if (fdc.fifobufpos != 0)  fdc.stat |= 0x80;
                        break;
                }
                if (paramstogo)
                {
                        paramstogo--;
                        temp=fdc.res[10 - paramstogo];
                        if (!paramstogo)
                        {
                                fdc.stat=0x80;
                        }
                        else
                        {
                                fdc.stat|=0xC0;
                        }
                }
                else
                {
                        if (lastbyte)
                           fdc.stat=0x80;
                        lastbyte=0;
                        temp=fdc.dat;
                }
                if (discint==0xA) 
		{
			timer_process();
			disctime = 1024 * (1 << TIMER_SHIFT);
			timer_update_outstanding();
		}
                fdc.stat &= 0xf0;
                break;
                case 7: /*Disk change*/
                if (fdc.dor & (0x10 << (fdc.dor & 1)))
                   temp = (vfdd[fdc.dor & 1].discchanged || vfdd[fdc.dor & 1].driveempty)?0x80:0;
                else
                   temp = 0;
                if (AMSTRADIO)  /*PC2086/3086 seem to reverse this bit*/
                   temp ^= 0x80;
                break;
                default:
                        temp=0xFF;
//                printf("Bad read FDC %04X\n",addr);
        }
//        /*if (addr!=0x3f4) */printf("%02X rate=%i\n",temp,fdc.rate);
	// printf("IN 0x%04X, %02X\n", addr, temp);
        return temp;
}

static int fdc_reset_stat = 0;

int return_sstate()
{
	return vfdd[fdc.drive].sstat[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive] - 1];
}

int sds_match()
{
	int d = 1;
	int e = 0;
	if (fdc.deldata)  d = 0;
	e = (ss_dam_nondel(current_state()) ? 1 : 0);
	return (d == e);
}

int real_bps()
{
	return (128 << real_bps_code());
}

uint8_t fdc_seek_by_id(uint32_t chrn, uint8_t *rc, uint8_t *rh, uint8_t *rr)
{
	int ic, ih, ir;
	*rr = 0;
	ic = *(uint8_t *) &chrn;

	for (ih = 0; ih < vfdd[fdc.drive].SIDES; ih++)
	{
		for (ir = 0; ir < vfdd[fdc.drive].spt[fdc.track[fdc.drive]]; ir++)
		{
			if ((*(uint32_t *) vfdd[fdc.drive].scid[ih][ic][ir]) == chrn)
			{
				*rc = ic;
				*rh = ih;
				*rr = ir + 1;
				return 1;
			}
		}
	}
	return 0;
}

int sector_has_id()
{
	return (vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][1] != 255) ? 1 : 0;
}

uint32_t scan_results = 0;
uint32_t satisfying_sectors = 0;

void fdc_readwrite(int mode)
{
	int i = 0;
	int quantity = 1;
	int mt = (fdc.command & 0x80);
	int mfm = (fdc.command & 0x40);
	int sk = (fdc.command & 0x20);
	int ec = (fdc.params[0] & 0x80);
	uint8_t step = (fdc.params[7]);
	int maxs = 0;
	int sc = 0;
	int temp = 0;
	int rbps = 0;
	uint8_t internal_in = 0;
	uint8_t rt = 0;
	uint8_t sr = 0;

	if ((fdc.command & 0x1F) == 2)
	{
		rt = 1;
		mt = 0;
		sk = 0;
	}

	if (mode != 2)  ec = 0;

	if (mode == 0)  sk = 0;

	if (ec)  maxs = fdc.params[7];
	if (ec && !maxs)  maxs = 256;

	if (fdc.fifo)  quantity = fdc.tfifo;

	// Find real sector position in the array by ID
	if (!vfdd[fdc.drive].rws)
	{
		/* If not XDF, sector ID's are normal so we already point at the correct sector. */
		sr = fdc_seek_by_id(*(uint32_t *) &(fdc.params[1]), &(fdc.track[fdc.drive]), &(fdc.head[fdc.drive]), &(fdc.sector[fdc.drive]));
		if (!sr)
		{
			discint = 0xFF;
			fdc_poll();
			return;
		}
		scan_results = 0;
		satisfying_sectors = 0;
		if (fdc.sector[fdc.drive] == 0)
		{
			discint = 0xFF;
			fdc_poll();
			return;
		}
		vfdd[fdc.drive].rws = 1;
	}
	rbps = real_bps();

	if ((mode == 0) && (vfdd[fdc.drive].WP == 2))
	{
		discint=-2;
		disctime=0;
		fdc_poll();
		fdc.stat=0xd0;		// We do need to allow to transfer our parameters to the host!
		fdc.st0=(fdc.head[fdc.drive]?4:0)|fdc.drive;
		fdc.st0|=0x40;
		fdc_int();
		fdc.res[4]=(fdc.head[fdc.drive]?4:0)|(fdc.params[0]&1);
		fdc.res[4] |= 0x40;
		fdc.st0=fdc.res[4];
		fdc.res[5]=2;
		fdc.res[6]=0;
		goto rw_result_phase_after_statuses;
	}
	if (mode == 0)
	{
		vfdd[fdc.drive].discmodified = 1;
	}

	if (fdc.pos[fdc.drive]<rbps)
	{
		for(i = 0; i < quantity; i++)
		{
			if ((mode == 1) || (mode > 3))
			{
				if (!sk || sds_match())
				{
					if (mode == 1)
						fdc.dat = vfdd[fdc.drive].disc[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive] - 1][fdc.pos[fdc.drive]];
					else
						internal_in = vfdd[fdc.drive].disc[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive] - 1][fdc.pos[fdc.drive]];
				}

				if (!sk)
				{
					if (!(sds_match()))  fdc.st2 |= 0x40;
				}
			}

			if (mode != 2)
			{
				if (mode == 1)
				{
					if (!sk || sds_match())
					{
						if (fdc.pcjr || !fdc.dma)
						{
							fdc.stat = 0xf0;
							fifo_buf_write(fdc.dat);
						}
						else
						{
							fdc.stat = 0xd0;
							if (dma_channel_write(2, fdc.dat) & DMA_OVER)
								fdc.abort[fdc.drive] = 1;
						}
					}
				}
				else
				{
					if (fdc.pcjr || !fdc.dma)
					{
						fdc.stat = 0xb0;
						fdc.dat = fifo_buf_read();
					}
					else
					{
						fdc.stat = 0x90;
						temp = dma_channel_read(2);
						fdc.dat = temp;
						if (temp & DMA_OVER)
							fdc.abort[fdc.drive] = 1;
						if (temp == DMA_NODATA)
						{
							fdc.abort[fdc.drive] = 1;
						}
					}
				}
			}
			// I think this is a must to make sure the abort occur before the sector ID is increased
			if (mode == 0)
			{
				if (temp != DMA_NODATA)
				{
					vfdd[fdc.drive].disc[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive] - 1][fdc.pos[fdc.drive]] = fdc.dat;
				}

				// Normal state is 0x43 actually
				if (fdc.deldata)
				{
					vfdd[fdc.drive].sstat[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1] &= 0xBF;
				}
				else
				{
					vfdd[fdc.drive].sstat[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1] |= 0x40;
				}
			}

			if (mode == 3)
			{
				fdc.st2 |= 8;
				if ((internal_in != fdc.dat) && ((internal_in != 0xFF) || !fdc.scan_wildcard))
				{
					fdc.st2 &= 0xF3;
					scan_results = 2;
					goto rw_result_phase;
				}
			}
			else if (mode == 4)
			{
				fdc.st2 |= 8;
				if ((internal_in > fdc.dat) && ((internal_in != 0xFF) || !fdc.scan_wildcard))
				{
					fdc.st2 &= 0xF3;
					scan_results = 2;
					goto rw_result_phase;
				}
				if ((internal_in < fdc.dat) && ((internal_in != 0xFF) || !fdc.scan_wildcard))
				{
					fdc.st2 &= 0xF3;
				}
			}
			else if (mode == 5)
			{
				fdc.st2 |= 8;
				if ((internal_in < fdc.dat) && ((internal_in != 0xFF) || !fdc.scan_wildcard))
				{
					fdc.st2 &= 0xF3;
					scan_results = 2;
					goto rw_result_phase;
				}
				if ((internal_in > fdc.dat) && ((internal_in != 0xFF) || !fdc.scan_wildcard))
				{
					fdc.st2 &= 0xF3;
				}
			}

                       	timer_process();
			disctime = ((mode == 0) ? 600 : 256) * (1 << TIMER_SHIFT);
                       	timer_update_outstanding();

			fdc.pos[fdc.drive]++;
			if(fdc.pos[fdc.drive] >= rbps)
			{
				// We've gone beyond the sector
				// We've gone beyond the sector
				if (mode > 2)
				{
					if (mode == 3)
					{
						if ((fdc.st2 & 0xC) == 8)  satisfying_sectors++;
					}
					else
					{
						if (!(fdc.st2 & 4))
						{
							satisfying_sectors++;
							if (fdc.st2 & 8)
								if (scan_results == 0)  scan_results = 0;
							else
								scan_results = 1;
						}						
					}
					fdc.st2 &= 0xF3;
				}
				
				fdc.pos[fdc.drive] -= rbps;
				if (!sk || sds_match())  vfdd[fdc.drive].sstates += sector_state(mode);
				/* Abort if CRC error is found and command is not read track. */
				if (!rt && sector_state(mode))
					goto rw_result_phase;
				if (mode < 3)
					fdc.sector[fdc.drive]++;
				else
					fdc.sector[fdc.drive] += step;
				sc++;
				if (!(sector_has_id()))
				{
					// We've auto-incremented into a sector without an ID
					// This means we've reached the end of the track
					// Decrease sector and abort
					if (!mt || (mt && (fdc.head[fdc.drive] == (vfdd[fdc.drive].SIDES - 1))))
					{
						fdc.sector[fdc.drive]--;
						goto end_of_track;
					}
				}
				if ((fdc.sector[fdc.drive] > fdc.params[5]) && !ec)
				{
					// Sector is bigger than the limit
					fdc.sector[fdc.drive] = 1;
					if (mt)
					{
						// Multi-track mode
						fdc.head[fdc.drive] ^= 1;
						if ((fdc.head[fdc.drive] == 0) || (vfdd[fdc.drive].SIDES == 1))
						{
							fdc.pos[fdc.drive] = rbps;
							fdc.abort[fdc.drive] = 1;
						}
					}
					else
					{
end_of_track:
						fdc.pos[fdc.drive] = rbps;
						fdc.abort[fdc.drive] = 1;
					}
				}
				if ((fdc.sector[fdc.drive] > vfdd[fdc.drive].spt[fdc.track[fdc.drive]]) && ec)
				{
					// Sector is bigger than the limit
					fdc.sector[fdc.drive] = 1;

					fdc.head[fdc.drive] ^= 1;
					if ((fdc.head[fdc.drive] == 0) || (vfdd[fdc.drive].SIDES == 1))
					{
						fdc.track[fdc.drive]++;
						if (vfdd[fdc.drive].SIDES == 1)  fdc.head[fdc.drive] = 0;
						if (fdc.track[fdc.drive] >= vfdd[fdc.drive].TRACKS)
						{
							pclog("Reached the end of the disk\n");
							fdc.track[fdc.drive] = vfdd[fdc.drive].TRACKS - 1;
							fdc.pos[fdc.drive] = rbps;
							fdc.abort[fdc.drive] = 1;
						}
					}
				}
				if ((sc > maxs) && ec)
				{
					fdc.pos[fdc.drive] = rbps;
					// Make sure we point to the last sector read/written, not to the next
					fdc.abort[fdc.drive] = 1;
				}
				if (fdc.abort[fdc.drive])
				{
					goto rw_result_phase;
				}
			}
rw_break:
			;
		}
		return;
	}
	else
	{
rw_result_phase:
		fdc.abort[fdc.drive] = 0;
		disctime=0;
		discint=-2;
		fdc_int();
		fdc.stat=0xd0;
		fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
		if (vfdd[fdc.drive].sstates)  fdc.res[4] |= 0x40;
		fdc.st0=fdc.res[4];
		fdc.res[5]=fdc.st1;

		if (mode > 2)
		{
			fdc.st2 &= 0xF3;

			switch (scan_results)
			{
				case 0:
					fdc.st2 |= 8;
					break;
				case 1:
					fdc.st2 |= 0;
					break;
				case 2:
					fdc.st2 |= 4;
					break;
			}
		}

		fdc.res[6]=fdc.st2;
rw_result_phase_after_statuses:
		fdc.st1 = 0;
		fdc.st2 = 0;
		fdc.res[7]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][0];
		fdc.res[8]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][1];
		fdc.res[9]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][2];
		fdc.res[10]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][3];
		paramstogo=7;
		vfdd[fdc.drive].rws=0;
		return;
	}
}

void fdc_poll()
{
        int temp;
	int a = 0;
	int n = 0;
	int rbps = 0;
	int maxsteps = 79;
	if (romset == ROM_ENDEAVOR)  maxsteps = 85;
        disctime = 0;
        // pclog("fdc_poll %08X %i %02X\n", discint, fdc.drive, fdc.st0);
        switch (discint)
        {
                case -4: /*End of command with interrupt and no result phase*/
	                fdc_int();
			fdc.stat = 0;
			return;
                case -3: /*End of command with interrupt*/
	                fdc_int();
                case -2: /*End of command*/
	                fdc.stat = (fdc.stat & 0xf) | 0x80;
        	        return;
                case -1: /*Reset*/
	                fdc_int();
	                fdc_reset_stat = 4;
	                return;
                case 2: /*Read track*/
			fdc_readwrite(1);
	                return;
                case 3: /*Specify*/
	                fdc.stat=0x80;
	                fdc.specify[0] = fdc.params[0];
	                fdc.specify[1] = fdc.params[1];
			fdc.dma = 1 - (fdc.specify[1] & 1);
	                return;

                case 4: /*Sense drive status*/
			fdc.res[10] = fdc.params[0] & 7;
	                fdc.res[10] |= 0x28;
	                if (fdc.track[fdc.drive] == 0) fdc.res[10] |= 0x10;
			if (vfdd[fdc.drive].WP) fdc.res[10] |= 0x40;

	                fdc.stat = (fdc.stat & 0xf) | 0xd0;
	                paramstogo = 1;
	                discint = 0;
	                disctime = 0;
	                return;
		case 5:
		case 9:
			fdc_readwrite(0);
			return;
		case 6:
		case 12:
			fdc_readwrite(1);
			return;
		case 0x16:
			fdc_readwrite(2);
			return;
                case 7: /*Recalibrate*/
			if (fdc.track[fdc.drive] <= maxsteps)
			{
				fdc.track[fdc.drive] = 0;
			}
			else
			{
				fdc.track[fdc.drive] -= maxsteps;
				fdc.st0 |= 0x10;
			}
			// fdc.sector[fdc.drive] = 1;
	                if (!vfdd[fdc.drive].driveempty)  vfdd[fdc.drive].discchanged = 0;
			fdc.st0=(fdc.head[fdc.drive]?4:0)|fdc.drive;
			fdc.st0 |= 0x20;
			discint=-3;
	                timer_process();
	                disctime = 2048 * (1 << TIMER_SHIFT);
	                timer_update_outstanding();
        	        fdc.stat = 0x80 | (1 << fdc.drive);
	                return;
                case 8: /*Sense interrupt status*/               
	                fdc.dat = fdc.st0;

	                if (fdc_reset_stat)
        	        {
				fdc.st0 &= 0xf8;
				fdc.st0|=(fdc.head[4 - fdc_reset_stat]?4:0)|(4 - fdc_reset_stat);
	                        fdc_reset_stat--;
	                }

	                fdc.stat    = (fdc.stat & 0xf) | 0xd0;
	                fdc.res[9]  = fdc.st0;
	                fdc.res[10] = fdc.track[fdc.drive];
	                if (!fdc_reset_stat) fdc.st0 = 0x80;

	                paramstogo = 2;
	                discint = 0;
	                disctime = 0;
	                return;
                case 10: /*Read sector ID*/
	                disctime=0;
	                discint=-2;
	                fdc_int();
	                fdc.stat=0xD0;
			fdc.drive = fdc.params[0] & 3;
			fdc.head[fdc.drive] = (fdc.params[0] & 4) ? 1 : 0;
			if (fdc.sector[fdc.drive] <= 0)  fdc.sector[fdc.drive] = 1;
			fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
			/* vfdd[fdc.drive].sstates = sector_state(3);
			if (vfdd[fdc.drive].sstates)  fdc.res[4] |= 0x40;
			if (vfdd[fdc.drive].sstates)  fatal("%c: ID not found\n", 0x41 + fdc.drive); */
			fdc.st0=fdc.res[4];
	                fdc.res[5]=fdc.st1;
	                fdc.res[6]=fdc.st2;
			fdc.st1=0;
			fdc.st2=0;
			fdc.res[7]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][0];
			fdc.res[8]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][1];
			fdc.res[9]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][2];
			fdc.res[10]=vfdd[fdc.drive].scid[fdc.head[fdc.drive]][fdc.track[fdc.drive]][fdc.sector[fdc.drive]-1][3];
			pclog("RSID: %02X %02X %02X %02X %02X %02X %02X\n", fdc.res[4], fdc.res[5], fdc.res[6], fdc.res[7], fdc.res[8], fdc.res[9], fdc.res[10]);
	                paramstogo=7;
	                return;
		case 13: /*Format*/
			if (vfdd[fdc.drive].WP)
			{
	                        disctime=0;
        	                discint=-2;
				fdc.stat=0xd0;		// We do need to allow to transfer our parameters to the host!
				fdc.st0=(fdc.head[fdc.drive]?4:0)|fdc.drive;
				fdc.st0 |= 0x20;
				fdc.st0|=0x40;
	                        fdc_int();
				fdc.res[4]=(fdc.head[fdc.drive]?4:0)|(fdc.params[0]&1);
				fdc.res[4] |= 0x20;
				fdc.res[4] |= 0x40;
				fdc.st0=fdc.res[4];
	                        fdc.st1=2;
	                        fdc.st2=0;
				fdc.res[5]=fdc.st1;
				fdc.res[6]=fdc.st2;
				goto format_result_phase_after_statuses;
			}
			else
			{
        	        	vfdd[fdc.drive].discmodified=1;
			}

			if (!fdc.format_started[fdc.drive])
			{
				if (fdc.command & 0x40)  vfdd[fdc.drive].SIDES=2;
				if (!(fdc.command & 0x40))  vfdd[fdc.drive].SIDES=1;
				vfdd[fdc.drive].temp_bps = fdc.params[1];
				vfdd[fdc.drive].temp_spt = fdc.params[2];
				vfdd[fdc.drive].spt[fdc.track[fdc.drive]] = 0;
				fdc.eot[fdc.drive] = fdc.params[2];
				fdc.sector[fdc.drive] = 1;
				fdc.pos[fdc.drive] = 0;
				fdc.head[fdc.drive] = (fdc.params[0] & 4) >> 2;
				vfdd[fdc.drive].sectors_formatted = 0;
				OLD_BPS = fdc.params[1];
				OLD_SPC = fdc.params[2];
				OLD_C = -1;
				fdc.fillbyte[fdc.drive] = fdc.params[4];
				fdc.format_started[fdc.drive] = 1;
				timer_process();
				disctime = 600 * (1 << TIMER_SHIFT);
				timer_update_outstanding();
				fdc.fdmaread[fdc.drive] = 0;
				vfdd[fdc.drive].sstates = 0;
				freetracksectors(fdc.drive, fdc.head[fdc.drive], fdc.track[fdc.drive]);

				timer_process();
				disctime = 600 * (1 << TIMER_SHIFT);
				timer_update_outstanding();
				if ((fdc.pcjr || !fdc.dma) && (fdc.pos[fdc.drive] <= vfdd[fdc.drive].BPS))  fdc.stat = 0xb0;
				if ((!fdc.pcjr && fdc.dma) && (fdc.pos[fdc.drive] <= vfdd[fdc.drive].BPS))  fdc.stat = 0x90;
				return;
			}
			else
			{
				if (vfdd[fdc.drive].sectors_formatted < vfdd[fdc.drive].temp_spt)
				{
					if (fdc.fifo)
					{
						for (a = 0; a < fdc.tfifo; a++)
						{
							if (a < 4)
							{
								if (fdc.fdmaread[fdc.drive] == 0)  pclog("SST %02X %02X %02X\n", vfdd[fdc.drive].sstates, fdc.sector[fdc.drive], fdc.dma);
	                        				if (fdc.pcjr || !fdc.dma)
								{
									fdc.stat = 0xb0;
	                                				fdc.params[5 + a] = fifo_buf_read();
								}
	                        				else
								{
									fdc.stat = 0x90;
	                                				fdc.params[5 + a] = dma_channel_read(2);
								}
							}
						}
					}
					else
					{
	                        		if (fdc.pcjr || !fdc.dma)
        	                        		if (fdc.fdmaread[fdc.drive] < 4)  fdc.params[5 + fdc.fdmaread[fdc.drive]] = fdc.dat;
	                        		else
	                                		if (fdc.fdmaread[fdc.drive] < 4)  fdc.params[5 + fdc.fdmaread[fdc.drive]] = dma_channel_read(2);
					}

	                        	if (fdc.params[5 + fdc.fdmaread[fdc.drive]] == DMA_NODATA)
	                        	{
						// pclog("DMAND\n");
	                                	discint=0xFD;
	                                	timer_process();
        	                        	disctime = 50 * (1 << TIMER_SHIFT);
                	                	timer_update_outstanding();
	                                	return;
        	                	}
					else
					{
						if (fdc.fifo)
						{
							fdc.fdmaread[fdc.drive]+=fdc.tfifo;
						}
						else
						{
							fdc.fdmaread[fdc.drive]++;
						}
						if (fdc.fdmaread[fdc.drive] < 4)
						{
							timer_process();
							disctime = 600 * (1 << TIMER_SHIFT);
	                               			timer_update_outstanding();
	                               			if ((fdc.pcjr || !fdc.dma) && fdc.fdmaread[fdc.drive] != 4)  fdc.stat = 0xb0;
	                               			if ((!fdc.pcjr && fdc.dma) && fdc.fdmaread[fdc.drive] != 4)  fdc.stat = 0x90;
						}
						else
						{
							timer_process();
							disctime = 128 * (1 << TIMER_SHIFT);
	                               			timer_update_outstanding();
							fdc.sector[fdc.drive] = vfdd[fdc.drive].sectors_formatted + 1;
							fdc_format();
							if (vfdd[fdc.drive].sectors_formatted < vfdd[fdc.drive].temp_spt)
							{
								timer_process();
								disctime = 128 * (1 << TIMER_SHIFT);
	                               				timer_update_outstanding();
								fdc.fdmaread[fdc.drive] = 0;
								if ((fdc.pcjr || !fdc.dma) && (vfdd[fdc.drive].sectors_formatted < vfdd[fdc.drive].temp_spt))  fdc.stat = 0xb0;
								if ((!fdc.pcjr && fdc.dma) && (vfdd[fdc.drive].sectors_formatted < vfdd[fdc.drive].temp_spt))  fdc.stat = 0x90;
							}
						}
					}
				}
				else
				{
					/* Format is finished! */
					pclog("Format finished, track %u with %u sectors!\n", fdc.track[fdc.drive], vfdd[fdc.drive].spt[fdc.track[fdc.drive]]);
	                        	disctime=0;
	                        	discint=-2;
	                        	fdc_int();
	                        	fdc.stat=0xd0;
					fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
					if (vfdd[fdc.drive].sstates)  fdc.res[4] |= 0x40;
					fdc.res[4] |= 0x20;
					fdc.st0=fdc.res[4];
	                        	fdc.res[5]=fdc.st1;
	                        	fdc.res[6]=fdc.st2;
format_result_phase_after_statuses:
					fdc.st1=0;
					fdc.st2=0;
					fdc.res[7]=0;
					fdc.res[8]=0;
					fdc.res[9]=0;
					fdc.res[10]=0;
	                        	paramstogo=7;
					fdc.format_started[fdc.drive] = 0;
	                        	return;
				}
			}
	                return;
                case 15: /*Seek*/
	                if (!fdc.relative)  fdc.track[fdc.drive]=fdc.params[1];
	                if (!vfdd[fdc.drive].driveempty)  vfdd[fdc.drive].discchanged = 0;
			fdc.head[fdc.drive] = (fdc.params[0] & 4) >> 2;
	               	fdc.st0=0x20|fdc.drive|(fdc.head[fdc.drive]?4:0);
			if (fdc.relative)
			{
				if (fdc.direction)  fdc.track[fdc.drive] -= fdc.params[1];
				if (!fdc.direction)  fdc.track[fdc.drive] += fdc.params[1];
				if (fdc.track[fdc.drive] < 0)
				{
					fdc.st0 |= 0x50;
					fdc.track[fdc.drive] += 256;
				}
				fdc.track[fdc.drive] &= 0xFF;
			}
			// Invalidate any other seeks beyond track limit for the current disk
			if (fdc.track[fdc.drive] >= vfdd[fdc.drive].TRACKS)
			{
				fdc.track[fdc.drive] = vfdd[fdc.drive].TRACKS - 1;
			}
			// fdc.sector[fdc.drive] = 1;
			// No result phase = no interrupt
			discint=-3;
	                timer_process();
			disctime = 2048 * (1 << TIMER_SHIFT);
	                timer_update_outstanding();
	                fdc.stat = 0x80 | (1 << fdc.drive);
	                return;
		case 0x11:
			fdc_readwrite(3);
			return;
		case 0x19:
			fdc_readwrite(4);
			return;
		case 0x1D:
			fdc_readwrite(5);
			return;
                case 0x0e: /*Dump registers*/
	                fdc.stat = (fdc.stat & 0xf) | 0xd0;
	                fdc.res[1] = fdc.track[0];
	                fdc.res[2] = fdc.track[1];
	                fdc.res[3] = 0;
	                fdc.res[4] = 0;
	                fdc.res[5] = fdc.specify[0];
	                fdc.res[6] = fdc.specify[1];
	                fdc.res[7] = fdc.eot[fdc.drive];
	                fdc.res[8] = (fdc.perp & 0x7f) | ((fdc.lock) ? 0x80 : 0);
			fdc.res[9] = fdc.config;
			fdc.res[10] = fdc.pretrk;
	                paramstogo=10;
	                discint=0;
	                disctime=0;
        	        return;

                case 0x10: /*Version*/
	                fdc.stat = (fdc.stat & 0xf) | 0xd0;
                	fdc.res[10] = 0x90;
	                paramstogo=1;
	                discint=0;
	                disctime=0;
	                return;
                
                case 0x12:
	                fdc.perp = fdc.params[0];
	                fdc.stat = 0x80;
	                disctime = 0;
	                return;
                case 0x13: /*Configure*/
	                fdc.config = fdc.params[1];
	                fdc.pretrk = fdc.params[2];
			if (fdc.params[1] & 0x20)  fdc.fifo = 0;
			if (!(fdc.params[1] & 0x20))  fdc.fifo = 1;
			fdc.tfifo = (fdc.params[1] & 0xF) + 1;
	                fdc.stat = 0x80;
	                disctime = 0;
	                return;
                case 0x14: /*Unlock*/
			// Reusing the same value for lock
	                fdc.lock = 0;
	                fdc.stat = (fdc.stat & 0xf) | 0xd0;
	                fdc.res[10] = 0;
	                paramstogo=1;
	                discint=0;
	                disctime=0;
	                return;
                case 0x94: /*Lock*/
	                fdc.lock = 1;
	                fdc.stat = (fdc.stat & 0xf) | 0xd0;
	                fdc.res[10] = 0x10;
	                paramstogo=1;
	                discint=0;
	                disctime=0;
	                return;

                case 0xfc: /*Invalid*/
	                fdc.dat = fdc.st0 = 0x80;
	                fdc.stat = (fdc.stat & 0xf) | 0xd0;
	                fdc.res[10] = fdc.st0;
	                paramstogo=1;
	                discint=0;
	                disctime=0;
	                return;

                case 0xFD: /*DMA aborted (PC1512)*/
	                /*In the absence of other information, lie and claim the command completed successfully.
	                  The PC1512 BIOS likes to program the FDC to write to all sectors on the track, but
	                  program the DMA length to the number of sectors actually transferred. Not aborting
	                  correctly causes disc corruption.
	                  This only matters on writes, on reads the DMA controller will ignore excess data.
	                  */
			/*This also happens with regular PC BIOS'es, such as the i430VX, for both reads and
			  writes. Maybe the FDC is supposed to keep reading but stop filling the DMA buffer
			  after it's full? Yes, that is exactly what is supposed to happen. */
	                disctime=0;
	                discint=-2;
	                fdc_int();
        	        fdc.stat=0xd0;
			fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
			fdc.st0=fdc.res[4];
	                fdc.res[5]=0;
	                fdc.res[6]=0;
	                fdc.res[7]=fdc.track[fdc.drive];
	                fdc.res[8]=fdc.head[fdc.drive];
	                fdc.res[9]=fdc.sector[fdc.drive];
	                fdc.res[10]=fdc.params[4];
	                paramstogo=7;
	                return;
                case 0xFE: /*Drive empty*/
	                fdc.stat = 0x10;
	                disctime = 0;
	                return;
                case 0xFF: /*Wrong rate*/
	                fdc.stat = 0x10;
	               	disctime=0;
	               	discint=-2;
	               	fdc_int();
	                fdc.stat=0xd0;
			fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
			fdc.res[4]|=0x40;
			fdc.st0=fdc.res[4];
	               	fdc.res[5]=5;
	               	fdc.res[6]=0;
	               	fdc.res[7]=0;
	               	fdc.res[8]=0;
	               	fdc.res[9]=0;
	               	fdc.res[10]=0;
	               	paramstogo=7;
	                return;

                case 0x100: /*Sector too big or no seek enabled*/
	                fdc.stat = 0x10;
	                disctime=0;
	                discint=-2;
	                fdc_int();
	                fdc.stat=0xd0;
			fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
			fdc.res[4]|=0x40;
			fdc.st0=fdc.res[4];
	                fdc.res[5]=0;
	                fdc.res[6]=0;
	                fdc.res[7]=0;
	                fdc.res[8]=0;
	                fdc.res[9]=0;
	                fdc.res[10]=0;
	                paramstogo=7;
	                return;

                case 0x101: /*Track too big*/
	                fdc.stat = 0x10;
	                disctime=0;
	                discint=-2;
	                fdc_int();
	                fdc.stat=0xd0;
			fdc.res[4]=(fdc.head[fdc.drive]?4:0)|fdc.drive;
			fdc.res[4]|=0x40;
			fdc.st0=fdc.res[4];
	                fdc.res[5]=0x80;
	                fdc.res[6]=0;
	                fdc.res[7]=0;
	                fdc.res[8]=0;
	                fdc.res[9]=0;
	                fdc.res[10]=0;
	                paramstogo=7;
	                return;

		default:
			pclog("Unknown discint %08X issued\n", discint);
			return;
        }
        printf("Bad FDC disc int %i\n",discint);
//        dumpregs();
//        exit(-1);
}

void fdc_init()
{
	timer_add(fdc_poll, &disctime, &disctime, NULL);
	config_default();
	/*
		Setting this to -1 means "do not care, return always 1 for 3.5 inch floppy drives".
		Whatever Super I/O Chip actually cares about this, should set it to 0 or 1 accordingly.
	*/
	// densel_polarity = -1;
	densel_polarity = 1;
	fdc_setswap(0);
}

void fdc_hard_reset()
{
	timer_add(fdc_poll, &disctime, &disctime, NULL);
	config_default();
	densel_polarity = 1;
	fdc_setswap(0);
}

void fdc_add_ex(uint16_t port, uint8_t superio)
{
	pclog("Readding FDC (superio = %u)...\n", superio);
	sio = superio;
	fdcport = port;
	if (superio)
	        io_sethandler(port + 2, 0x0004, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);
	else
	        io_sethandler(port, 0x0006, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);
        io_sethandler(port + 7, 0x0001, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);
        fdc.pcjr = 0;
}

void fdc_add()
{
	fdc_add_ex(0x3f0, 0);
}

void fdc_add_pcjr()
{
        io_sethandler(0x00f0, 0x0006, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);
	timer_add(fdc_watchdog_poll, &fdc.watchdog_timer, &fdc.watchdog_timer, &fdc);
        fdc.pcjr = 1;
}

void fdc_remove_ex(uint16_t port)
{
	pclog("Removing FDC (sio = %u)...\n", sio);
        if (sio)
		io_removehandler(port + 2, 0x0004, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);
	else
		io_removehandler(port, 0x0006, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);
        if (!fdc.pcjr)  io_removehandler(port + 7, 0x0001, fdc_read, NULL, NULL, fdc_write, NULL, NULL, NULL);        
}

void fdc_remove_stab()
{
	/* Remove 0x3F0 (STA) and 0x3F1 (STB) so that a Super I/O Chip can be installed on these two ports. */
	sio = 0;
	fdc_remove_ex(fdcport);
	fdc_add_ex(fdcport, 1);
}

void fdc_remove()
{
	/* Remove both */
	fdc_remove_ex(0x3f0);
	fdc_remove_ex(0x370);
}

FDD fdd[2];
FDD vfdd[2];

void fdc_setswap(int val)
{
	drive_swap = val;
	switch(val)
	{
		case 0:
			vfdd[0] = fdd[0];
			vfdd[1] = fdd[1];
			break;
		case 1:
			vfdd[0] = fdd[1];
			vfdd[1] = fdd[0];
			break;
	}
}
