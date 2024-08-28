#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#include <libps5000a/ps5000aApi.h>

/*
 * Reads data into a file with signed little-endian 16 bit values.  To
 * convert into unsigned byte values (sacrificing precision) this works:
 *
 * od -w2 -t d1 -v <filename> | LC_ALL=C awk '{ printf("%c", $3+128) }' > output.bin
 *
 * To show as decimal, do this:
 *
 * od -w2 -t d2 -v <filename>
 */

#define CHECK_CALL(call, message) \
  { \
    PICO_STATUS status = (call); \
    if (status != PICO_OK) { \
      fprintf(stderr, "ERROR: %s (status %d)\n", message, status); exit(1); \
    } \
  }

typedef enum enBOOL{FALSE,TRUE} BOOL;

#define min(a,b) ((a) < (b) ? a : b)

int16_t g_ready = FALSE;
int32_t g_sampleCount;
uint32_t g_startIndex;
int16_t g_overflow;

int16_t *driverBuffer;
int16_t *appBuffer;
	
void PREF4 callBackStreaming(int16_t handle, int32_t n_samples, uint32_t startIndex,
			     int16_t overflow, uint32_t triggerAt, int16_t triggered,
			     int16_t autoStop, void *pParameter)
{
  if (n_samples != 0) {
    g_sampleCount = n_samples;
    g_startIndex  = startIndex;
    g_ready = TRUE;
    g_overflow = overflow;
    memcpy(appBuffer + startIndex, driverBuffer + startIndex, n_samples * sizeof(int16_t));
  }
}

void collectStreamingImmediate(int16_t handle, char *filename, uint32_t sampleInterval, long n_total_samples)
{
  uint32_t bufferSize = 2000000;
  int hasOverflow = 0;

  /* Trigger disabled */
  CHECK_CALL(ps5000aSetSimpleTrigger(handle, 0, PS5000A_CHANNEL_A, 0, PS5000A_RISING, 0, 0),
	     "ps5000aSetSimpleTrigger failed");

  driverBuffer = (int16_t*)calloc(bufferSize, sizeof(int16_t));
  appBuffer = (int16_t*)calloc(bufferSize, sizeof(int16_t));

  CHECK_CALL(ps5000aSetDataBuffer(handle, PS5000A_CHANNEL_A, driverBuffer, bufferSize, 0, PS5000A_RATIO_MODE_NONE),
	     "ps5000aSetDataBuffers");
  
  printf("Streaming Data for %lu samples to %s\n", n_total_samples, filename);
  FILE *fp = fopen(filename, "w");
  if (!fp) {
    printf("Cannot open the file %s for writing.\n", filename);
    exit(1);
  }

  CHECK_CALL(ps5000aRunStreaming(handle, &sampleInterval, PS5000A_NS,
				 0 /* maxPreTriggerSamples */, 0 /* maxPostTriggerSamples */, FALSE /*autostop*/, 
				 1 /* downsampleRatio */, PS5000A_RATIO_MODE_NONE, bufferSize),
	     "ps5000aRunStreaming");
  printf("Actual sample interval used: %d\n", sampleInterval);

  struct timeval start, end;
  gettimeofday(&start, NULL);
  
  long totalSamples = 0;
  while (totalSamples < n_total_samples || n_total_samples == 0) {
    g_ready = FALSE;
    CHECK_CALL(ps5000aGetStreamingLatestValues(handle, callBackStreaming, NULL),
	       "ps5000aGetStreamingLatestValues");

    if (g_ready && g_sampleCount > 0) { /* Can be ready and have no data, if autoStop has fired */
      totalSamples += g_sampleCount;
      printf("Collected %7u samples, index = %7u, Total: %11ld samples %s         \r",
	     g_sampleCount, g_startIndex, totalSamples, g_overflow ? " OVERFLOW" : "");
      hasOverflow |= g_overflow;
      fwrite(appBuffer + g_startIndex, sizeof(*appBuffer), g_sampleCount, fp);
    }
  }
  
  gettimeofday(&end, NULL);
  long millis = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;
    
  printf("\nData collection complete.  Total time %ld ms. %s\n", millis, hasOverflow ? " OVERFLOW detected!" : "");
  ps5000aStop(handle);
  fclose(fp);

  free(driverBuffer);
  free(appBuffer);
  
  CHECK_CALL(ps5000aSetDataBuffers(handle, PS5000A_CHANNEL_A, NULL, NULL, 0, 0, PS5000A_RATIO_MODE_NONE),
	     "ps5000aSetDataBuffers");
}

int32_t main(int argc, char *argv[])
{
  PS5000A_RANGE range = PS5000A_100MV; // PS5000A_500MV;
  PS5000A_COUPLING coupling = PS5000A_AC;
  PS5000A_DEVICE_RESOLUTION resolution = PS5000A_DR_12BIT;
  long n_total_samples = 0L; // 100000000L;
  uint32_t sample_interval = 16; // ns
  
  char *filename = "stream.bin";
  if (argc == 2)
    filename = argv[1];
  
  int maxChannels = 100;
  int16_t handle;
  
  PICO_STATUS status = ps5000aOpenUnit(&handle, NULL, resolution);
  if (status == PICO_NOT_FOUND) {
    printf("Picoscope device not found\n");
    exit(1);
  } else if (status == PICO_POWER_SUPPLY_NOT_CONNECTED) {
    printf("5 V power supply not connected.  Using USB only.\n");
    maxChannels = 2;
    CHECK_CALL(ps5000aChangePowerSource(handle, PICO_POWER_SUPPLY_NOT_CONNECTED), "ps5000aChangePowerSource"); // Tell the driver that's ok
  } else if (status == PICO_USB3_0_DEVICE_NON_USB3_0_PORT) {
    maxChannels = 2;
    printf("Switching to use USB power from non-USB 3.0 port.\n");
    CHECK_CALL(ps5000aChangePowerSource(handle, PICO_USB3_0_DEVICE_NON_USB3_0_PORT), "ps5000aChangePowerSource");
  } else if (status != PICO_OK) {
    printf("Picoscope open failed\n");
    exit(1);
  }

  char line[80];
  int16_t requiredSize = 0;
  CHECK_CALL(ps5000aGetUnitInfo(handle, line, sizeof(line), &requiredSize, PICO_VARIANT_INFO), "ps5000aGetUnitInfo");

  int16_t channelCount = min((int16_t)line[1] - '0', maxChannels);
  printf("Unit has %d channels.\n", channelCount);

  int16_t value;
  ps5000aMaximumValue(handle, &value);
  printf("Max ADC value=%d\n", value);

  float maximumVoltage;
  float minimumVoltage;
  CHECK_CALL(ps5000aGetAnalogueOffset(handle, range, coupling, &maximumVoltage, &minimumVoltage), "ps5000aGetAnalogueOffset");

  printf("Permitted analog offset range: %f-%f\n", minimumVoltage, maximumVoltage);
  
  if (strstr(line, "MSO"))
    for (int i = 0; i < 2; i++)
      CHECK_CALL(ps5000aSetDigitalPort(handle, (PS5000A_CHANNEL)(i + PS5000A_DIGITAL_PORT0), 0, 0), "ps5000aSetDigitalPort");

  CHECK_CALL(ps5000aSetEts(handle, PS5000A_ETS_OFF, 0, 0, NULL), "ps5000aSetEts"); // Turn off hasHardwareETS
  
  for (int i = 0; i < channelCount; i++)
    CHECK_CALL(ps5000aSetChannel(handle, (PS5000A_CHANNEL)(PS5000A_CHANNEL_A + i),
				 i == 0, // enable only channel A
				 coupling,
				 range,
				 0.0), // Analogue offset
	       "ps5000aSetChannel");

  if (coupling == PS5000A_AC) {
    long samples_to_discard = 1000000000L / sample_interval; // 1s
    printf("Capturing %ld samples to /dev/null to let AC coupling capacitor settle\n", samples_to_discard);
    collectStreamingImmediate(handle, "/dev/null", sample_interval, samples_to_discard);
  }
  
  collectStreamingImmediate(handle, filename, sample_interval, n_total_samples);
  
  ps5000aCloseUnit(handle);
  return 0;
}
