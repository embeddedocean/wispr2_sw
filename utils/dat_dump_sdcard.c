/*
 * dat_dump_sdcard.c
 *
 * Reads the contents of a WISPR SD card and dumps the data to raw data (.dat) files.
 * The contents of the card are written to multiple data files with a specified length.
 * The length of the output data files is specific in seconds.
 * The size in seconds of the output data file is specified by the -t command line option. 
 * Data files can also be separated by time gaps in the data.
 * This is usefull if data is collected intermittently with periods of sleep (data gaps) 
 * between periods of continuous data logging. In this case, the program forces a new file 
 * to be created if a data gap is found that exceeds the set threshold.
 * The size in seconds of the max data gap is specified by the -g command line option.
 *
 * Waveform data is written to files with .dat file extensions.
 * Spectrum data is written to files with .psd file extensions.
 * The names of the output files are created using a file prefix and the data timestamp.
 * For example, an output file with name WISPR_200325_151711.dat contains waveform data 
 * starting at date 20/03/25 and time 15:17:11. 
 *
 * The program will read all the valid data on the card, starting with the first data block 
 * and continuing to the last data block written to the card. 
 * The first and last data block on the card are read from the card header. 
 *
 * TODO: Add start and stop times for data dump.
 *  
 * cjones, April 2020
 *
 */
 
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "sd_card_lite.h"

#include "wispr.h"
#include "wav_file.h"                                                                                                                                                                                                                           

uint8_t buffer[ADC_BUFFER_SIZE];
uint8_t *buffer_header = buffer;  // header is at start of buffer
uint8_t *buffer_data = &buffer[WISPR_DATA_HEADER_SIZE]; // data follows header

wispr_data_header_t hdr;
wispr_config_t cfg;
sd_card_t sd;

uint32_t find_header(int fd);

int main(int argc, char **argv)
{
  char *input_path = "/dev/sdb";
  char *output_path = ".";
  char *prefix = "WISPR";

  char out_filename[128];
  int c = 0;
  size_t buffer_size = 0;
  size_t nrd = 0;
  size_t nwrt = 0;
  size_t start_block = WISPR_SD_CARD_START_BLOCK;
  uint32_t pos = 0;

  FILE *input_fp;
  FILE *dat_fp = NULL;
  FILE *psd_fp = NULL;

  int open_new_file = 1;
  uint32_t seconds_per_file = 60;
  uint32_t prev_sec = 0;
  uint32_t data_gap = 2; 
  float duration = 0.0;

  int dat_buffer_count = 0;
  int psd_buffer_count = 0;

  // Parse command line args
  while ((c = getopt(argc, argv, "b:s:i:o:p:t:g:h")) != -1) 
  {
    switch (c) 
    {
      case 's':
      {
        start_block = atoi(optarg);
        break;
      }
      case 't':
      {
        seconds_per_file = atoi(optarg);
        break;
      }
      case 'g':
      {
        data_gap = (uint32_t)atoi(optarg);
        break;
      }
      case 'i':
      {
        input_path = optarg;
        break;
      }
      case 'o':
      {
        output_path = optarg;
        break;
      }
      case 'p':
      {
        prefix = optarg;
        break;
      }
      case 'h':
      {
        fprintf(stdout, "\nRead SD Card and write data to file\n");
        fprintf(stdout, "  Usage %s [options]\n", argv[0]);
        fprintf(stdout, "  Optional command line arguments [defaults]\n");
        fprintf(stdout, "    -i input path raw data [%s]\n", input_path);
        fprintf(stdout, "    -o path to save data files [%s]\n", output_path);
        fprintf(stdout, "    -o data file prefix [%s]\n", prefix);
        fprintf(stdout, "    -t duration of data file in seconds [%d]\n", seconds_per_file);
        fprintf(stdout, "    -g max data gap between data records in seconds [%d]\n", data_gap);
        return 1;
      }
    }
  }
  
  // Open the device with raw samples
  input_fp = fopen(input_path, "r");
  
  //if(input_fd < 0) {
  if(input_fp == NULL) {
    fprintf(stdout, "failed to open SD Card: %s\n", input_path);
    return -1;
  }
  fprintf(stdout, "Openned SD Card\n");

  // read the card header block
  pos = (WISPR_SD_CARD_BLOCK_SIZE * WISPR_SD_CARD_HEADER_BLOCK);
  fseek(input_fp, pos, SEEK_SET);
  nrd = fread(buffer, 1, WISPR_SD_CARD_BLOCK_SIZE, input_fp);
  if (nrd =! WISPR_SD_CARD_BLOCK_SIZE) {
     fprintf(stdout, "Failed to read card header block: %d\n", nrd);
     return -1;
  }
  if( sd_card_parse_header(buffer, &sd) == 0 ) {
     fprintf(stdout, "Failed to parse card header\n");
  }

  // read the configuration block of the card
  pos = (WISPR_SD_CARD_BLOCK_SIZE * WISPR_SD_CARD_CONFIG_BLOCK);
  fseek(input_fp, pos, SEEK_SET);
  nrd = fread(buffer, 1, WISPR_SD_CARD_BLOCK_SIZE, input_fp);
  if (nrd =! WISPR_SD_CARD_BLOCK_SIZE) {
     fprintf(stdout, "Failed to read card configuration block: %d\n", nrd);
     return -1;
  }
  if( wispr_parse_config(buffer, &cfg) == 0 ) {
     fprintf(stdout, "Failed to parse configuration\n");
  }

  // print the card header info
  sd_card_print_header(&sd);

  //wispr_print_config(&cfg);
  
  // seek to start of data    
  start_block = sd.start; //WISPR_SD_CARD_START_BLOCK;
  pos = (WISPR_SD_CARD_BLOCK_SIZE * start_block);
  fseek(input_fp, pos, SEEK_SET);
  pos = ftell(input_fp) / WISPR_SD_CARD_BLOCK_SIZE;
  fprintf(stdout, "Seek to file position %d (block %d)\n", pos, start_block);

  int go = 1;
  while(go) 
  {

    // Read data buffer header
    nrd = fread(buffer_header, 1, WISPR_DATA_HEADER_SIZE, input_fp);
    if (nrd != WISPR_DATA_HEADER_SIZE) {
       fprintf(stdout, "failed to read data header: %d\n", nrd);
       return -1;
    }

    // Parse data header
    if( wispr_parse_data_header(buffer_header, &hdr) == 0 ) {
       fprintf(stdout, "failed to parse data header\n");
       return -1;
       //continue;
    }

    //wispr_print_data_header(&hdr);

    // Read data buffer
    buffer_size = (size_t)hdr.buffer_size - WISPR_DATA_HEADER_SIZE;
    nrd = fread(buffer_data, 1, buffer_size, input_fp);
    if (nrd != buffer_size) {
       fprintf(stdout, "failed to read data block: %d\n", nrd);
       return -1;
    }

    if(hdr.type == WISPR_WAVEFORM) {

		if(prev_sec == 0) prev_sec = hdr.second;

		// check the data buffer time to determine total duration of data written to file
		duration += ((float)hdr.samples_per_buffer / (float)hdr.sampling_rate);
		if( duration >= (float)seconds_per_file ) {
			open_new_file = 1;
			duration = 0.0;
		}
		// check the gap time between buffer
		// if it's larger than threshold then make a new file
		if( (hdr.second - prev_sec) > data_gap ) {
			open_new_file = 1;
			duration = 0.0;
			fprintf(stdout, "Data gap of %u seconds found\n", hdr.second - prev_sec);
		}
		prev_sec = hdr.second;
    
		// open new output file
		if(open_new_file == 1) {

			//wispr_print_data_header(&hdr);
    
			open_new_file = 0;
    
			rtc_time_t rtc;
			epoch_to_rtc_time(&rtc, hdr.second);
       
			// Open file to write raw data
			if(dat_fp != NULL) {
		      fprintf(stdout, "Closing raw data file %s, %d buffers written\n\n", out_filename, dat_buffer_count);
			  fclose(dat_fp);
			  dat_buffer_count = 0;
			}
			sprintf(out_filename,"%s/%s_%02d%02d%02d_%02d%02d%02d.dat", output_path, prefix, 
               rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second);
			dat_fp = fopen(out_filename, "a+");
			fprintf(stdout, "Opening raw data file %s\n", out_filename);
			if(dat_fp == NULL) {
              fprintf(stdout, "Failed to open output file\n");
              return -1;
			}
		}
          
		// Write the whole buffer (header and data) to output file
		if(dat_fp != NULL) {
           buffer_size = (size_t)hdr.buffer_size;
           nwrt = fwrite(buffer, 1, buffer_size, dat_fp);
           if (nwrt != buffer_size) {
             fprintf(stdout, "failed to write buffer: %d\n", nwrt);
             //continue;
           }
           dat_buffer_count++;
        }
    }

    // leave read loop if the last block has been read
    pos = ftell(input_fp) / WISPR_SD_CARD_BLOCK_SIZE;
    if( pos >= sd.write_addr ) {
        go = 0;
        fprintf(stdout, "Last data record found at block %d\n", pos);
    }

  }

  fclose(input_fp);
  if(dat_fp != NULL) fclose(dat_fp);

  return 1;
}



uint32_t find_header(int fd) 
{
    uint32_t pos = 0;
    char buf, prev_buf;
    int nrd = 0; 
    while(1) {
        nrd = read(fd, &buf, 1); pos++;
        if(buf == 'W') {
            nrd = read(fd, &buf, 1); pos++;
            if(buf == 'I') {
                nrd = read(fd, &buf, 1); pos++;
                if(buf == 'S') {
                    nrd = read(fd, &buf, 1); pos++;
                    if(buf == 'P') {
                        nrd = read(fd, &buf, 1); pos++;
                        if(buf == 'R') {
                            fprintf(stdout, "WISPR header found at %d\n", pos-5);
                            return(pos-5);
                        }
                    }
                }
            }
        }
    }
    return(-1);
}


