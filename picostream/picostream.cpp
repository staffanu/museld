#ifndef _GNU_SOURCE
#define _GNU_SOURCE // for sync_file_range
#endif
#include <stdio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

#include <libps5000a/ps5000aApi.h>

#include "SignalLossDetector.h"

/*
 * Streams samples from a Picoscope 5000A-series device to a file.
 *
 * Analog mode (default): signed little-endian 16 bit samples from one channel,
 * or signed 8 bit samples with --bits 8.
 * To convert 16 bit samples into unsigned byte values (sacrificing precision) this works:
 *
 * od -w2 -t d1 -v <filename> | LC_ALL=C awk '{ printf("%c", $3+128) }' > output.bin
 *
 * To show as decimal, do this:
 *
 * od -w2 -t d2 -v <filename>
 *
 * Disk writes run on a separate thread fed by a pool of buffers (--buffers), with Linux
 * writeback managed so dirty pages never stall the capture; the end-of-capture report
 * flags any write-buffer overrun or driver-buffer wrap (i.e. lost samples).
 *
 * Digital mode (--digital, MSO models only): each sample is one byte holding
 * D0-D7 (--bits 8), or one little-endian 16 bit word holding D0-D15 (--bits 16).
 *
 * On exit (analog mode) the min/max sample values are reported in both integer and volt
 * form, together with how many samples reached the ADC full-scale limit and the 99/99.9/
 * 99.99th |sample| percentiles (a noise-robust amplitude gauge for picking --range).  With
 * --stop-on-signal-loss the capture stops automatically once the input goes silent.
 */

#define CHECK_CALL(call, message) \
  { \
    PICO_STATUS status = (call); \
    if (status != PICO_OK) { \
      fprintf(stderr, "ERROR: %s (status %d)\n", message, status); exit(1); \
    } \
  }

int16_t g_ready = 0;
int32_t g_sampleCount;
uint32_t g_startIndex;
int16_t g_overflow;
volatile sig_atomic_t g_stop = 0;

bool g_digital = false;
int g_digitalPorts = 1; // 1 / 2 ports of 8 digital channels each
size_t g_bytesPerSample = 2;
PS5000A_CHANNEL g_channel = PS5000A_CHANNEL_A;
double g_rangeVolts = 0.1; // full-scale voltage of the selected --range
int g_maxAdcValue = 32767;  // ADC reading at full scale (from ps5000aMaximumValue)
int g_writeBuffers = 64;    // number of 1 MiB write buffers absorbing disk-write stalls
uint32_t g_deviceBufferSize = 4000000; // driver circular buffer in samples (~64 ms at 62.5 MS/s)

int16_t *driverBuffers[2];
int16_t *appBuffer;

void sigintHandler(int)
{
  if (g_stop) // second Ctrl-C: give up on a clean shutdown
    _exit(1);
  g_stop = 1;
}

void PREF4 callBackStreaming(int16_t handle, int32_t n_samples, uint32_t startIndex,
			     int16_t overflow, uint32_t triggerAt, int16_t triggered,
			     int16_t autoStop, void *pParameter)
{
  if (n_samples != 0) {
    g_sampleCount = n_samples;
    g_startIndex  = startIndex;
    g_ready = 1;
    g_overflow = overflow;

    if (!g_digital && g_bytesPerSample == 1)
      for (int i = 0; i < n_samples; i++) // 8 bit resolution: the ADC value is in the high byte
        ((int8_t *)appBuffer)[startIndex + i] = (int8_t)(driverBuffers[0][startIndex + i] >> 8);
    else if (!g_digital)
      memcpy(appBuffer + startIndex, driverBuffers[0] + startIndex, n_samples * sizeof(int16_t));
    else
      for (int i = 0; i < n_samples; i++) {
        if (g_digitalPorts == 2)
          appBuffer[startIndex + i] = // little endian
                  (driverBuffers[0][startIndex + i] & 0xff) | ((driverBuffers[1][startIndex + i] & 0xff) << 8);
        else
          ((uint8_t *)appBuffer)[startIndex + i] = driverBuffers[0][startIndex + i] & 0xff;
      }
  }
}

/*
 * Disk writes run on a dedicated thread fed by a pool of fixed-size buffers, so a stall
 * in write() never blocks the polling loop (which would let the driver's circular buffer
 * wrap over unread samples).  On Linux, writeback of each completed window is started
 * eagerly and the previous window is waited on and evicted, so dirty pages never pile up
 * enough for the kernel to throttle write() in balance_dirty_pages() -- the ~200 ms stalls
 * that throttling causes are what silently overrun capture buffers on this hardware.
 * Ported from fx3usbadc/usbstream's BulkTransfer.
 */
struct WriteBlock { uint8_t *data; size_t len; };

struct Writer {
  int fd = -1;
  bool regular = false;               // apply writeback management only to real files
  size_t blockCap = 1u << 20;         // 1 MiB per buffer
  static constexpr long window = 8L << 20; // writeback window (8 blocks)

  std::deque<WriteBlock> vacant, filled;
  std::mutex mtx;
  std::condition_variable cvFilled;
  bool running = false;

  long writeOffset = 0, flushedOffset = 0;

  int poolCount = 0;
  int vacantMin = 0;                  // low-water of vacant buffers -> high-water of usage
  long overruns = 0;                  // times the pool was empty and a chunk was dropped
  WriteBlock cur{nullptr, 0};         // buffer the producer is currently filling
};

static void writerThread(Writer *w)
{
  for (;;) {
    WriteBlock b{nullptr, 0};
    {
      std::unique_lock<std::mutex> lock(w->mtx);
      w->cvFilled.wait(lock, [w] { return !w->running || !w->filled.empty(); });
      if (!w->running && w->filled.empty())
	break;
      b = w->filled.front();
      w->filled.pop_front();
    }

    for (size_t off = 0; off < b.len; ) {
      ssize_t n = write(w->fd, b.data + off, b.len - off);
      if (n < 0) {
	fprintf(stderr, "\nwrite failed: %s\n", strerror(errno));
	exit(1);
      }
      off += n;
    }

    {
      std::unique_lock<std::mutex> lock(w->mtx);
      w->vacant.push_back(b);
    }

    if (w->regular) {
      w->writeOffset += b.len;
      while (w->writeOffset - w->flushedOffset >= Writer::window) {
	sync_file_range(w->fd, w->flushedOffset, Writer::window, SYNC_FILE_RANGE_WRITE);
	if (w->flushedOffset >= Writer::window) {
	  long prev = w->flushedOffset - Writer::window;
	  sync_file_range(w->fd, prev, Writer::window,
			  SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
	  posix_fadvise(w->fd, prev, Writer::window, POSIX_FADV_DONTNEED);
	}
	w->flushedOffset += Writer::window;
      }
    }
  }
}

// Append `len` bytes to the write pool, enqueueing full blocks to the writer thread.
// If the pool is empty (writer can't keep up), the remaining bytes are dropped and an
// overrun is counted -- dropping keeps the poll loop alive rather than stalling it into
// a driver-buffer overrun.
static void produce(Writer &w, const uint8_t *src, size_t len)
{
  while (len > 0) {
    if (w.cur.data == nullptr) {
      std::unique_lock<std::mutex> lock(w.mtx);
      w.vacantMin = std::min(w.vacantMin, (int)w.vacant.size());
      if (w.vacant.empty()) {
	w.overruns++;
	return;
      }
      w.cur = w.vacant.front();
      w.vacant.pop_front();
      w.cur.len = 0;
    }
    size_t n = std::min(len, w.blockCap - w.cur.len);
    memcpy(w.cur.data + w.cur.len, src, n);
    w.cur.len += n;
    src += n;
    len -= n;
    if (w.cur.len == w.blockCap) {
      std::unique_lock<std::mutex> lock(w.mtx);
      w.filled.push_back(w.cur);
      w.cvFilled.notify_one();
      w.cur = WriteBlock{nullptr, 0};
    }
  }
}

void collectStreamingImmediate(int16_t handle, const char *filename, uint32_t sampleInterval, long n_total_samples,
			       SignalLossDetector *detector, bool reportStats)
{
  uint32_t bufferSize = g_deviceBufferSize; // driver circular buffer; poll-gap slack = bufferSize / rate
  int hasOverflow = 0;
  bool signalLost = false;

  int sampleMin = INT_MAX;
  int sampleMax = INT_MIN;
  long long clippedSamples = 0;
  std::vector<long long> absHist; // |sample| histogram for exact amplitude percentiles
  if (reportStats)
    absHist.assign(g_maxAdcValue + 2, 0);

  /* Trigger disabled */
  CHECK_CALL(ps5000aSetSimpleTrigger(handle, 0, PS5000A_CHANNEL_A, 0, PS5000A_RISING, 0, 0),
	     "ps5000aSetSimpleTrigger failed");

  int n_buffers = g_digital ? g_digitalPorts : 1;
  for (int i = 0; i < n_buffers; i++) {
    PS5000A_CHANNEL source = g_digital ? (PS5000A_CHANNEL)(PS5000A_DIGITAL_PORT0 + i) : g_channel;
    driverBuffers[i] = (int16_t*)calloc(bufferSize, sizeof(int16_t));
    CHECK_CALL(ps5000aSetDataBuffer(handle, source, driverBuffers[i], bufferSize, 0, PS5000A_RATIO_MODE_NONE),
	       "ps5000aSetDataBuffer");
  }

  appBuffer = (int16_t*)calloc(bufferSize, sizeof(int16_t));

  if (n_total_samples != 0)
    printf("Streaming Data for %ld samples to %s\n", n_total_samples, filename);
  else
    printf("Streaming Data to %s until interrupted (Ctrl-C)\n", filename);
  Writer writer;
  writer.fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
  if (writer.fd == -1) {
    fprintf(stderr, "Cannot open the file %s for writing: %s\n", filename, strerror(errno));
    exit(1);
  }
  struct stat st;
  writer.regular = fstat(writer.fd, &st) == 0 && S_ISREG(st.st_mode);
  writer.poolCount = std::max(2, g_writeBuffers);
  writer.vacantMin = writer.poolCount;
  for (int i = 0; i < writer.poolCount; i++)
    writer.vacant.push_back(WriteBlock{(uint8_t *)malloc(writer.blockCap), 0});
  writer.running = true;
  std::thread writerThreadHandle(writerThread, &writer);

  uint32_t requestedInterval = sampleInterval;
  CHECK_CALL(ps5000aRunStreaming(handle, &sampleInterval, PS5000A_NS,
				 0 /* maxPreTriggerSamples */, 0 /* maxPostTriggerSamples */, 0 /*autostop*/,
				 1 /* downsampleRatio */, PS5000A_RATIO_MODE_NONE, bufferSize),
	     "ps5000aRunStreaming");
  if (sampleInterval != requestedInterval)
    fprintf(stderr, "WARNING: sample interval %u ns not supported by the device; using %u ns (%.6g MS/s)\n",
	    requestedInterval, sampleInterval, 1e3 / sampleInterval);
  else
    printf("Actual sample interval used: %u ns (%.6g MS/s)\n", sampleInterval, 1e3 / sampleInterval);

  struct timeval start, end;
  gettimeofday(&start, NULL);

  // Poll-cadence diagnostics: samples produced (elapsed*rate) but not yet delivered are
  // sitting in the driver's circular buffer, so that difference is the buffer's current
  // fill level.  Its peak shows how close we came to a wrap (100% = full = samples lost).
  const double rate = 1e9 / sampleInterval;              // samples per second
  const double bufSpanSec = (double)bufferSize / rate;   // time the driver buffer holds
  auto tStream = std::chrono::steady_clock::now();
  auto tLast = tStream;
  auto tLastWarn = tStream;
  long long totalDelivered = 0;
  double maxDeviceBufferFill = 0;                        // peak occupancy of the driver buffer, in samples
  double maxGapSec = 0;
  long wrapWarnings = 0;

  long totalSamples = 0;
  while (!g_stop && (totalSamples < n_total_samples || n_total_samples == 0)) {
    g_ready = 0;
    CHECK_CALL(ps5000aGetStreamingLatestValues(handle, callBackStreaming, NULL),
	       "ps5000aGetStreamingLatestValues");

    auto now = std::chrono::steady_clock::now();
    double gap = std::chrono::duration<double>(now - tLast).count();
    tLast = now;
    if (gap > maxGapSec) maxGapSec = gap;

    if (g_ready && g_sampleCount > 0) { /* Can be ready and have no data, if autoStop has fired */
      long n_write = g_sampleCount;
      if (n_total_samples != 0)
	n_write = std::min(n_write, n_total_samples - totalSamples); // exact requested sample count
      totalSamples += n_write;
      totalDelivered += g_sampleCount;

      double fill = std::chrono::duration<double>(now - tStream).count() * rate - (double)totalDelivered;
      if (fill > maxDeviceBufferFill) maxDeviceBufferFill = fill;

      // Returned far fewer samples than were produced during the gap -> the driver's
      // circular buffer wrapped over unread data.  Rate-limited so a burst is one line.
      double expectedThisGap = gap * rate;
      if (gap > bufSpanSec && totalDelivered > bufferSize) {
	wrapWarnings++;
	if (std::chrono::duration<double>(now - tLastWarn).count() > 0.5) {
	  fprintf(stderr,
		  "\nWARNING: driver returned %d samples but ~%.0f were produced in the %.1f ms since the "
		  "last poll -- buffer wrapped, ~%.0f samples lost\n",
		  g_sampleCount, expectedThisGap, gap * 1e3, expectedThisGap - g_sampleCount);
	  tLastWarn = now;
	}
      }

      int used;
      { std::unique_lock<std::mutex> lock(writer.mtx); used = writer.poolCount - (int)writer.vacant.size(); }
      printf("\rCollected %7d samples, index = %7u, Total: %11ld  buf %2d/%d ovr %ld%s\033[K",
	     g_sampleCount, g_startIndex, totalSamples, used, writer.poolCount, writer.overruns,
	     g_overflow ? " OVERFLOW" : "");
      fflush(stdout);
      hasOverflow |= g_overflow;
      produce(writer, (uint8_t *)appBuffer + g_startIndex * g_bytesPerSample, (size_t)n_write * g_bytesPerSample);

      if (reportStats) { // stats on the raw ADC value (over the written samples) so they convert to volts
	const int16_t *src = driverBuffers[0] + g_startIndex;
	for (long i = 0; i < n_write; i++) {
	  int value = src[i];
	  if (value < sampleMin) sampleMin = value;
	  if (value > sampleMax) sampleMax = value;
	  int mag = std::abs(value);
	  if (mag >= g_maxAdcValue) clippedSamples++; // saturated at the ADC rail
	  absHist[std::min(mag, (int)absHist.size() - 1)]++;
	}
      }

      if (detector && detector->process((uint8_t *)appBuffer + g_startIndex * g_bytesPerSample, g_sampleCount)) {
	signalLost = true;
	break;
      }
    }
  }

  // Flush the partially-filled block, then drain and stop the writer thread.
  if (writer.cur.data) {
    std::unique_lock<std::mutex> lock(writer.mtx);
    if (writer.cur.len > 0) {
      writer.filled.push_back(writer.cur);
      writer.cvFilled.notify_one();
    } else {
      writer.vacant.push_back(writer.cur);
    }
    writer.cur = WriteBlock{nullptr, 0};
  }
  {
    std::unique_lock<std::mutex> lock(writer.mtx);
    writer.running = false;
    writer.cvFilled.notify_one();
  }
  writerThreadHandle.join();

  gettimeofday(&end, NULL);
  long millis = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;

  const char *how = signalLost ? "stopped (signal lost)" : g_stop ? "interrupted" : "complete";
  printf("\nData collection %s.  Total time %ld ms. %s\n",
	 how, millis, hasOverflow ? " OVERFLOW detected!" : "");

  if (signalLost) {
    char when[64];
    struct tm tm;
    localtime_r(&end.tv_sec, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    printf("*** SIGNAL LOST at %s -- capture stopped automatically after %ld samples ***\n",
	   when, totalSamples);
  }

  if (reportStats && sampleMax >= sampleMin) { // saw at least one sample
    double minVolts = (double)sampleMin * g_rangeVolts / g_maxAdcValue;
    double maxVolts = (double)sampleMax * g_rangeVolts / g_maxAdcValue;
    printf("Sample range: min %d (%.6g V), max %d (%.6g V)\n", sampleMin, minVolts, sampleMax, maxVolts);
    if (clippedSamples > 0)
      printf("Out of range: %lld samples (%.3f%% of %ld) reached the ADC full-scale limit\n",
	     clippedSamples, 100.0 * clippedSamples / totalSamples, totalSamples);
    else
      printf("Out of range: none\n");

    // High-|sample| percentiles: a noise-robust amplitude gauge for picking --range,
    // unlike min/max which are single worst-case samples inflated by range-scaled noise.
    auto percentile = [&] (double p) -> int {
      long long target = std::max<long long>(1, llround(p * totalSamples));
      long long cum = 0;
      for (size_t v = 0; v < absHist.size(); v++) {
	cum += absHist[v];
	if (cum >= target)
	  return (int)v;
      }
      return (int)absHist.size() - 1;
    };
    printf("Amplitude (|sample|):");
    for (double p : {0.99, 0.999, 0.9999}) {
      int adc = percentile(p);
      printf("  %.4g%% = %d (%.4g V)", p * 100, adc, (double)adc * g_rangeVolts / g_maxAdcValue);
    }
    printf("\n");
  }

  // Capture-integrity report: write-buffer pressure and poll-cadence gaps.
  {
    int usedMax = writer.poolCount - writer.vacantMin;
    printf("Write buffers: peak %d/%d used, overruns %ld\n", usedMax, writer.poolCount, writer.overruns);
    printf("Poll timing: max poll gap %.1f ms; peak device-buffer fill %.0f%% (%.1f of %.1f ms)\n",
	   maxGapSec * 1e3, 100.0 * maxDeviceBufferFill / bufferSize,
	   maxDeviceBufferFill / rate * 1e3, bufSpanSec * 1e3);
    if (writer.overruns > 0)
      printf("*** DATA LOST: %ld write-buffer overrun(s) -- writer fell behind; raise --buffers or use a faster disk ***\n",
	     writer.overruns);
    if (wrapWarnings > 0)
      printf("*** DATA LOST: %ld driver-buffer wrap(s) -- poll loop stalled longer than the buffer span ***\n",
	     wrapWarnings);
  }

  ps5000aStop(handle);
  close(writer.fd);
  while (!writer.vacant.empty()) { free(writer.vacant.front().data); writer.vacant.pop_front(); }

  for (int i = 0; i < n_buffers; i++) {
    PS5000A_CHANNEL source = g_digital ? (PS5000A_CHANNEL)(PS5000A_DIGITAL_PORT0 + i) : g_channel;
    CHECK_CALL(ps5000aSetDataBuffer(handle, source, NULL, 0, 0, PS5000A_RATIO_MODE_NONE),
	       "ps5000aSetDataBuffer");
    free(driverBuffers[i]);
  }
  free(appBuffer);
}

int32_t main(int argc, char *argv[])
{
  PS5000A_RANGE range = PS5000A_100MV;
  PS5000A_COUPLING coupling = PS5000A_AC;
  int bits = 0; // 0 = default for the selected mode
  uint32_t sample_interval = 16; // ns
  long n_total_samples = 0; // 0 = stream until interrupted
  double duration_seconds = 0;
  double logic_threshold_volts = 1.65;
  double signal_loss_timeout = 0; // 0 = disabled
  double signal_loss_threshold = 0.1;
  std::string filename = "stream.bin";
  bool filename_given = false;

  const std::vector<std::string> args(argv + 1, argv + argc);
  auto it = args.cbegin();

  struct Option {
    std::string name;
    std::string help;
    std::function<void ()> handler;
  };
  std::vector<Option> options;

  auto usage = [&options] () -> void {
    fprintf(stderr, "usage: picostream [options] [output_file (default stream.bin)]\n");
    for (const auto &o: options)
      fprintf(stderr, "  %-18s %s\n", o.name.c_str(), o.help.c_str());
    exit(EXIT_FAILURE);
  };

  auto nextArg = [&it, &args, &usage] () -> const std::string & {
    if (it == args.cend())
      usage();
    return *it++;
  };

  options.push_back({"--digital", "capture digital ports (MSO models) instead of an analog channel", [&] () -> void {
    g_digital = true;
  }});
  options.push_back({"--bits", "<n>: analog ADC resolution 8/12/14/15/16 (default 12; 8 writes s8, others s16le); digital channel count 8/16 (default 8)", [&] () -> void {
    bits = stoi(nextArg());
  }});
  options.push_back({"--rate", "<hz>: sample rate, e.g. 62.5e6 (default 62.5e6)", [&] () -> void {
    double rate = stod(nextArg());
    if (rate <= 0)
      throw std::runtime_error("Invalid sample rate");
    sample_interval = (uint32_t)fmax(1, llround(1e9 / rate));
    double actual_rate = 1e9 / sample_interval;
    if (fabs(actual_rate - rate) > 1e-6 * rate)
      fprintf(stderr, "WARNING: rate %.9g Hz is not a whole number of nanoseconds per sample; requesting %u ns (%.9g Hz)\n",
	      rate, sample_interval, actual_rate);
  }});
  options.push_back({"--sample-interval", "<ns>: sample interval in nanoseconds (alternative to --rate; default 16)", [&] () -> void {
    sample_interval = stoul(nextArg());
    if (sample_interval == 0)
      throw std::runtime_error("Invalid sample interval");
  }});
  options.push_back({"--range", "<v>: analog voltage range 10mV/20mV/50mV/100mV/200mV/500mV/1V/2V/5V/10V/20V (default 100mV)", [&] () -> void {
    static const struct { const char *name; PS5000A_RANGE range; double volts; } ranges[] = {
      {"10mV", PS5000A_10MV, 0.01}, {"20mV", PS5000A_20MV, 0.02}, {"50mV", PS5000A_50MV, 0.05},
      {"100mV", PS5000A_100MV, 0.1}, {"200mV", PS5000A_200MV, 0.2}, {"500mV", PS5000A_500MV, 0.5},
      {"1V", PS5000A_1V, 1}, {"2V", PS5000A_2V, 2}, {"5V", PS5000A_5V, 5},
      {"10V", PS5000A_10V, 10}, {"20V", PS5000A_20V, 20},
    };
    const std::string name = nextArg();
    auto match = std::find_if(std::begin(ranges), std::end(ranges),
			      [&name] (const auto &r) -> bool { return strcasecmp(name.c_str(), r.name) == 0; });
    if (match == std::end(ranges))
      throw std::runtime_error("Invalid voltage range: " + name);
    range = match->range;
    g_rangeVolts = match->volts;
  }});
  options.push_back({"--coupling", "<ac|dc>: analog input coupling (default ac)", [&] () -> void {
    const std::string name = nextArg();
    if (strcasecmp(name.c_str(), "ac") == 0)
      coupling = PS5000A_AC;
    else if (strcasecmp(name.c_str(), "dc") == 0)
      coupling = PS5000A_DC;
    else
      throw std::runtime_error("Invalid coupling: " + name);
  }});
  options.push_back({"--channel", "<A-D>: analog input channel (default A)", [&] () -> void {
    const std::string name = nextArg();
    if (name.size() != 1 || toupper(name[0]) < 'A' || toupper(name[0]) > 'D')
      throw std::runtime_error("Invalid channel: " + name);
    g_channel = (PS5000A_CHANNEL)(PS5000A_CHANNEL_A + toupper(name[0]) - 'A');
  }});
  options.push_back({"--threshold", "<volts>: digital logic threshold (default 1.65)", [&] () -> void {
    logic_threshold_volts = stod(nextArg());
    if (fabs(logic_threshold_volts) > 5)
      throw std::runtime_error("Logic threshold must be within -5 to 5 V");
  }});
  options.push_back({"--samples", "<n>: number of samples to capture (default: until Ctrl-C)", [&] () -> void {
    n_total_samples = stol(nextArg());
    if (n_total_samples < 0)
      throw std::runtime_error("Invalid sample count");
  }});
  options.push_back({"--duration", "<seconds>: capture duration (alternative to --samples)", [&] () -> void {
    duration_seconds = stod(nextArg());
    if (duration_seconds <= 0)
      throw std::runtime_error("Invalid duration");
  }});
  options.push_back({"--stop-on-signal-loss", "<seconds>: auto-stop when the analog signal stays silent this long (default: off)", [&] () -> void {
    signal_loss_timeout = stod(nextArg());
    if (signal_loss_timeout <= 0)
      throw std::runtime_error("Signal loss timeout must be positive");
  }});
  options.push_back({"--signal-loss-threshold", "<frac>: silence level as a fraction of the running average (0-1, default 0.1)", [&] () -> void {
    signal_loss_threshold = stod(nextArg());
    if (signal_loss_threshold <= 0 || signal_loss_threshold >= 1)
      throw std::runtime_error("Signal loss threshold must be between 0 and 1 (exclusive)");
  }});
  options.push_back({"--buffers", "<n>: number of 1 MiB write buffers absorbing disk-write stalls (default 64)", [&] () -> void {
    g_writeBuffers = stoi(nextArg());
    if (g_writeBuffers < 2)
      throw std::runtime_error("Need at least 2 write buffers");
  }});
  options.push_back({"--device-buffer-size", "<n>: driver circular buffer in samples (default 4000000; set low to test wrap detection)", [&] () -> void {
    g_deviceBufferSize = stoul(nextArg());
    if (g_deviceBufferSize < 1000)
      throw std::runtime_error("Device buffer size must be at least 1000 samples");
  }});
  options.push_back({"--help", "show this help", [&] () -> void {
    usage();
  }});

  try {
    while (it != args.cend()) {
      auto option = std::find_if(options.cbegin(), options.cend(),
				 [it] (const Option &o) -> bool { return *it == o.name; });
      if (option != options.cend()) {
	it++;
	option->handler();
      } else if (it->find("-", 0) == 0) {
	usage();
      } else if (!filename_given) {
	filename = *it++;
	filename_given = true;
      } else {
	usage();
      }
    }
  } catch (const std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    usage();
  }

  PS5000A_DEVICE_RESOLUTION resolution;
  if (g_digital) {
    if (bits == 0)
      bits = 8;
    if (bits != 8 && bits != 16) {
      fprintf(stderr, "--bits must be 8 or 16 in digital mode\n");
      exit(1);
    }
    g_digitalPorts = bits / 8;
    g_bytesPerSample = g_digitalPorts;
    resolution = PS5000A_DR_8BIT; // required for fastest sampling rates
  } else {
    if (bits == 0)
      bits = 12;
    switch (bits) {
      case 8:  resolution = PS5000A_DR_8BIT; break;
      case 12: resolution = PS5000A_DR_12BIT; break;
      case 14: resolution = PS5000A_DR_14BIT; break;
      case 15: resolution = PS5000A_DR_15BIT; break;
      case 16: resolution = PS5000A_DR_16BIT; break;
      default:
	fprintf(stderr, "--bits must be 8, 12, 14, 15 or 16 in analog mode\n");
	exit(1);
    }
    g_bytesPerSample = bits == 8 ? 1 : 2;
  }

  if (g_digital && signal_loss_timeout > 0) {
    fprintf(stderr, "--stop-on-signal-loss is only supported in analog mode\n");
    exit(1);
  }

  if (duration_seconds > 0)
    n_total_samples = llround(duration_seconds * 1e9 / sample_interval);

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
  CHECK_CALL(ps5000aGetUnitInfo(handle, (int8_t*)line, sizeof(line), &requiredSize, PICO_VARIANT_INFO), "ps5000aGetUnitInfo");

  int16_t channelCount = std::min<int16_t>((int16_t)(line[1] - '0'), maxChannels);

  CHECK_CALL(ps5000aSetEts(handle, PS5000A_ETS_OFF, 0, 0, NULL), "ps5000aSetEts"); // Turn off hasHardwareETS

  if (g_digital) {
    if (!strstr(line, "MSO")) {
      printf("Not MSO model, digital channels not supported.\n");
      exit(1);
    }

    printf("Unit has %d analog channels -- disabling all.\n", channelCount);
    for (int i = 0; i < channelCount; i++)
      CHECK_CALL(ps5000aSetChannel(handle, (PS5000A_CHANNEL)(PS5000A_CHANNEL_A + i),
				   0, // disable
				   PS5000A_DC,
				   PS5000A_1V,
				   0.0), // Analogue offset
		 "ps5000aSetChannel");

    int16_t logic_level = (int16_t)lround(logic_threshold_volts / 5.0 * 32767); // -32767 to 32767 corresponds to -5 to 5 V
    for (int i = 0; i < 2; i++)
      CHECK_CALL(ps5000aSetDigitalPort(handle, (PS5000A_CHANNEL)(i + PS5000A_DIGITAL_PORT0),
				       i < g_digitalPorts, logic_level),
		 "ps5000aSetDigitalPort");
    printf("Capturing %d digital channels, logic threshold %.2f V\n", bits, logic_threshold_volts);
  } else {
    printf("Unit has %d channels.\n", channelCount);
    if (g_channel - PS5000A_CHANNEL_A >= channelCount) {
      printf("Channel %c not available.\n", 'A' + (g_channel - PS5000A_CHANNEL_A));
      exit(1);
    }

    int16_t value;
    ps5000aMaximumValue(handle, &value);
    g_maxAdcValue = value;
    printf("Max ADC value=%d\n", value);

    float maximumVoltage;
    float minimumVoltage;
    CHECK_CALL(ps5000aGetAnalogueOffset(handle, range, coupling, &maximumVoltage, &minimumVoltage), "ps5000aGetAnalogueOffset");

    printf("Permitted analog offset range: %f-%f\n", minimumVoltage, maximumVoltage);

    if (strstr(line, "MSO"))
      for (int i = 0; i < 2; i++)
	CHECK_CALL(ps5000aSetDigitalPort(handle, (PS5000A_CHANNEL)(i + PS5000A_DIGITAL_PORT0), 0, 0), "ps5000aSetDigitalPort");

    for (int i = 0; i < channelCount; i++)
      CHECK_CALL(ps5000aSetChannel(handle, (PS5000A_CHANNEL)(PS5000A_CHANNEL_A + i),
				   PS5000A_CHANNEL_A + i == g_channel, // enable only the selected channel
				   coupling,
				   range,
				   0.0), // Analogue offset
		 "ps5000aSetChannel");
  }

  signal(SIGINT, sigintHandler);

  if (!g_digital && coupling == PS5000A_AC) {
    long samples_to_discard = 1000000000L / sample_interval; // 1s
    printf("Capturing %ld samples to /dev/null to let AC coupling capacitor settle\n", samples_to_discard);
    collectStreamingImmediate(handle, "/dev/null", sample_interval, samples_to_discard, nullptr, false);
  }

  SignalLossDetector *detector = nullptr;
  if (signal_loss_timeout > 0) {
    detector = new SignalLossDetector(signal_loss_timeout, signal_loss_threshold, g_bytesPerSample == 1);
    printf("Will auto-stop after %.3g s below %.3g x the running average signal level\n",
	   signal_loss_timeout, signal_loss_threshold);
  }

  if (!g_stop)
    collectStreamingImmediate(handle, filename.c_str(), sample_interval, n_total_samples, detector, !g_digital);

  delete detector;
  ps5000aCloseUnit(handle);
  return 0;
}
