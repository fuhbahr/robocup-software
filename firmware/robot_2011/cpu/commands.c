#include <usb/device/cdc-serial/CDCDSerialDriver.h>
#include <board.h>
#include <stdio.h>

#include "timer.h"
#include "console.h"
#include "sound.h"
#include "write.h"
#include "spi.h"
#include "tools/reflash.h"
#include "cc1101.h"
#include "status.h"
#include "radio.h"
#include "control.h"
#include "power.h"
#include "main.h"
#include "version.h"
#include "ball_sense.h"

static void cmd_help(int argc, const char *argv[], void *arg)
{
	printf("Commands:\n");
	for (int i = 0; commands[i].name; ++i)
	{
		printf("  %s\n", commands[i].name);
	}
}

static void cmd_reflash(int argc, const char *argv[], void *arg)
{
	unsigned int len;
	
	if (argc != 1)
	{
		printf("Use the host-side reflash script\n");
		return;
	}
	
	music_stop();
	
	len = parse_uint32(argv[0]);
	
	// Set FMR for timing and auto-erase
	AT91C_BASE_MC->MC_FMR = 0x00340100;
	
	printf("GO %08x\n", len);
	
	// Disable interrupts
	AT91C_BASE_AIC->AIC_IDCR = ~0;
	
	// After this point, we're committed to running the reflasher.
	// RAM contents are forfeit.  Only the stack should be considered usable.
	
	//FIXME - Copy reflash to its final location in SRAM.
	//  For now, it's always there thanks to the relocate section, but this wastes memory.
	//  This would be unnecessary if we are running from SRAM to begin with.
	
	// This never returns
	reflash_main(len);
}

//FIXME - This can be smaller
const char *const motor_names[5] =
{
	"BL",
	"FL",
	"FR",
	"BR",
	"DR"
};

// Prints a power supply measurement with a label
static void print_supply(const char *label, int raw)
{
	int supply_mv = raw * VBATT_NUM / VBATT_DIV;
	printf("%s: %d.%03dV\n", label, supply_mv / 1000, supply_mv % 1000);
}

static void cmd_status(int argc, const char *argv[], void *arg)
{
	printf("Robot ");
	if (!controller)
	{
		printf("NOT ");
	}
	printf("running\n");
	
	printf("Robot ID %X\n", robot_id);
	printf("Reset type %x\n", (AT91C_BASE_SYS->RSTC_RSR >> 8) & 7);
	
	printf("Failures: 0x%08x", failures);
	if (failures & Fail_FPGA)
	{
		printf(" FPGA");
	}
	if (failures & Fail_Radio)
	{
		printf(" Radio");
	}
	if (failures & Fail_Power)
	{
		printf(" Power");
	}
	if (failures & Fail_Ball)
	{
		printf(" BallSense");
	}
	putchar('\n');
	
	printf("Power:\n");
	print_supply("  Now", supply_raw);
	print_supply("  Min", supply_min);
	print_supply("  Max", supply_max);
	
	printf("Motor faults: 0x%02x", motor_faults);
	if (motor_faults)
	{
		for (int i = 0; i < 5; ++i)
		{
			if (motor_faults & (1 << i))
			{
				putchar(' ');
				printf(motor_names[i]);
			}
		}
	} else {
		printf(" None");
	}
	putchar('\n');
	
	printf("Encoders:");
	for (int i = 0; i < 4; ++i)
	{
		printf(" 0x%04x", encoder[i]);
	}
	printf("\n");
	printf("   Delta:");
	for (int i = 0; i < 4; ++i)
	{
		printf(" %4d", encoder[i] - last_encoder[i]);
	}
	printf("\n");
	printf(" Command:");
	for (int i = 0; i < 4; ++i)
	{
		printf(" %4d", wheel_command[i]);
	}
	printf("\n");
	printf("  Output:");
	for (int i = 0; i < 4; ++i)
	{
		printf(" %4d", wheel_out[i]);
	}
	printf("\n");
	
	printf("Ball sensor:\n");
	printf("  Light: 0x%03x\n", ball_sense_light);
	printf("  Dark:  0x%03x\n", ball_sense_dark);
	printf("  Delta: 0x%03x\n", ball_sense_light - ball_sense_dark);
	
	printf("GIT version: %s\n", git_version);
}

static void cmd_timers(int argc, const char *argv[], void *arg)
{
	printf("timer_t    time       period\n");
	//      0x01234567 0x01234567 0x01234567
	for (timer_t *t = first_timer; t; t = t->next)
	{
		printf("%p 0x%08x 0x%08x\n", t, t->time, t->period);
	}
}

static void cmd_print_uint32(int argc, const char *argv[], void *arg)
{
	printf("0x%08x\n", *(unsigned int *)arg);
}

static void cmd_spi_test(int argc, const char *argv[], void *arg)
{
	spi_select(NPCS_FLASH);

	spi_xfer(0xab);
	spi_xfer(0);
	spi_xfer(0);
	spi_xfer(0);
	printf("Signature: 0x%02x\n", spi_xfer(0));
	spi_deselect();
	
	spi_xfer(0x05);
	printf("Status:    0x%02x\n", spi_xfer(0));
	spi_deselect();
}

static int spi_wait(int max)
{
	spi_xfer(0x05);
	for (unsigned int  start_time = current_time; (current_time - start_time) < max;)
	{
		// Reset the watchdog timer
		AT91C_BASE_WDTC->WDTC_WDCR = 0xa5000001;
		
		uint8_t status = spi_xfer(0);
		if (!(status & 1))
		{
			spi_deselect();
			return 1;
		}
	}
	printf("*** Timeout!\n");
	spi_deselect();
	return 0;
}

static void cmd_spi_erase(int argc, const char *argv[], void *arg)
{
	spi_select(NPCS_FLASH);
	
	// Bulk erase
	// WREN
	spi_xfer(0x06);
	spi_deselect();

	// BE
	spi_xfer(0xc7);
	spi_deselect();

	// Wait for completion
	spi_wait(4000);
}

static void cmd_spi_write(int argc, const char *argv[], void *arg)
{
	uint32_t addr, len;
	int rx_pos;
	
	if (argc != 2)
	{
		printf("spi_write <address> <length>\n");
		return;
	}
	
	addr = parse_uint32(argv[0]);
	len = parse_uint32(argv[1]);
	
	usb_rx_len = 0;
	rx_pos = 0;
	
	// Program one page at a time
	spi_select(NPCS_FLASH);
	while (len)
	{
		// WREN
		spi_xfer(0x06);
		spi_deselect();
		
		// PP
		spi_xfer(0x02);
		spi_xfer(addr >> 16);
		spi_xfer(addr >> 8);
		spi_xfer(addr);
		
		// Program up to the end of the page or the end of the data
		do
		{
			// Wait for more data if necessary
			if (rx_pos == usb_rx_len)
			{
				usb_rx_start();
				while (!usb_rx_len);
				rx_pos = 0;
			}
			
			spi_xfer(usb_rx_buffer[rx_pos++]);
			++addr;
			--len;
		} while ((addr & 0x7f) && len);
		
		spi_deselect();
		
		if (!spi_wait(5))
		{
			return;
		}
	}
	//FIXME - I don't like how this fucntion handles input.
	//  There should be an input buffer.  Currently we depend on there not being extra input.
	usb_rx_len = 0;
	printf("OK\n");
}

static void cmd_spi_read(int argc, const char *argv[], void *arg)
{
	uint32_t addr, len;
	
	if (argc != 2)
	{
		printf("spi_read <address> <length>\n");
		return;
	}
	
	addr = parse_uint32(argv[0]);
	len = parse_uint32(argv[1]);
	
	spi_select(NPCS_FLASH);
	spi_xfer(0x03);
	spi_xfer(addr >> 16);
	spi_xfer(addr >> 8);
	spi_xfer(addr);
	
	for (; len; --len)
	{
		putchar_raw(spi_xfer(0));
	}
	flush_stdout();
	
	spi_deselect();
}

static void cmd_fpga_test(int argc, const char *argv[], void *arg)
{
	spi_select(NPCS_FPGA);
	
	for (int i = 0; i < argc; ++i)
	{
		uint8_t byte = parse_uint32(argv[i]);
		printf("0x%02x\n", spi_xfer(byte));
	}
	
	spi_deselect();
}

static void cmd_fpga_on(int argc, const char *argv[], void *arg)
{
	switch (fpga_init())
	{
		case 0:
			music_start(song_failure);
			printf("*** Failed\n");
			break;
		
		case 1:
			music_start(song_startup);
			printf("Configured\n");
			break;
		
		case 2:
			printf("Already configured\n");
			break;
	}
}

static void cmd_fpga_reset(int argc, const char *argv[], void *arg)
{
	AT91C_BASE_PIOA->PIO_CODR = MCU_PROGB;
	delay_ms(1);
	cmd_fpga_on(0, 0, 0);
}

static void cmd_read_word(int argc, const char *argv[], void *arg)
{
	if (argc != 1)
	{
		printf("rw <addr>\n");
		return;
	}
	
	uint32_t addr = parse_uint32(argv[0]);
	printf("0x%08x\n", *(unsigned int *)addr);
}

static void cmd_write_word(int argc, const char *argv[], void *arg)
{
	if (argc != 2)
	{
		printf("ww <addr> <value>\n");
		return;
	}
	
	uint32_t addr = parse_uint32(argv[0]);
	uint32_t value = parse_uint32(argv[1]);
	*(uint32_t *)addr = value;
}

static void cmd_radio_start(int argc, const char *argv[], void *arg)
{
	radio_command(SIDLE);
	radio_command(SFRX);
	radio_command(SRX);
}

static void cmd_last_rx(int argc, const char *argv[], void *arg)
{
 	printf("RSSI %d dBm\n", (int)last_rssi / 2 - 74);
	
	printf("%02x %02x %02x\n", forward_packet[0], forward_packet[1], forward_packet[2]);
	for (int i = 0; i < 5; ++i)
	{
		int off = 3 + i * 5;
		printf("%02x %02x %02x %02x %02x\n",
			   forward_packet[off],
			   forward_packet[off + 1],
			   forward_packet[off + 2],
			   forward_packet[off + 3],
			   forward_packet[off + 4]);
	}
	putchar('\n');
}

static void cmd_stfu(int argc, const char *argv[], void *arg)
{
	if (argc)
	{
		power_music_disable = parse_uint32(argv[0]);
	} else {
		power_music_disable = 1;
	}
	
	// Stop any music that may be playing
	music_stop();
}

static const note_t *const test_songs[] = {song_startup, song_failure, song_overvoltage, song_undervoltage, song_fuse_blown};
#define NUM_TEST_SONGS (sizeof(test_songs) / sizeof(test_songs[0]))

static void cmd_music(int argc, const char *argv[], void *arg)
{
	int n;
	if (argc != 1 || (n = parse_uint32(argv[0])) >= NUM_TEST_SONGS)
	{
		printf("music <0-%d>\n", (int)NUM_TEST_SONGS - 1);
		return;
	}
	
	music_start(test_songs[n]);
}

static void cmd_tone(int argc, const char *argv[], void *arg)
{
	AT91C_BASE_PWMC->PWMC_DIS = 1 << 3;
	if (argc == 1)
	{
		int period = PERIOD(parse_uint32(argv[0]));
		AT91C_BASE_PWMC->PWMC_CH[3].PWMC_CPRDR = period;
		AT91C_BASE_PWMC->PWMC_CH[3].PWMC_CDTYR = period / 2;
		AT91C_BASE_PWMC->PWMC_ENA = 1 << 3;
		printf("%d\n", period);
	}
}

static void cmd_fail(int argc, const char *argv[], void *arg)
{
	if (argc != 1)
	{
		printf("fail <flags>\n");
		return;
	}
	
	failures = parse_uint32(argv[0]);
}

static void cmd_adc(int argc, const char *argv[], void *arg)
{
	printf("0x%08x\n", AT91C_BASE_ADC->ADC_CHSR);
	printf("0x%08x\n", AT91C_BASE_ADC->ADC_MR);
	printf("0x%08x\n", AT91C_BASE_ADC->ADC_SR);
	for (int i = 0; i < 8; ++i)
	{
		if (i == 3)
		{
			// PA20 is not assigned to AD3
			continue;
		}
		printf("%d: 0x%03x\n", i, adc[i]);
	}
	
	if (argc)
		AT91C_BASE_ADC->ADC_CR = AT91C_ADC_START;
}

static void cmd_run(int argc, const char *argv[], void *arg)
{
	printf("FIXME\n");
}

static const write_int_t write_fpga_off = {&AT91C_BASE_PIOA->PIO_CODR, MCU_PROGB};
static const write_int_t write_reset = {AT91C_RSTC_RCR, 0xa5000005};

const command_t commands[] =
{
	{"help", cmd_help},
	{"status", cmd_status},
	{"reflash", cmd_reflash},
	{"reset", cmd_write_int, (void *)&write_reset},
	{"rw", cmd_read_word},
	{"ww", cmd_write_word},
	{"stfu", cmd_stfu},
	{"run", cmd_run},
	{"time", cmd_print_uint32, (void *)&current_time},
	{"inputs", cmd_print_uint32, (void *)&AT91C_BASE_PIOA->PIO_PDSR},
	{"timers", cmd_timers},
	{"fpga_reset", cmd_fpga_reset},
	{"fpga_off", cmd_write_int, (void *)&write_fpga_off},
	{"fpga_on", cmd_fpga_on},
	{"fpga_test", cmd_fpga_test},
	{"spi_test", cmd_spi_test},
	{"spi_erase", cmd_spi_erase},
	{"spi_write", cmd_spi_write},
	{"spi_read", cmd_spi_read},
	{"radio_configure", (void *)radio_configure},
	{"radio_start", cmd_radio_start},
	{"last_rx", cmd_last_rx},
	{"music", cmd_music},
	{"tone", cmd_tone},
	{"fail", cmd_fail},
	{"adc", cmd_adc},

	// End of list placeholder
	{0, 0}
};
