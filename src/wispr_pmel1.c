/*
 * wispr_pmel1.c
 *
 * - Intermittent data acquisition where adc samples are acquired continuously for a finite time window 
 *   then the system goes to sleep. The system wakes up and reboots after a specified amount of time 
 *   or is waken up by detecting an input on one of the serial ports.
 * - Data is logged to flat binary data files on the exFat formatted SD card. 
 * - No data headers are written to the binary files.
 * - Data buffer header info is written to a separate text file (.txt) with the same name as the data files. 
 * - To optimize memory usage, the sample size and buffer size are fixed.
 * - The fft size for spectral analysis is also fixed.
 * - The user is prompted of other data collection parameters when the system is reset by the user.
 *
 * Support and FAQ: visit <a href="http://www.atmel.com/design-support/">Atmel Support</a>
 */
#include <asf.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "wispr.h"
#include "wispr_config.h"
#include "board.h"
#include "sd_card.h"
#include "ltc2512.h"
#include "console.h"
#include "rtc_time.h"
#include "uart_queue.h"
#include "gps.h"
#include "i2c.h"
#include "pcf2129.h"
#include "ina260.h"
#include "pcf8574.h"
#include "pps_timer.h"
#include "ff.h"
#include "com.h"

#include "spectrum.h"
#include "arm_math.h"
#include "arm_const_structs.h"

#include "spectrum.h"
#include "pmel.h"

// Allocate fixed size buffers
// Using the COMPILER_WORD_ALIGNED macro will avoid any memory alignment problem,
// although the compiler will still give the warning.

COMPILER_WORD_ALIGNED uint8_t adc_buffer[ADC_BUFFER_SIZE+4];
//uint8_t *adc_data = adc_buffer; 
wispr_data_header_t adc_header;

//COMPILER_WORD_ALIGNED uint8_t psd_buffer[PSD_BUFFER_SIZE+4];
//float32_t *psd_data = (float32_t *)&psd_buffer[0];
COMPILER_WORD_ALIGNED float32_t psd_buffer[PSD_MAX_NUM_BINS+1];
wispr_data_header_t psd_header;

// cumulative psd average 
COMPILER_WORD_ALIGNED float32_t psd_average[PSD_MAX_NUM_BINS+1];

// set this to a nonzero value only for testing - this will force card swaps 
//uint32_t test_sd_card_nblocks = 300 * ADC_MAX_BLOCKS_PER_BUFFER;
uint32_t test_sd_card_nblocks = 0;

// local function prototypes
void log_buffer_to_file(wispr_config_t *config, fat_file_t *ff, uint8_t *buffer, uint16_t nblocks, char *type);
void go_to_sleep(wispr_config_t *config);
uint8_t swap_sd_cards(wispr_config_t *config);
int initialize_sd_cards(wispr_config_t *config);
void initialize_config(wispr_config_t *config);
void change_gain(wispr_config_t *config);
uint32_t initialize_datetime(void);
uint32_t initialize_datetime_with_gps(void);
uint32_t sync_adc_start_with_new_file(wispr_config_t *config, fat_file_t *ff);
void handle_data_buffer(wispr_config_t *config, uint8_t *buffer, uint16_t nsamps);
void process_spectrum(wispr_config_t *config, uint8_t *buffer, uint16_t nsamps);

// local variables
fat_file_t dat_file;
fat_file_t psd_file;
pmel_control_t pmel;

char config_filename[] = "wispr1.txt";

uint32_t start_sec = 0;

//
// main
//
int main (void)
{
	wispr_config_t wispr; // current wispr configuration

	// initialize global variables
	wispr.active_sd_card = 0; // no active sd card number yet
	dat_file.state = SD_FILE_CLOSED;
	psd_file.state = SD_FILE_CLOSED;
	
	strcpy(pmel.location_id, "PGEN04");
	strcpy(pmel.instrument_id, "PACW01");
	pmel.version[0] = 1;
	pmel.version[1] = 0;
	
	// initialize the board specific functions (clocks, gpio, console, wdt, ...)
	// returns the reason the board was last reset (user, sleep, watchdog, ...)
	int reset_type = board_init();

	// Initialize the WDT
	// Once the WDT starts it can't be stopped, Only a processor reset resets it.
	// So make sure wdt_restart() is called regularly. wdt_disable() doesn't stop the wdt.
	uint32_t msecs = board_wdt_init();
	printf("Enable watchdog with %d msec period\n\r", msecs);

	// init I2C bus for the rtc and gpio
	i2c_init(TWI0, WISPR_I2C_SPEED);
	
	// Setup the External RTC
	// This supplies the 32k clock to the internal rtc and a 1Hz pps
	if( pcf2129_init() != STATUS_OK ) {
		printf("Error initializing RTC\r\n");
	}

	rtc_time_t dt;
	pcf2129_get_datetime(&dt);  // read back time
	
	// if time is not valid, set a default time
	uint32_t status = rtc_valid_datetime(&dt);
	if( status != RTC_STATUS_OK ) {
		printf("\r\nInvalid System Time: %02d/%02d/%02d %02d:%02d:%02d\r\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
		dt.year = 21; dt.month = 1; dt.day=1; dt.hour=1; dt.minute=0; dt.second=0;
		pcf2129_set_datetime(&dt);  // read back time
	}
		
	// initialize the uart com communications port
	status = com_init(BOARD_COM_PORT, BOARD_COM_BAUDRATE);

	// start the pps timer and synchronize the rtc with the pps input
	if ( pps_timer_init() != RTC_STATUS_OK ) {
		printf("RTC Sync failed\r\n");
	}

	//pps_timer_calib();

	// synchronize the rtc initialization with the pps
	//pps_timer_sync( initialize_datetime_with_gps );
	if ( pps_timer_sync( initialize_datetime ) != RTC_STATUS_OK ) {
		printf("RTC Sync failed\r\n");
	}
	
	// check all sd cards and configure them if needed
	// this also sets the active sd card number 
	FRESULT res = initialize_sd_cards(&wispr);
	if( res != FR_OK) {
		printf("SD Cards failed to initialize: error %d\n\r", res);
		return(0);
	}
	
	// read the configuration from the active sd card 
	printf("\r\nWriting to ");
	sd_card_print_info(wispr.active_sd_card);

	// if user reset then prompt to change/initialize config
	// skip if reset from backup because there typically will be no user at the console
	// this means that the configuration can only be changed after a user reset or power up
	if( reset_type != BOARD_BACKUP_RESET ) {
		initialize_config(&wispr);
	}

	// Define/redefine the variables that control the window and interval timing.
	// Some are fixed and some are variables
	wispr.adc.sample_size = ADC_SAMPLE_SIZE; // fixed
	wispr.adc.buffer_size = ADC_BLOCKS_PER_BUFFER * WISPR_SD_CARD_BLOCK_SIZE; // fixed
	wispr.adc.samples_per_buffer = wispr.adc.buffer_size / wispr.adc.sample_size;

	uint16_t adc_samples_per_buffer = wispr.adc.samples_per_buffer;
	float adc_buffer_duration = (float)wispr.adc.samples_per_buffer / (float)wispr.adc.sampling_rate; // seconds
	uint16_t blocks_per_file = wispr.file_size;
	float file_duration = (float)blocks_per_file * adc_buffer_duration / (float)ADC_BLOCKS_PER_BUFFER; // seconds

	// since the adc buffer duration is defined by a fixed number of blocks
	// the actual sampling window may be different than the requested
	//float actual_sampling_time = (float)adc_buffers_per_file * adc_buffer_duration;

	printf("New data file created every %f seconds with %d blocks\r\n", file_duration, blocks_per_file);

	// save the updated config
	sd_card_fwrite_config(config_filename, &wispr);

	// save the config because it could have changed
	wispr_config_print(&wispr);
	
	// setup INA260 power monitor
	ina260_init(INA260_CONFIG_MODE_TRIGGERED | INA260_CONFIG_AVG_1024 | INA260_CONFIG_CT_1100us, INA260_ALARM_NONE, 0);
	// send a PWR com message every second
	//ina260_init(INA260_CONFIG_MODE_CONTINUOUS|INA260_CONFIG_AVG_1024|INA260_CONFIG_CT_1100us, INA260_ALARM_CONVERSION_READY, 1);
	float32_t volts; // Volts
	float32_t amps;   // mAmps
	ina260_read(&amps, &pmel.volts, 1);
	printf("Supply Voltage: %.2f V\r\n", pmel.volts);	

	// gpio control example
	//uint8_t gpio = 0xFF;
	//pcf8574_write(gpio);
	
	// Initialize spectrum
	//wispr.psd_nbins = wispr.psd.size / 2;
	//psd_nblocks_per_buffer = wispr.psd_nbins * PSD_SAMPLE_SIZE / WISPR_SD_CARD_BLOCK_SIZE;
	//spectrum_init_q31(&wispr.psd_nbins, wispr.psd.size, wispr.psd.overlap, wispr.psd.window_type);
	
	// turn on power to adc board
	ioport_set_pin_level(PIN_ENABLE_5V5, 1); 
	
	//printf("\n\rStart data acquisition for %.3f seconds (%d buffers)\n\r", actual_sampling_time, wispr.buffers_per_window);
	printf("\n\rStart data acquisition\n\r");
	
	// initialize the adc with the current config
	ltc2512_init(&wispr.adc, &adc_header);
	
//	int hdr_size = pmel_file_header((char *)adc_buffer, &wispr, &adc_header);
//	if( sd_card_fwrite(&dat_file, adc_buffer, 1) != FR_OK ) {
//		printf("Error writing to file: %s\r\n", dat_file.name);
//	}
	//printf("File Header size %d bytes\r\n", hdr_size);
	
	// start adc - this starts the receiver and conversion clock, but doesn't trigger the adc
	ltc2512_start();

	// Trigger the adc by syncing with the pps timer.
	// This will call ltc2512 trigger function on the next pps rising edge.
	start_sec = sync_adc_start_with_new_file(&wispr, &dat_file);
	//start_sec = pps_timer_sync( ltc2512_trigger );
	//start_sec = ltc2512_trigger();

	// used for state transitions
	wispr.state = WISPR_ACTIVE;
	uint8_t prev_state = wispr.state;

	// loop over adc read buffers
	uint16_t count = 0;
	uint8_t go = 1;
	while ( go ) {
				
		// check for a com message, no wait timeout
		prev_state = wispr.state;
		if(  pmel_control(&wispr, 0) ) {
			printf("pmel control: state %d, mode %d\r\n", wispr.state, wispr.mode);
		}

		// wakeup from sleep
		if( (wispr.state & WISPR_ACTIVE) && (prev_state & WISPR_SLEEP_WFI) ) {
			start_sec = pps_timer_sync( ltc2512_trigger );
		}

		// active reading and logging mode
		if( wispr.state & WISPR_ACTIVE ) {
		
			// read the current buffer. If a new buffer is not ready read returns 0
			uint16_t nsamps = ltc2512_read_dma(&adc_header, adc_buffer);
		
			// if a new buffer is available
			if( nsamps == adc_samples_per_buffer ) {
			
				// reset the wdt every time a buffer is read
				wdt_restart(WDT);
			
				handle_data_buffer(&wispr, adc_buffer, nsamps);
							
				count++;	// increment buffer count

			}
			
			// sleep between buffers, the next interrupt will wake from sleep
			pmc_sleep(SAM_PM_SMODE_SLEEP_WFI); // sleep until next interrupt
		
		}

		// Paused - is this the same as WFI Sleep		
		if( wispr.state == WISPR_PAUSED ) {
			ltc2512_stop();
			ltc2512_stop_dma();
			pmc_sleep(SAM_PM_SMODE_SLEEP_WFI); // sleep until next interrupt		
		}
		
		// Sleep Waiting For Interrupt
		// highest level sleep mode, clocks are still running
		if( wispr.state == WISPR_SLEEP_WFI ) {
			ltc2512_stop();
			ltc2512_stop_dma();
			pmc_sleep(SAM_PM_SMODE_SLEEP_WFI); // sleep until next interrupt			
		}

		// Sleep in backup mode (deep sleep)
		// lowest level sleep mode, reset/wakeup on defined inputs such as UART or GPIO input
		if( wispr.state == WISPR_SLEEP_BACKUP ) { 
			go = 0;
		}
	
	}

	// shutdown the adc
	ltc2512_stop();
	ltc2512_shutdown();
	ltc2512_stop_dma();
	ioport_set_pin_level(PIN_ENABLE_5V5, 0);
	
	// make sure to close the data file, otherwise it may be lost
	sd_card_fclose(&dat_file);
	sd_card_fclose(&psd_file);
		
	// save the latest config
	// update the config time so the last active card number can be determined on wakeup
	sd_card_fwrite_config(config_filename, &wispr);
	
	// close active sd card
	sd_card_unmount(wispr.active_sd_card);
	
	// save config, close the active card, and disable all the sd cards
	sd_card_disable(1);
	sd_card_disable(2);

	// shutdown sets HSMCI pins as inputs to prevent problems when the card is powered down
	sd_card_shutdown();
	
	// stop the pps timer
	pps_timer_stop();
	
	ina260_stop();

	// go to deep sleep 
	if( wispr.sleep_time > 0 ) {

		// for testing only
		//printf("nbins = %d\r\n", psd_nbins);
		//printf("psd = [\r\n");
		//for(int n = 0; n < psd_nbins; n++) {
		//	printf("%f ", psd_data[n]);
		//}
		//printf("];\r\n");				

		// enter backup mode sleep
		// The core will reset when exiting from backup mode.
		go_to_sleep(&wispr);
		
	}
	
	printf("Finished\n\r");
	
	exit(0);
}

void handle_data_buffer(wispr_config_t *config, uint8_t *buffer, uint16_t nsamps) 
{
				
	// debug gpio
	ioport_set_pin_level(PIN_PB10, 1);

	//for(int n = 0; n < 8; n++) printf("%x%x ", buffer[n*2+1], buffer[n*2]);
	//printf("\r\n");

	//write the waveform to the active sd card
	if( config->mode & WISPR_WAVEFORM ) {
	
		// log the adc buffer to file
		log_buffer_to_file(config, &dat_file, buffer, ADC_BLOCKS_PER_BUFFER, "dat");

	}
	
	//printf("usec = %d, delta = %d\r\n", adc_header.usec, adc_header.usec-prev_usec);
	//prev_usec = adc_header.usec;
	
	// process the averaged spectrum 
	if( config->mode & WISPR_SPECTRUM ) {
		
		process_spectrum(config, buffer, nsamps);

	}
				
	ioport_set_pin_level(PIN_PB10, 0);
	
}

//
// Calculate the spectrum of the data buffer and add it to the running psd estimate
// When the specified number of buffers have been processed, transmit the spectrum in pmel format
//
void process_spectrum(wispr_config_t *config, uint8_t *buffer, uint16_t nsamps)
{
	wispr_psd_t *psd = &config->psd;
	uint16_t nbins = config->psd.nbins;
	uint16_t navg = config->psd.navg;
	uint16_t count = config->psd.count;
	uint16_t n = 0;
	
	if( count < navg ) {
	
		// start the psd average
		if( count == 0 ) {
			spectrum_init_q31(&psd->nbins, psd->size, psd->overlap, psd->window_type);
			for(n = 0; n < nbins; n++) psd_average[n] = 0.0;
			psd->second = adc_header.second;  // start time
		}
		
		// call spectrum function
		spectrum_q31(&psd_header, psd_buffer, &adc_header, buffer, nsamps);
		//spectrum_f32(&psd_header, psd_buffer, &adc_header, adc_buffer, adc_nsamps);
		
		// accumulate psd average
		for (n = 0; n < nbins; n++) {
			psd_average[n] += psd_buffer[n];
			//psd_average[n] = psd_buffer[n];
		}
		
		psd->count = count + 1;
		
		//printf("PSD: count=%d\r\n", config->psd_count);

	}
	
	// finish the running psd average
	if( count == navg ) {
		
		float32_t norm = 1.0 / (float32_t)navg;
		for(n = 0; n < nbins; n++) psd_average[n] *= norm;

		// pause the adc to prevent data buffer overruns while transmitting the spectrum
		// this means that there will be a small data gap while the spectrum is being transmitted
		ltc2512_pause();
		
		// send the spectrum
		pmel_transmit_spectrum(config, psd_average, nbins, buffer, &pmel);
		
		// trigger the adc start to with the pps so it starts at a know time
		start_sec = pps_timer_sync( ltc2512_trigger );
		
		// clear the flags to stop spectrum processing
		psd->count = 0;
		config->mode &= ~WISPR_SPECTRUM;
		
		//printf("PSD Done: count=%d\r\n", config->psd.count);
		
	}

	// log the psd buffer to file
	//log_buffer_to_file(&wispr, &psd_file, (uint8_t *)psd_buffer, PSD_BLOCKS_PER_BUFFER, "dat");
	
	//if( sd_card_fwrite(&psd_file, psd_buffer, PSD_BLOCKS_PER_BUFFER) != FR_OK ) {
	//	printf("Error writing to file: %s\r\n", psd_file.name);
	//}
	
}

uint32_t sync_adc_start_with_new_file(wispr_config_t *config, fat_file_t *ff)
{
	char filename[32];
	uint32_t epoch;
	rtc_time_t dt;

	// create filename with the current time
	rtc_get_epoch( &epoch );
	epoch = epoch + 1; // add a second because adc will start on the next pps
	epoch_to_rtc_time(&dt, epoch+1);
	pmel_filename(filename, "WISPR", "dat", &dt);

	// check if active sd card is full
	if( sd_card_is_full(config->active_sd_card, ADC_BLOCKS_PER_BUFFER+1) == SD_CARD_FULL ) {
		// close all log files
		sd_card_fclose(&dat_file);
		printf("Card %d is full: %d\r\n", config->active_sd_card);
		// toggle between the available sd cards
		swap_sd_cards(config);
	}
		
	if( sd_card_fopen(ff, filename, FA_OPEN_ALWAYS | FA_WRITE, config->active_sd_card) == FR_OK ) {
		printf("Open new data file: %s\r\n", ff->name);
	}
	
	// trigger the adc start to with the pps
	start_sec = pps_timer_sync( ltc2512_trigger );
	
	// set the max file size
	sd_card_set_file_size(ff, config->file_size);

	// get the number of free blocks on sd card 	
	sd_card_get_free(config->active_sd_card, &pmel.free);

	// read battery voltage
	ina260_read(&pmel.amps, &pmel.volts, 0);
	
	// write the ascii pmel data file header to the new file, using the adc buffer as a temporary buffer
	// so the first block of the data file will be ascii header information
	// this call is after the adc initialization because the exact sampling rate is defined in the adc init function
	adc_header.second = epoch; // set the data header time to the data start time
	adc_header.usec = 0; // this is zero because the start trigger was synced to the pps edge
	char hdr_buf[512];
	memset(hdr_buf, 0, 512); // zero out the header buffer
	pmel_file_header(hdr_buf, config, &adc_header, &pmel); // build the ascii buffer with the header info 
	
	// write the header buffer to the first block of the new ile
	if( sd_card_fwrite(ff, hdr_buf, 1) != FR_OK ) {
		printf("Error writing to file: %s\r\n", ff->name);
	}
	
	return(start_sec);
	
}

//
// Log a buffer to file
// close current file if full, pausing adc to prevent buffer overflow
// open a new file if it's closed and sync the adc start on the next pps  
//
void log_buffer_to_file(wispr_config_t *config, fat_file_t *ff, uint8_t *buffer, uint16_t nblocks, char *type)
{
	// close the file if full
	if( ff->state & SD_FILE_FULL ) {
		printf("Closing data file: %s\r\n", ff->name);
		ltc2512_pause();
		sd_card_fclose(ff);
	}

	// open the file if closed
	if( ff->state == SD_FILE_CLOSED ) {
		sync_adc_start_with_new_file(config, ff);
	}

	// write the buffer
	if( sd_card_fwrite(ff, buffer, nblocks) != FR_OK ) {
		printf("Error writing to file: %s\r\n", ff->name);
	}
	
}


//
// Enter backup mode sleep, shutting down as much as possible to save power
//
void go_to_sleep(wispr_config_t *config)
{
	rtc_time_t dt;
	uint32_t now;
	rtc_get_epoch(&now);
	
	// set the alarm to wakeup for the next window read cycle
	// the wakeup signal is generated by the external rtc
	printf("\r\nEntering sleep mode for %d seconds at %s\r\n", config->sleep_time, epoch_time_string(now));

	epoch_to_rtc_time(&dt, now + config->sleep_time);
	pcf2129_set_alarm(&dt);

	//printf("\r\nWakeup alarm set for %s\r\n", epoch_time_string(now + config->sleep_time));
	printf("\r\nWakeup alarm set for %02d/%02d/%02d %02d:%02d:%02d\r\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);

	// flush the uarts
	while (!uart_is_tx_empty(UART0)) {}
	while (!uart_is_tx_empty(UART1)) {}
	//delay_ms(100);

	// initialize the wakeup signal on PA02
	board_init_wakeup();

	/* Switch MCK to slow clock  */
	//pmc_switch_mck_to_sclk(PMC_MCKR_PRES_CLK_1);
	//pmc_switch_mainck_to_fastrc(CKGR_MOR_MOSCRCF_4_MHz);
	//pmc_switch_mck_to_mainck(PMC_PCK_PRES_CLK_64);

	// Configure all PIOs as inputs to save power
	pio_set_input(PIOA, 0xFFFFFFFF, PIO_DEFAULT);
	pio_set_input(PIOB, 0xFFFFFFFF, PIO_DEFAULT);

	/* Disable unused clock to save power */
	//	pmc_osc_disable_xtal(1);
	pmc_disable_pllack();
	//	pmc_disable_all_periph_clk();
	pmc_disable_periph_clk(ID_PIOA);
	//	pmc_osc_disable_fastrc();

	// Enter into backup mode
	pmc_enable_backupmode();
	//pmc_sleep(SAM_PM_SMODE_BACKUP);

}

uint8_t swap_sd_cards(wispr_config_t *config)
{	
	
	if( config->active_sd_card == 1 ) {
		sd_card_unmount(1);
		sd_card_mount(2);
		config->active_sd_card = 2;
	}
	else if( config->active_sd_card == 2 ) {
		sd_card_unmount(2);
		sd_card_mount(1);
		config->active_sd_card = 1;
	} else {
		printf("SD Card Swap Failed\n\r");
		return(0);
	}

	// for testing only
	if( test_sd_card_nblocks > 0 ) {
		
		// grow the card size - used for testing
		uint32_t nblocks = sd_card_get_number_of_blocks(config->active_sd_card);
		sd_card_set_number_of_blocks(config->active_sd_card, nblocks + test_sd_card_nblocks);

		uint32_t epoch = 0;
		rtc_get_epoch(&epoch);
		printf("\r\nSwitch to Card %d at %s\r\n", config->active_sd_card, epoch_time_string(epoch));

		//sd_card_print_info(config->active_sd_card);

	}

	// update the config time and save it to preserve the active card number
	rtc_get_epoch(&config->epoch);
	sd_card_fwrite_config(config_filename, config);

	printf("\r\nSwitch to Card %d at %s\r\n", config->active_sd_card, epoch_time_string(config->epoch));

	return(config->active_sd_card);
}
// 
// Open each sd card and check the free storage.
// Stop and return when a card has space, making that card active.
// Exits with active card mounted and config updated with values from the active card.
//
int initialize_sd_cards(wispr_config_t *config)
{
	FRESULT res;
	
	// loop over all the cards 
	for(int n = 1; n <= NUMBER_SD_CARDS; n++) {
	
		// mount card
		res = sd_card_mount(n);

		// if problem mounting, then continue to next card
		if (res != FR_OK) {
			if ( res == FR_NO_FILESYSTEM ) {		
				printf("initialize_sd_cards: card %d if unformatted\r\n", n);
			} else {
				printf("initialize_sd_cards: card %d error %d\r\n", n, res);
			}
			res = sd_card_unmount(n);
			continue;
		}
		
		// check free space on card
		// continue to next card if there's not enough
		if( sd_card_free_space(n) < 32 ) {
			printf("initialize_sd_cards: card %d is full\r\n", n);
			res = sd_card_unmount(n);
			continue;
		}

		// read the configuration
		res = sd_card_fread_config(config_filename, config);
		if( res != FR_OK) {
			if( res == FR_NO_FILE) {
				
				// set defaults config and write it 
				wispr_config_set_default(config);
				
				if( sd_card_fwrite_config(config_filename, config) == FR_OK ) {
					printf("Default configuration set and written to card %d\r\n", n);
					res = FR_OK;
				}

			} else {			
				printf("initialize_sd_cards: error %d reading config from card %d\r\n", res, n);
			}
		}
		
		// use this card and break from loop
		config->active_sd_card = n;
		
		//printf("initialize_sd_cards: card %d selected\r\n", n);
		
		break;
		
	}
	return(res);
}

void initialize_config(wispr_config_t *config)
{		
	//uint8_t card_num = config->active_sd_card;
	// writes config to whatever card that is currently mounted	

	// display the config
	wispr_config_print(config);
			
	// Prompt to set new configuration
	while(1) {
		if( console_prompt_int("Change configuration?", 0, 8) ) {
			wispr_config_menu(config, 60);
		} else break;	
		wispr_config_print(config);
	}
	
	// save the new config and close the card
	sd_card_fwrite_config(config_filename, config);

}

uint32_t initialize_datetime(void)
{
	uint32_t status;
	rtc_time_t dt;

	// read and display the external rtc time
	//status = ds3231_get_datetime(&dt);
	status = pcf2129_get_datetime(&dt);
	if ( status != RTC_STATUS_OK ) {
		printf("DS3231 RTC failed, status %d\r\n", status);
		//rtc_status = ds3231_get_datetime(&dt);
	}
	//printf("External RTC time is %02d/%02d/%02d %02d:%02d:%02d\r\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
	
	// Initialize the internal RTC using the external RTC time
	status = rtc_init(&dt);
	while ( status != RTC_STATUS_OK ) {
		printf("Waiting for RTC, status %d\r\n", status);
		status = rtc_init(&dt);
	}
	printf("\r\nRTC set to %02d/%02d/%02d %02d:%02d:%02d\r\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);

	return(status);
}


uint32_t initialize_datetime_with_gps(void)
{
	uint32_t status;
	uint16_t timeout = 10; //10 sec wait for GPS message
	rtc_time_t dt;
	wispr_config_t tmp; // temporary config 
	
	// request a gps message
	// if a valid gps message is received, use it to set the time
	// if other msgs are rcvd while waiting for gps, they are ignored
	if( pmel_request_gps(&tmp, timeout) == COM_VALID_MSG ) {

		//Convert epoch time to RTC datetime format
		epoch_to_rtc_time(&dt, tmp.gps.second);

		// Initialize the external RTC using GPS time
		status = pcf2129_set_datetime(&dt);

	} else { // else NO GPS time available. Sync internal RTC with external RTC
		
		printf("No GPS message received from COM0. Sync int RTC by DS3231\n\r");
		
		// read the external rtc time
		pcf2129_get_datetime(&dt);
		
	}
	
	// Initialize the internal RTC using ds3231 RTC time
	status = rtc_init(&dt);
	while ( status != RTC_STATUS_OK ) {
		printf("Waiting for RTC, status %d\r\n", status);
		status = rtc_init(&dt);
	}
	printf("RTC set to %02d/%02d/%02d %02d:%02d:%02d\r\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
	printf("\r\n");
	
	return(status);
}

